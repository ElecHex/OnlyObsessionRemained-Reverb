#include "dsp/tanks/PlateTank.h"

#include <algorithm>
#include <cmath>

namespace
{
    // Dattorro《Effect Design Part 2》原文延迟长度，基准采样率 29761 Hz
    constexpr double kReferenceSampleRate = 29761.0;

    // 输入扩散：4 级级联全通，延迟互质，增益按原文分两组
    constexpr int   kInputApfDelay[4] = { 141, 107, 379, 277 };
    constexpr float kInputApfGain [4] = { 0.75f, 0.75f, 0.625f, 0.625f };

    // 左支路：APF1 → Delay1 → APF2 → Delay2
    constexpr int kLeftApf1Delay  = 672;
    constexpr int kLeftDelay1     = 4453;
    constexpr int kLeftApf2Delay  = 1800;
    constexpr int kLeftDelay2     = 3720;

    // 右支路：与左支路同为 figure-8 的镜像臂，延迟长度错开以去相关
    constexpr int kRightApf1Delay = 908;
    constexpr int kRightDelay1    = 4217;
    constexpr int kRightApf2Delay = 2656;
    constexpr int kRightDelay2    = 3163;

    constexpr float kTankApfGain = 0.5f;

    // 输出抽头偏移（Dattorro 原版输出矩阵的 7 个系数位置，符号在求和时给定）
    constexpr int kTapApf1Out  = 266;    // Delay1 近端
    constexpr int kTapDel1Far  = 2974;   // Delay1 远端
    constexpr int kTapApf2     = 1913;   // APF2 内部（偏移大于其延迟长度，需预留容量）
    constexpr int kTapDel2Far  = 1996;   // Delay2 远端
    constexpr int kTapCrossD1  = 1990;   // 跨支路：对侧 Delay1
    constexpr int kTapCrossAp2 = 187;    // 跨支路：对侧 APF2
    constexpr int kTapCrossD2  = 1066;   // 跨支路：对侧 Delay2

    // 绕 figure-8 一整圈（左支路 + 右支路）的总延迟，用于由 RT60 反推反馈增益。
    // 注意必须把两支路的全通延迟长度一并计入：全通虽是"零损耗"，但它的相位延迟
    // 构成回路的实际往返时间，漏算会让实测 RT60 系统性偏长约 1.39 倍。
    constexpr int kLoopLengthSamples = (kLeftApf1Delay  + kLeftDelay1
                                      + kLeftApf2Delay  + kLeftDelay2)
                                     + (kRightApf1Delay + kRightDelay1
                                      + kRightApf2Delay + kRightDelay2);

    // 腔体输出电平校准：以白噪声实测标定（offline 第 5 节）。
    // kOutputScale = 0.5 时，默认参数（Decay 0.30 / HiDamp 0.50 / LoDamp 0.20）
    // 下 wet RMS / dry RMS = 1.02，即 Mix=100% 的湿声与干声等响；
    // 长 decay 端因尾巴能量累积升到约 1.24（+1.8 dB），与真实混响行为一致。
    constexpr float kOutputScale = 0.5f;

    // 反馈点微饱和的驱动量：把交叉耦合点的信号电平换算到「湿声输出」刻度上，
    // 使 softSaturate 的 0.7 拐点统一表示「湿声峰值约 −3 dBFS」。
    // 实测（offline 第 22 节电平扫描）：交叉点峰值 ≈ 湿声峰值 × 0.28，
    // 取 5.0 后起弯点在输入峰值 0.5 附近（约 −6 dBFS），正常素材不染色。
    constexpr float kSatDrive = 5.0f;

    // 延迟容量余量，供 M3 的 LFO 调制偏移使用
    constexpr float kHeadroomScale = 1.08f;

    // M3 调制：四条长延迟线各自的 LFO 频率（Hz）与初始相位。
    // 频率直接采用 Dattorro 原规格的 1.0 / 1.5 / 1.2 / 1.8 Hz（相位均分 0/0.25/0.5/0.75
    // 使支路音高漂移去相关）。此前我们用的是 0.10–0.18 Hz，比论文慢约 4.7 倍——
    // 这正是"Mod 各档听起来差不多"的主因：调制深度再大，速率过慢也听不出差异。
    constexpr float kModLfoFrequency[4] = { 1.00f, 1.50f, 1.20f, 1.80f };
    constexpr float kModLfoPhase    [4] = { 0.00f,  0.25f,  0.50f,  0.75f  };

    // Mod=1 时的调制深度（基准采样率 29761 Hz 下的样本数），运行时按 srScale 缩放。
    // Dattorro 原规格的最大偏移为 ±16 样本，对应本参数下的 Mod = 40%，
    // 即"论文原样"是 Mod 40%；上限取 40 样本是为了让旋钮能覆盖到比论文更强的调制。
    constexpr float kMaxModDepthReference = 40.0f;

