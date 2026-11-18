#include "dsp/tanks/RoomTank.h"

#include <algorithm>
#include <cmath>

namespace
{
    // 与 PlateTank 相同的基准采样率，便于两套腔体的延迟长度互相参照
    constexpr double kReferenceSampleRate = 29761.0;

    // 输入扩散：4 级级联全通（延迟互质，增益分两组）
    constexpr int   kInputApfDelay[4] = { 131, 173, 257, 337 };
    constexpr float kInputApfGain [4] = { 0.70f, 0.70f, 0.62f, 0.62f };

    // FDN 八条延迟线长度（基准采样率下的样本数，均为质数，两两互质）。
    //
    // 历史：原表均值约 1486 样本（≈ 50 ms），总长 Σm 在 48 kHz 下只有 0.399 s。
    // 按 Smith 的模态密度下界（Σm ≥ 0.15·RT60·fs），这只够支撑到 RT60 ≈ 1.6 s；
    // Decay 0.8 的名义 RT60 是 11.5 s，长尾因此明显偏金属、偏稀疏。
    //
    // 现表整体放大约 2.25 倍（仍全部取质数），Σm ≈ 26754 基准样本 ≈ 0.90 s @48 kHz，
    // 可支撑到 RT60 ≈ 6 s。代价是平均自由程从 50 ms 变成约 112 ms——Room 的
    // 空间感会从"小房间"变成"大厅"，这是为长 RT60 付的必然代价。
    constexpr int kLineDelay[RoomTank::kNumLines] = {
        1949, 2297, 2729, 3109, 3529, 3943, 4397, 4801
    };

    // 调制：每线一路慢速三角波 LFO，频率互不整除、相位在 0–1 间均分八份，
    // 使各延迟线的音高漂移彼此去相关，把静态梳状结构打散成自然的起伏。
    constexpr float kModLfoFrequency[RoomTank::kNumLines] = {
        0.24f, 0.31f, 0.35f, 0.42f, 0.52f, 0.56f, 0.67f, 0.74f
    };
    constexpr float kModLfoPhase[RoomTank::kNumLines] = {
        0.000f, 0.125f, 0.250f, 0.375f, 0.500f, 0.625f, 0.750f, 0.875f
    };

    // Mod=1 时的调制深度（基准采样率下的样本数），运行时按 srScale 缩放。
    // 由 16 提到 40，与 Plate 同步，使 Mod 各档差异明确可辨。
    constexpr float kMaxModDepthReference = 40.0f;

    // 激励注入各延迟线的符号：交替取号，避免八条线初始收到同相激励而产生
    // 一次性的强同相爆发。
    constexpr float kInjectSign[RoomTank::kNumLines] = {
        +1.0f, -1.0f, +1.0f, -1.0f, +1.0f, -1.0f, +1.0f, -1.0f
    };

    // 输出权重：两组互相正交的 ±1 符号模式（Hadamard 行），乘 1/√N 归一化。
    // 正交保证左右声道从各延迟线取到不同的线性组合，立体声自然去相关。
    constexpr float kOutSignL[RoomTank::kNumLines] = {
        +1.0f, -1.0f, +1.0f, -1.0f, +1.0f, -1.0f, +1.0f, -1.0f
    };
    constexpr float kOutSignR[RoomTank::kNumLines] = {
        +1.0f, +1.0f, -1.0f, -1.0f, +1.0f, +1.0f, -1.0f, -1.0f
    };

    constexpr float kWeightNorm   = 0.35355339f;   // 1 / √8
    constexpr float kInjectScale  = 0.35355339f;   // 1 / √8

    // 腔体输出电平校准：以白噪声实测标定（offline 第 12 节）。
    // 延迟线加长约 2.25 倍后，同样的注入电平下稳态湿声变小（实测 wet/dry 从 0.999
    // 掉到 0.872），因此把 2.22 上调到 2.55，使默认参数（Decay 0.30 / HiDamp 0.50 /
    // LoDamp 0.20）下 wet/dry 仍 ≈ 1.0。长 decay 端因尾巴能量累积会略高于 1。
    constexpr float kOutputScale = 2.55f;

