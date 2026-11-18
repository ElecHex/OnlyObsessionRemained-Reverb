#include "dsp/tanks/SpringTank.h"

#include <algorithm>
#include <cmath>

namespace
{
    // 与 Plate/Room 相同的基准采样率
    constexpr double kReferenceSampleRate = 29761.0;

    // 色散链：每条弹簧 48 级级联全通，延迟取 2–7 的小整数并错开排列。
    //
    // 延迟必须足够小：单级全通的群延迟 τ(ω) = D(1-g²)/(1-2g·cos(ωD)+g²) 只在
    // ωD < π 的区间单调下降（低频慢、高频快），越过 π 后转为振荡，色散方向会混乱。
    // 若取 D=17–97，则 300 Hz 处 ωD 已跨过 π，实测高频反而更晚到；改成 D=2–7 后，
    // 直到约 3 kHz 都保持单调，色散方向稳定。
    //
    // 单级色散量很小（≈ D·4g/(1-g²)），需较多级数累积出可闻的啁啾；级数取 48，
    // 直通项 g^48 已可忽略，不会在响应开头留下咔哒。
    constexpr int kDispersionDelay[SpringTank::kNumSprings][SpringTank::kNumDispersion] = {
        { 2, 3, 5, 7, 4, 6, 3, 7,   2, 4, 6, 3, 5, 7, 2, 6,
          3, 5, 7, 4, 6, 2, 7, 3,   5, 4, 6, 2, 7, 3, 5, 6,
          4, 7, 2, 5, 6, 3, 7, 4,   2, 6, 5, 3, 7, 4, 6, 2 },
        { 3, 4, 6, 2, 7, 5, 4, 5,   3, 6, 4, 7, 2, 5, 3, 7,
          4, 6, 2, 5, 7, 3, 6, 4,   5, 3, 7, 4, 6, 2, 5, 7,
          6, 3, 5, 7, 2, 4, 6, 5,   3, 7, 4, 2, 6, 5, 3, 4 }
    };

    // 全通增益越高，单级色散量 4g/(1-g²) 越大、共振越尖锐、鸣响越长。
    // 听感标定 round1 反馈"弹簧应有那种嗡嗡的回声"，故由 0.70 提到 0.78：
    // 单级色散量约提升 1.45 倍，啁啾更长、模态峰更突出，更接近真实弹簧的"啵/嗡"。
    constexpr float kDispersionGain = 0.78f;

    // 回授延迟线长度（基准采样率），左右错开以去相关
    constexpr int kLoopDelay[SpringTank::kNumSprings] = { 1601, 1699 };

    // 调制：弹簧的自然颤动。频率略高于板/房间，深度较浅，只做轻微音高漂移。
    // 听感标定 round3 反馈"Mod 每一档区别不大"，深度由 12 提到 28。
    constexpr float kLfoFrequency[SpringTank::kNumSprings] = { 0.65f, 0.90f };
    constexpr float kLfoPhase    [SpringTank::kNumSprings] = { 0.00f, 0.50f };

    // Mod=1 时的调制深度（基准采样率下的样本数）
    constexpr float kMaxModDepthReference = 28.0f;

    // 预延迟上限（毫秒）
    constexpr float kMaxPreDelayMs = 500.0f;

    // 反馈路径固定直流阻断的截止频率（直流增益严格为 0）
    constexpr float kDcBlockHz = 15.0f;

    // 延迟容量余量
    constexpr float kHeadroomScale = 1.08f;

    // 腔体输出电平校准：以白噪声实测标定（offline 第 18 节）。
    // 0.87 时默认参数（Decay 0.30 / HiDamp 0.50 / LoDamp 0.20）下 wet/dry ≈ 1.00。
    constexpr float kOutputScale = 0.87f;