    // 预延迟上限（毫秒），参数范围与之对应；延迟线按此上限一次性分配
    constexpr float kMaxPreDelayMs = 500.0f;

    // 反馈路径固定直流阻断的截止频率（与 Room/Spring 一致）。
    // 混合式阻尼在 damp=0 时完全透明，直流必须另有阻断，否则会在回路里累积。
    constexpr float kDcBlockHz = 15.0f;

    int toSamples (int referenceSamples, float srScale)
    {
        return std::max (1, static_cast<int> (std::lround (static_cast<double> (referenceSamples)
                                                           * static_cast<double> (srScale))));
    }

    int capacityFor (int referenceSamples, float srScale)
    {
        return static_cast<int> (std::lround (static_cast<double> (referenceSamples)
                                              * static_cast<double> (srScale)
                                              * static_cast<double> (kHeadroomScale))) + 4;
    }

    // 抽头偏移也需按采样率缩放，否则非 29761 Hz 下抽头位置会错位
    int tapSamples (int referenceSamples, float srScale)
    {
        return toSamples (referenceSamples, srScale);
    }
}

void PlateTank::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    srScale    = static_cast<float> (sampleRate / kReferenceSampleRate);

    for (int i = 0; i < 4; ++i)
    {
        inputAllpass[i].prepare (capacityFor (kInputApfDelay[i], srScale));
        inputAllpass[i].setDelay (toSamples (kInputApfDelay[i], srScale));
        inputAllpass[i].setGain  (kInputApfGain[i]);
    }

    leftApf1 .prepare (capacityFor (kLeftApf1Delay,  srScale));
    leftApf1 .setDelay (toSamples (kLeftApf1Delay,  srScale));
    leftApf1 .setGain  (kTankApfGain);

    rightApf1.prepare (capacityFor (kRightApf1Delay, srScale));
    rightApf1.setDelay (toSamples (kRightApf1Delay, srScale));
    rightApf1.setGain  (kTankApfGain);

    // APF2 的内部延迟线要能读到 kTapApf2，该偏移大于左支路全通自身的延迟长度
    const int apf2Capacity = capacityFor (std::max (kLeftApf2Delay, kTapApf2), srScale);

    leftApf2 .prepare (apf2Capacity);
    leftApf2 .setDelay (toSamples (kLeftApf2Delay,  srScale));
    leftApf2 .setGain  (kTankApfGain);

    rightApf2.prepare (capacityFor (std::max (kRightApf2Delay, kTapApf2), srScale));
    rightApf2.setDelay (toSamples (kRightApf2Delay, srScale));
    rightApf2.setGain  (kTankApfGain);

    leftDelay1 .prepare (capacityFor (kLeftDelay1,  srScale));
    leftDelay2 .prepare (capacityFor (kLeftDelay2,  srScale));
    rightDelay1.prepare (capacityFor (kRightDelay1, srScale));
    rightDelay2.prepare (capacityFor (kRightDelay2, srScale));

    leftDelay1Samples  = toSamples (kLeftDelay1,  srScale);
    leftDelay2Samples  = toSamples (kLeftDelay2,  srScale);
    rightDelay1Samples = toSamples (kRightDelay1, srScale);
    rightDelay2Samples = toSamples (kRightDelay2, srScale);

    lfoLeftDelay1 .prepare (sampleRate, kModLfoFrequency[0], kModLfoPhase[0]);
    lfoLeftDelay2 .prepare (sampleRate, kModLfoFrequency[1], kModLfoPhase[1]);
    lfoRightDelay1.prepare (sampleRate, kModLfoFrequency[2], kModLfoPhase[2]);
    lfoRightDelay2.prepare (sampleRate, kModLfoFrequency[3], kModLfoPhase[3]);

    // 预延迟按上限一次性分配（process 内零分配）；read 会折叠到 length-1，故多留几样本
    preDelayLine.prepare (static_cast<int> (std::lround (kMaxPreDelayMs * 0.001
                                                         * sampleRate)) + 4);

    setHighDamping (0.0f);
    setLowDamping  (0.0f);
    modDepthSamples = 0.0f;
    setPreDelay (preDelayMs);

    dcBlockL.setCutoff (kDcBlockHz, sampleRate);
    dcBlockR.setCutoff (kDcBlockHz, sampleRate);

    updateFeedbackGain();
    reset();
}