    // 反馈点微饱和的驱动量：把 Householder 混合前的 fb 电平换算到「湿声输出」刻度上，
    // 使 softSaturate 的 0.7 拐点统一表示「湿声峰值约 −3 dBFS」。
    // 实测（offline 第 22 节电平扫描）：fb 峰值 ≈ 湿声峰值 × 0.5，
    // 取 2.8 后起弯点在输入峰值 0.5 附近（约 −6 dBFS），正常素材不染色。
    constexpr float kSatDrive = 2.8f;

    // 延迟容量余量，供调制偏移使用
    constexpr float kHeadroomScale = 1.08f;

    // 预延迟上限（毫秒），与参数范围一致
    constexpr float kMaxPreDelayMs = 500.0f;

    // 反馈路径固定直流阻断的截止频率。取得很低（15 Hz）以免影响可闻低频，
    // 但直流增益严格为 0，杜绝直流在回路里累积。
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
}

void RoomTank::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    srScale    = static_cast<float> (sampleRate / kReferenceSampleRate);

    for (int i = 0; i < 4; ++i)
    {
        inputAllpass[i].prepare (capacityFor (kInputApfDelay[i], srScale));
        inputAllpass[i].setDelay (toSamples (kInputApfDelay[i], srScale));
        inputAllpass[i].setGain  (kInputApfGain[i]);
    }

    for (int i = 0; i < kNumLines; ++i)
    {
        lineSamples[i] = toSamples (kLineDelay[i], srScale);
        line[i].prepare (capacityFor (kLineDelay[i], srScale));
        lfo[i].prepare (sampleRate, kModLfoFrequency[i], kModLfoPhase[i]);
        dcBlock[i].setCutoff (kDcBlockHz, sampleRate);
    }

    // 预延迟按上限一次性分配（process 内零分配）
    preDelayLine.prepare (static_cast<int> (std::lround (kMaxPreDelayMs * 0.001
                                                         * sampleRate)) + 4);

    setHighDamping (0.0f);
    setLowDamping  (0.0f);
    modDepthSamples = 0.0f;
    setPreDelay (preDelayMs);
    updateLoopGains();
    reset();
}

void RoomTank::reset()
{
    preDelayLine.reset();

    for (auto& ap : inputAllpass)
        ap.reset();

    for (int i = 0; i < kNumLines; ++i)
    {
        line[i].reset();
        highDampFilter[i].reset();
        lowDampFilter[i].reset();
        dcBlock[i].reset();
        lfo[i].reset();
    }
}

void RoomTank::setDecay (float decay01) noexcept
{
    decay01 = std::clamp (decay01, 0.0f, 1.0f);

    // 与 PlateTank 相同的指数映射：0 → 约 0.25 s，1 → 约 30 s
    rt60 = 0.25f * std::pow (120.0f, decay01);

    updateLoopGains();
}

void RoomTank::setHighDamping (float highDamp01) noexcept
{
    highDamp01 = std::clamp (highDamp01, 0.0f, 1.0f);
    highDampAmount = highDamp01;

    // 与 PlateTank 一致的截止映射：0 → 约 20 kHz；1 → 约 1 kHz。
    // 实际阻尼强度由 highDampAmount 的混合比例决定，0 时完全透明。
    const float cutoff = 20000.0f * std::pow (0.05f, highDamp01);

    for (auto& f : highDampFilter)
        f.setCutoff (cutoff, sampleRate);
}

void RoomTank::setLowDamping (float lowDamp01) noexcept
{
    lowDamp01 = std::clamp (lowDamp01, 0.0f, 1.0f);
    lowDampAmount = lowDamp01;

    const float cutoff = 20.0f * std::pow (100.0f, lowDamp01);

    for (auto& f : lowDampFilter)
        f.setCutoff (cutoff, sampleRate);
}

void RoomTank::setModulation (float mod01) noexcept
{
    mod01 = std::clamp (mod01, 0.0f, 1.0f);

    // Mod=0 时深度为 0，分数延迟读取退化为整数读取，输出与静止腔体一致
    modDepthSamples = kMaxModDepthReference * srScale * mod01;
}