    // 反馈点微饱和的驱动量：把回授点电平换算到「湿声输出」刻度上，
    // 使 softSaturate 的 0.7 拐点统一表示「湿声峰值约 −3 dBFS」。
    // 实测（offline 第 22 节电平扫描）：回授点峰值 ≈ 湿声峰值 × 1.0（色散链能量集中），
    // 取 1.4 后起弯点在输入峰值 0.5 附近（约 −6 dBFS），正常素材不染色。
    constexpr float kSatDrive = 1.4f;

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

void SpringTank::prepare (double newSampleRate)
{
    sampleRate = newSampleRate > 0.0 ? newSampleRate : 48000.0;
    srScale    = static_cast<float> (sampleRate / kReferenceSampleRate);

    for (int c = 0; c < kNumSprings; ++c)
    {
        int dispersionSum = 0;

        for (int k = 0; k < kNumDispersion; ++k)
        {
            dispersionDelay[c][k] = toSamples (kDispersionDelay[c][k], srScale);

            dispersion[c][k].prepare (capacityFor (kDispersionDelay[c][k], srScale));
            dispersion[c][k].setDelay (dispersionDelay[c][k]);
            dispersion[c][k].setGain  (kDispersionGain);

            dispersionSum += dispersionDelay[c][k];
        }

        loopDelaySamples[c] = toSamples (kLoopDelay[c], srScale);
        loopDelay[c].prepare (capacityFor (kLoopDelay[c], srScale));

        // 回路等效长度取「回授延迟 + 色散链延迟之和」：全通群延迟对频率取平均恰好等于
        // 其原始延迟 D（∫dω/(1-2g·cosωD+g²) 的平均为 1/(1-g²)），因此原始延迟和就是
        // 宽带的等效回路长度；若改用低频群延迟 D(1+g)/(1-g) 会系统性高估、使 RT60 偏短。
        loopLengthSamples[c] = loopDelaySamples[c] + dispersionSum;

        lfo[c].prepare (sampleRate, kLfoFrequency[c], kLfoPhase[c]);
        dcBlock[c].setCutoff (kDcBlockHz, sampleRate);
    }

    preDelayLine.prepare (static_cast<int> (std::lround (kMaxPreDelayMs * 0.001
                                                         * sampleRate)) + 4);

    setHighDamping (0.0f);
    setLowDamping  (0.0f);
    modDepthSamples = 0.0f;
    setPreDelay (preDelayMs);
    updateLoopGains();
    reset();
}

void SpringTank::reset()
{
    preDelayLine.reset();

    for (int c = 0; c < kNumSprings; ++c)
    {
        for (int k = 0; k < kNumDispersion; ++k)
            dispersion[c][k].reset();

        loopDelay[c].reset();
        highDampFilter[c].reset();
        lowDampFilter [c].reset();
        dcBlock[c].reset();
        lfo[c].reset();

        feedback[c] = 0.0f;
    }
}

void SpringTank::setDecay (float decay01) noexcept
{
    decay01 = std::clamp (decay01, 0.0f, 1.0f);

    rt60 = 0.25f * std::pow (120.0f, decay01);

    updateLoopGains();
}

void SpringTank::setHighDamping (float highDamp01) noexcept
{
    highDamp01 = std::clamp (highDamp01, 0.0f, 1.0f);
    highDampAmount = highDamp01;

    const float cutoff = 20000.0f * std::pow (0.05f, highDamp01);

    for (auto& f : highDampFilter)
        f.setCutoff (cutoff, sampleRate);
}

void SpringTank::setLowDamping (float lowDamp01) noexcept
{
    lowDamp01 = std::clamp (lowDamp01, 0.0f, 1.0f);
    lowDampAmount = lowDamp01;

    const float cutoff = 20.0f * std::pow (100.0f, lowDamp01);

    for (auto& f : lowDampFilter)
        f.setCutoff (cutoff, sampleRate);
}

void SpringTank::setModulation (float mod01) noexcept
{
    mod01 = std::clamp (mod01, 0.0f, 1.0f);
    modDepthSamples = kMaxModDepthReference * srScale * mod01;
}

void SpringTank::setPreDelay (float ms) noexcept
{
    preDelayMs = std::clamp (ms, 0.0f, kMaxPreDelayMs);

    // 分数样本：引擎层对 preDelay 做 40 ms 平滑，整数取整会让读指针逐块跳变
    preDelaySamplesF = static_cast<float> (preDelayMs * 0.001 * sampleRate);
    preDelayActive   = preDelayMs > 0.0f;
}

void SpringTank::updateLoopGains() noexcept
{
    // 每绕一圈回授一次，回路长度即 loopLengthSamples：
    //     g ^ (RT60 * fs / L) = 10^-3  =>  g = 10 ^ (-3 * L / (RT60 * fs))
    for (int c = 0; c < kNumSprings; ++c)
    {
        if (loopLengthSamples[c] <= 0)
            continue;

        decayGain[c] = std::pow (10.0f, -3.0f * static_cast<float> (loopLengthSamples[c])
                                           / (rt60 * static_cast<float> (sampleRate)));
    }
}

void SpringTank::process (const float* inL, const float* inR,
                          float* outL, float* outR, int numSamples) noexcept
{
    for (int n = 0; n < numSamples; ++n)
    {
        // 预延迟先读后写：读取值恰为 preDelaySamples 个样本之前
        const float xRaw = 0.5f * (inL[n] + inR[n]);

        float x = xRaw;
        if (preDelayActive)
            x = preDelayLine.readFractional (preDelaySamplesF);

        preDelayLine.write (xRaw);

        for (int c = 0; c < kNumSprings; ++c)
        {
            // 色散链在回路内部：每绕一圈多累积一次色散
            float v = x + feedback[c];

            for (int k = 0; k < kNumDispersion; ++k)
                v = dispersion[c][k].process (v);

            loopDelay[c].write (v);

            const float mod = modDepthSamples * lfo[c].process();
            const float d   = loopDelay[c].readFractional (static_cast<float> (loopDelaySamples[c]) + mod);

            // 混合式阻尼：0 时严格透明
            const float hi = highDampFilter[c].process (d);
            const float hd = d + highDampAmount * (hi - d);

            const float lo = lowDampFilter[c].process (hd);
            const float ld = hd + lowDampAmount * (lo - hd);

            // 固定直流阻断（不依赖用户参数），杜绝直流在反馈回路里逐圈累积；
            // 回授写入前过一次微饱和，给弹簧的长尾染上一层温和的谐波
            feedback[c] = saturateFeedback (dcBlock[c].process (ld) * decayGain[c], kSatDrive);

            if (c == 0)
                outL[n] = d * kOutputScale;
            else
                outR[n] = d * kOutputScale;
        }
    }
}