void PlateTank::reset()
{
    preDelayLine.reset();

    for (auto& ap : inputAllpass)
        ap.reset();

    leftApf1 .reset();
    leftApf2 .reset();
    rightApf1.reset();
    rightApf2.reset();

    leftDelay1 .reset();
    leftDelay2 .reset();
    rightDelay1.reset();
    rightDelay2.reset();

    highDampL.reset();
    highDampR.reset();
    lowDampL .reset();
    lowDampR .reset();
    dcBlockL .reset();
    dcBlockR .reset();

    lfoLeftDelay1 .reset();
    lfoLeftDelay2 .reset();
    lfoRightDelay1.reset();
    lfoRightDelay2.reset();

    toLeft  = 0.0f;
    toRight = 0.0f;
}

void PlateTank::setDecay (float decay01) noexcept
{
    decay01 = std::clamp (decay01, 0.0f, 1.0f);

    // 指数映射：0 → 约 0.25 s，1 → 约 30 s
    rt60 = 0.25f * std::pow (120.0f, decay01);

    updateFeedbackGain();
}

void PlateTank::setHighDamping (float highDamp01) noexcept
{
    highDamp01 = std::clamp (highDamp01, 0.0f, 1.0f);
    highDampAmount = highDamp01;

    // 0 → 几乎不阻尼（约 20 kHz）；1 → 强阻尼（约 1 kHz）
    const float cutoff = 20000.0f * std::pow (0.05f, highDamp01);

    highDampL.setCutoff (cutoff, sampleRate);
    highDampR.setCutoff (cutoff, sampleRate);
}

void PlateTank::setLowDamping (float lowDamp01) noexcept
{
    lowDamp01 = std::clamp (lowDamp01, 0.0f, 1.0f);
    lowDampAmount = lowDamp01;

    // 0 → 高通截止极低（约 20 Hz，基本不衰减低频）；1 → 低频明显更快衰减（约 2 kHz）
    const float cutoff = 20.0f * std::pow (100.0f, lowDamp01);

    lowDampL.setCutoff (cutoff, sampleRate);
    lowDampR.setCutoff (cutoff, sampleRate);
}

void PlateTank::setModulation (float mod01) noexcept
{
    mod01 = std::clamp (mod01, 0.0f, 1.0f);

    // Mod=0 时深度为 0，分数延迟读取退化为整数读取，输出与 M2 完全一致
    modDepthSamples = kMaxModDepthReference * srScale * mod01;
}

void PlateTank::setPreDelay (float ms) noexcept
{
    preDelayMs = std::clamp (ms, 0.0f, kMaxPreDelayMs);

    // 取分数样本：引擎层对 preDelay 做了 40 ms 平滑，整数取整会让读指针逐块跳变
    preDelaySamplesF = static_cast<float> (preDelayMs * 0.001 * sampleRate);
    preDelayActive   = preDelayMs > 0.0f;
}

void PlateTank::updateFeedbackGain() noexcept
{
    // 每条支路上 decayGain 出现两次（APF2 输入、支路输出），因此绕一圈共四次：
    //     (decayGain^4) ^ (RT60 * fs / L) = 10^-3
    //  => decayGain = 10 ^ (-0.75 * L / (RT60 * fs))
    const float loopSamples = static_cast<float> (kLoopLengthSamples) * srScale;

    decayGain = std::pow (10.0f, -0.75f * loopSamples
                                       / (rt60 * static_cast<float> (sampleRate)));
}