void RoomTank::setPreDelay (float ms) noexcept
{
    preDelayMs = std::clamp (ms, 0.0f, kMaxPreDelayMs);

    // 分数样本：引擎层对 preDelay 做 40 ms 平滑，整数取整会让读指针逐块跳变
    preDelaySamplesF = static_cast<float> (preDelayMs * 0.001 * sampleRate);
    preDelayActive   = preDelayMs > 0.0f;
}

void RoomTank::updateLoopGains() noexcept
{
    // 单线环路：信号绕行 d 个样本后振幅乘 g。要求绕行 RT60 秒后衰减到 -60 dB：
    //     g ^ (RT60 * fs / d) = 10^-3
    //  => g = 10 ^ (-3 * d / (RT60 * fs))
    // 逐线按各自延迟长度取 g，八条线的衰减速率因此严格一致（不像统一 g 那样
    // 让短延迟线先衰减、长延迟线后衰减，导致尾巴由长线主导而发浑）。
    for (int i = 0; i < kNumLines; ++i)
    {
        if (lineSamples[i] <= 0)
            continue;

        lineGain[i] = std::pow (10.0f, -3.0f * static_cast<float> (lineSamples[i])
                                          / (rt60 * static_cast<float> (sampleRate)));
    }
}

void RoomTank::process (const float* inL, const float* inR,
                        float* outL, float* outR, int numSamples) noexcept
{
    for (int n = 0; n < numSamples; ++n)
    {
        // 激励取单声道和；预延迟先读后写，读取值恰为 preDelaySamples 个样本之前。
        const float xRaw = 0.5f * (inL[n] + inR[n]);

        float x = xRaw;
        if (preDelayActive)
            x = preDelayLine.readFractional (preDelaySamplesF);

        preDelayLine.write (xRaw);

        for (auto& ap : inputAllpass)
            x = ap.process (x);

        // ---- 读延迟线 → 阻尼 → 衰减 ----
        float tap[kNumLines];
        float fb [kNumLines];

        for (int i = 0; i < kNumLines; ++i)
        {
            const float mod = modDepthSamples * lfo[i].process();
            const float d   = line[i].readFractional (static_cast<float> (lineSamples[i]) + mod);

            tap[i] = d;   // 输出抽头取阻尼前的节点，保持湿声明亮

            // 混合式阻尼：0 时严格透明（d 原样通过），1 时为完整滤波
            const float hi = highDampFilter[i].process (d);
            const float hd = d + highDampAmount * (hi - d);

            const float lo = lowDampFilter[i].process (hd);
            const float ld = hd + lowDampAmount * (lo - hd);

            // 固定直流阻断（不依赖用户参数），杜绝直流在反馈回路里逐圈累积；
            // 再过一次微饱和后才参与混合——放在 Householder 混合**之前**，
            // 使矩阵仍作用在一个有界的向量上（正交性不被幅度发散破坏）。
            fb[i] = saturateFeedback (dcBlock[i].process (ld) * lineGain[i], kSatDrive);
        }

        // ---- Householder 无损混合：y = x - (2/N)·Σx ----
        // 该矩阵正交（H² = I），不损失能量，把各线的信号均匀重新分配。
        float sum = 0.0f;
        for (int i = 0; i < kNumLines; ++i)
            sum += fb[i];

        const float mixScale = 2.0f / static_cast<float> (kNumLines);

        for (int i = 0; i < kNumLines; ++i)
            line[i].write ((fb[i] - mixScale * sum) + x * kInjectScale * kInjectSign[i]);

        // ---- 输出：两组正交符号加权，得到去相关的立体声 ----
        float wetL = 0.0f, wetR = 0.0f;
        for (int i = 0; i < kNumLines; ++i)
        {
            wetL += kOutSignL[i] * tap[i];
            wetR += kOutSignR[i] * tap[i];
        }

        outL[n] = wetL * kWeightNorm * kOutputScale;
        outR[n] = wetR * kWeightNorm * kOutputScale;
    }
}