void PlateTank::process (const float* inL, const float* inR,
                         float* outL, float* outR, int numSamples) noexcept
{
    const int tapApf1Out  = tapSamples (kTapApf1Out,  srScale);
    const int tapDel1Far  = tapSamples (kTapDel1Far,  srScale);
    const int tapApf2     = tapSamples (kTapApf2,     srScale);
    const int tapDel2Far  = tapSamples (kTapDel2Far,  srScale);
    const int tapCrossD1  = tapSamples (kTapCrossD1,  srScale);
    const int tapCrossAp2 = tapSamples (kTapCrossAp2, srScale);
    const int tapCrossD2  = tapSamples (kTapCrossD2,  srScale);

    for (int n = 0; n < numSamples; ++n)
    {
        // M3：四条长延迟线各自的调制偏移（样本）。Mod=0 时为 0，
        // 分数延迟读取退化为整数读取，与 M2 逐比特一致。
        const float modL1 = modDepthSamples * lfoLeftDelay1 .process();
        const float modL2 = modDepthSamples * lfoLeftDelay2 .process();
        const float modR1 = modDepthSamples * lfoRightDelay1.process();
        const float modR2 = modDepthSamples * lfoRightDelay2.process();

        // 腔体激励取单声道和：左右分支共享激励，靠不对称延迟结构自然去相关
        float x = 0.5f * (inL[n] + inR[n]);

        // M4：预延迟。始终写入以保持延迟线内容连续，PreDelay=0 时跳过读取（逐比特直通）。
        // DelayLine::read 在写入后读取，其 D 实际对应 D-1 样本延迟，故 +1 补偿使预延迟
        // 精确等于设定的毫秒数（腔体内部延迟沿用原约定，不受影响）。
        preDelayLine.write (x);

        if (preDelayActive)
            x = preDelayLine.readFractional (preDelaySamplesF + 1.0f);

        for (auto& ap : inputAllpass)
            x = ap.process (x);

        // ---- 左支路 ----
        const float lApf1Out = leftApf1.process (x + toLeft);
        leftDelay1.write (lApf1Out);
        const float lDel1 = leftDelay1.readFractional (static_cast<float> (leftDelay1Samples) + modL1);

        // 混合式阻尼（与 Room/Spring 同一口径）：k=0 严格透明，k=1 为完整滤波。
        // 抽头仍取阻尼前，保持湿声明亮。之后再过固定直流阻断。
        const float lHi = highDampL.process (lDel1);
        const float lHd = lDel1 + highDampAmount * (lHi - lDel1);

        const float lLo = lowDampL.process (lHd);
        const float lDamped = dcBlockL.process (lHd + lowDampAmount * (lLo - lHd));
        const float lApf2Out = leftApf2.process (lDamped * decayGain);
        leftDelay2.write (lApf2Out);
        const float lDel2 = leftDelay2.readFractional (static_cast<float> (leftDelay2Samples) + modL2);

        // 交叉耦合：左支路末端灌入右支路。写回前过一次微饱和——这是绕 figure-8
        // 一整圈的唯一收口点，因此每圈只染色一次，长尾会随电平缓慢"变暖"，
        // 同时把环路幅度钳在 2.0 以内（见 Saturator.h）。
        toRight = saturateFeedback (lDel2 * decayGain, kSatDrive);

        // ---- 右支路（与左支路对称）----
        const float rApf1Out = rightApf1.process (x + toRight);
        rightDelay1.write (rApf1Out);
        const float rDel1 = rightDelay1.readFractional (static_cast<float> (rightDelay1Samples) + modR1);

        const float rHi = highDampR.process (rDel1);
        const float rHd = rDel1 + highDampAmount * (rHi - rDel1);

        const float rLo = lowDampR.process (rHd);
        const float rDamped = dcBlockR.process (rHd + lowDampAmount * (rLo - rHd));
        const float rApf2Out = rightApf2.process (rDamped * decayGain);
        rightDelay2.write (rApf2Out);
        const float rDel2 = rightDelay2.readFractional (static_cast<float> (rightDelay2Samples) + modR2);

        toLeft = saturateFeedback (rDel2 * decayGain, kSatDrive);   // 交叉耦合：右支路末端灌入左支路

        // ---- 输出：7 路抽头带符号求和 ----
        // 本侧 4 路（APF1 输出、Delay1 远端、Delay2 远端取正；APF2 取负），
        // 跨支路 3 路全取负，形成宽立体声且避免同相叠加导致的梳状抵消。
        // 抽头与反馈共用同一条延迟线，因此带上相同的调制偏移（等效于读写头整体移动）；
        // APF2 的抽头不受调制影响（短延迟线不参与调制）。
        const float wetL = lApf1Out
                         + leftDelay1.readFractional (static_cast<float> (tapApf1Out) + modL1)
                         + leftDelay1.readFractional (static_cast<float> (tapDel1Far) + modL1)
                         - leftApf2.readDelayTap (tapApf2)
                         + leftDelay2.readFractional (static_cast<float> (tapDel2Far) + modL2)
                         - rightDelay1.readFractional (static_cast<float> (tapCrossD1) + modR1)
                         - rightApf2.readDelayTap (tapCrossAp2)
                         - rightDelay2.readFractional (static_cast<float> (tapCrossD2) + modR2);

        const float wetR = rApf1Out
                         + rightDelay1.readFractional (static_cast<float> (tapApf1Out) + modR1)
                         + rightDelay1.readFractional (static_cast<float> (tapDel1Far) + modR1)
                         - rightApf2.readDelayTap (tapApf2)
                         + rightDelay2.readFractional (static_cast<float> (tapDel2Far) + modR2)
                         - leftDelay1.readFractional (static_cast<float> (tapCrossD1) + modL1)
                         - leftApf2.readDelayTap (tapCrossAp2)
                         - leftDelay2.readFractional (static_cast<float> (tapCrossD2) + modL2);

        outL[n] = wetL * kOutputScale;
        outR[n] = wetR * kOutputScale;
    }
}
