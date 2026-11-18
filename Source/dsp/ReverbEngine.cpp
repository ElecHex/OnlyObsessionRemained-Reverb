#include "dsp/ReverbEngine.h"

#include <cmath>

namespace
{
    // Shimmer 环路增益上限。湿声升八度后回灌腔体输入，构成反馈环：
    //     环路增益 = shimmerGain × 腔体「输入→湿声」增益
    // 实测腔体对持续素材的增益随 Decay 上升且与类型相关（Decay 0.8 时
    // Plate 约 1.7、Room 约 2.8、Spring 约 2.2）。原先 0.85 的上限在
    // shimmer=100 / decay=0.6 时环路增益约 1.008，14 s 内峰值自激到 2.3e4。
    // 现在配合回灌低切（斩断直流/次低频堆积，见 shimmerHpf），上限取 0.50：
    // Decay 0.2–0.9 全范围内（含 Room）实测峰值均 < 1，留有余量。
    constexpr float kMaxShimmerFeedback = 0.50f;

    // Shimmer 回灌的低切 / 柔化低通截止频率（Hz）。
    // 低通为两级级联（二阶）的 5 kHz，是音色塑形而非抗混叠（抗混叠见 PitchShifter
    // 内部的 2× 过采样 + 半带滤波）。
    constexpr float kShimmerFeedbackHighpassHz = 120.0f;
    constexpr float kShimmerFeedbackLowpassHz  = 5000.0f;

    // 反馈环兜底软限幅：|x| ≤ 1 时完全透明，超出后渐近压到 2。
    // 正常电平（湿声峰值约 0.8 × 上限 0.50 ≈ 0.4）低于拐点、不产生任何染色；
    // 仅在因未知工况（异常素材、宿主参数跳变）导致环路增益意外越过 1 时，
    // 把幅度钳住而不是指数发散。
    inline float softLimit (float x) noexcept
    {
        const float a = std::abs (x);

        if (a <= 1.0f)
            return x;

        return (x > 0.0f ? 1.0f : -1.0f) * (2.0f - 1.0f / a);
    }

    // Decay → 湿声电平的实测漂移曲线（听感标定 round1，粉噪 burst，Decay 0.2→0.9）。
    // 湿声 RMS 相对 Decay=0.2 的比值近似为二次曲线 1 + a·u + b·u²（u = decay - 0.2）：
    //   Decay 0.2/0.4/0.5/0.7/0.8/0.9 → 1.000/1.061/1.134/1.376/1.543/1.741
    // 用户反馈"升 Decay 时响度应基本不变"，故取该曲线的倒数做补偿。
    constexpr float kDecayLevelSlope = -0.0123f;
    constexpr float kDecayLevelCurve =  1.5300f;
}

ReverbEngine::ReverbEngine() = default;

void ReverbEngine::prepare (double sampleRate, int maximumBlockSize, int numChannels)
{
    sr       = sampleRate > 0.0 ? sampleRate : 44100.0;
    maxBlock = juce::jmax (1, maximumBlockSize);
    channels = juce::jlimit (1, 2, numChannels);

    // 一次性分配，process 内零分配
    wetBuffer    .setSize (2, maxBlock, false, true, true);
    gainRamp     .setSize (2, maxBlock, false, true, true);
    inputBuffer  .setSize (2, maxBlock, false, true, true);
    shimmerBuffer.setSize (2, maxBlock, false, true, true);
    fadeBuffer   .setSize (2, maxBlock, false, true, true);

    fadeIncrement = 1.0f / (kTypeCrossfadeSeconds * static_cast<float> (juce::jmax (1.0, sr)));

    plateTank.prepare (sr);
    plateTank.setPreDelay (params.preDelayMs);

    // 各腔体在 prepare 时同时就绪，切换 Type 不需重新分配或重建
    roomTank.prepare (sr);
    roomTank.setPreDelay (params.preDelayMs);

    springTank.prepare (sr);
    springTank.setPreDelay (params.preDelayMs);

    pitchShifterL.prepare (sr);
    pitchShifterR.prepare (sr);

    shimmerHpfL.setCutoff (kShimmerFeedbackHighpassHz, sr);
    shimmerHpfR.setCutoff (kShimmerFeedbackHighpassHz, sr);
    shimmerLpf1L.setCutoff (kShimmerFeedbackLowpassHz, sr);
    shimmerLpf1R.setCutoff (kShimmerFeedbackLowpassHz, sr);
    shimmerLpf2L.setCutoff (kShimmerFeedbackLowpassHz, sr);
    shimmerLpf2R.setCutoff (kShimmerFeedbackLowpassHz, sr);

    constexpr double gainRampSeconds = 0.02;
    dryGain.reset (sr, gainRampSeconds);
    wetGain.reset (sr, gainRampSeconds);

    constexpr double paramRampSeconds = 0.04;
    smoothedHighDamp .reset (sr, paramRampSeconds);
    smoothedLowDamp  .reset (sr, paramRampSeconds);
    smoothedMod      .reset (sr, paramRampSeconds);
    smoothedPreDelay .reset (sr, paramRampSeconds);

    smoothedHighDamp .setCurrentAndTargetValue (params.highDamp);
    smoothedLowDamp  .setCurrentAndTargetValue (params.lowDamp);
    smoothedMod      .setCurrentAndTargetValue (params.mod);
    smoothedPreDelay .setCurrentAndTargetValue (params.preDelayMs);

    reset();
}

void ReverbEngine::reset()
{
    wetBuffer    .clear();
    gainRamp     .clear();
    inputBuffer  .clear();
    shimmerBuffer.clear();

    plateTank .reset();
    roomTank  .reset();
    springTank.reset();

    activeType   = params.type;
    fadingType   = params.type;
    crossfading  = false;
    typeInitialised = false;
    hasPendingType  = false;
    fadeProgress = 1.0f;

    pitchShifterL.reset();
    pitchShifterR.reset();

    shimmerHpfL.reset();
    shimmerHpfR.reset();
    shimmerLpf1L.reset();
    shimmerLpf1R.reset();
    shimmerLpf2L.reset();
    shimmerLpf2R.reset();

    // 初始化即对齐目标值，避免启动时出现淡入
    dryGain.setCurrentAndTargetValue (computeDryGain());
    wetGain.setCurrentAndTargetValue (computeWetGain());
}

void ReverbEngine::setParameters (const Parameters& newParameters) noexcept
{
    params = newParameters;

    // 切 Type：新腔体先清空（避免从上次使用时的陈旧尾音恢复），旧腔体继续跑
    // 120 ms 等功率淡出。淡变中途又切换时排队，等这一段结束后再开始下一段——
    // 否则要在同一瞬间丢掉正在淡出的腔体，那一下本身就是一次硬切。
    // 首次收到参数（启动/加载会话）直接落位——否则开头 120 ms 会从静默淡入。
    if (! typeInitialised)
    {
        typeInitialised = true;
        activeType      = params.type;
        fadingType      = params.type;
        crossfading     = false;
        hasPendingType  = false;
        resetTank (activeType);
    }
    else if (params.type != activeType)
    {
        if (crossfading)
        {
            pendingType    = params.type;
            hasPendingType = true;
        }
        else
        {
            beginCrossfade (params.type);
        }
    }

    dryGain.setTargetValue (computeDryGain());
    wetGain.setTargetValue (computeWetGain());

    shimmerGain = juce::jlimit (0.0f, 1.0f, params.shimmer) * kMaxShimmerFeedback
                * computeWetLevelCompensation();

    // 三个腔体的参数不在这里直接推：先设平滑目标，由 process() 每块推进一次后
    // 再推给腔体。否则宿主每块改一次阻尼/预延迟就会让滤波器系数与延迟读指针跳变。
    smoothedHighDamp .setTargetValue (params.highDamp);
    smoothedLowDamp  .setTargetValue (params.lowDamp);
    smoothedMod      .setTargetValue (params.mod);
    smoothedPreDelay .setTargetValue (params.preDelayMs);
}

void ReverbEngine::process (juce::AudioBuffer<float>& buffer)
{
    juce::ScopedNoDenormals noDenormals;

    const int totalSamples = buffer.getNumSamples();
    const int numChannels  = juce::jmin (buffer.getNumChannels(), channels);

    if (totalSamples <= 0 || numChannels <= 0)
        return;

    // 100% 干声且平滑已就位：彻底旁通，保证逐比特直通（M0 的 null test 依据）
    if (params.mix <= 0.0f && ! dryGain.isSmoothing())
    {
        updateWetLevel (0.0f, totalSamples);   // 旁通时也让电平包络自然衰减
        return;
    }

    // 平滑参数每块推进一次，推给三个腔体；再做切块处理。
    applySmoothedParameters (totalSamples);

    // 按预分配的最大块长切块。超长块不再被整块丢弃：每一段都真正处理，
    // 输出始终是"湿声正确混合"的结果，而不是悄悄变成干声直通。
    for (int offset = 0; offset < totalSamples; offset += maxBlock)
    {
        const int n = juce::jmin (maxBlock, totalSamples - offset);
        processRange (buffer, offset, n, numChannels);
    }
}

void ReverbEngine::applySmoothedParameters (int numSamples) noexcept
{
    smoothedHighDamp .skip (numSamples);
    smoothedLowDamp  .skip (numSamples);
    smoothedMod      .skip (numSamples);
    smoothedPreDelay .skip (numSamples);

    const float h = smoothedHighDamp .getCurrentValue();
    const float l = smoothedLowDamp  .getCurrentValue();
    const float m = smoothedMod      .getCurrentValue();
    const float d = smoothedPreDelay .getCurrentValue();

    // 只推活动腔体（淡变中再推一份给正在淡出的旧腔体）。非活动腔体不再每块
    // 重算 updateLoopGains（含 8 次 pow）与滤波器系数——它们的状态在切回时
    // 会被 reset，不需要保持"随时同步"。
    auto push = [this] (ReverbType type, float dec, float hi, float lo,
                        float modDepth, float pre) noexcept
    {
        switch (type)
        {
            case ReverbType::plate:
                plateTank.setDecay (dec);
                plateTank.setHighDamping (hi);
                plateTank.setLowDamping (lo);
                plateTank.setModulation (modDepth);
                plateTank.setPreDelay (pre);
                break;

            case ReverbType::room:
                roomTank.setDecay (dec);
                roomTank.setHighDamping (hi);
                roomTank.setLowDamping (lo);
                roomTank.setModulation (modDepth);
                roomTank.setPreDelay (pre);
                break;

            case ReverbType::spring:
                springTank.setDecay (dec);
                springTank.setHighDamping (hi);
                springTank.setLowDamping (lo);
                springTank.setModulation (modDepth);
                springTank.setPreDelay (pre);
                break;
        }
    };

    push (activeType, params.decay, h, l, m, d);

    if (crossfading)
        push (fadingType, params.decay, h, l, m, d);
}

void ReverbEngine::processTank (ReverbType type, const float* inL, const float* inR,
                                float* outL, float* outR, int numSamples) noexcept
{
    switch (type)
    {
        case ReverbType::plate:  plateTank .process (inL, inR, outL, outR, numSamples); break;
        case ReverbType::room:   roomTank  .process (inL, inR, outL, outR, numSamples); break;
        case ReverbType::spring: springTank.process (inL, inR, outL, outR, numSamples); break;
    }
}

void ReverbEngine::beginCrossfade (ReverbType next) noexcept
{
    fadingType   = activeType;
    activeType   = next;

    resetTank (activeType);   // 新腔体从静默起步，不继承上次使用时的状态

    crossfading  = true;
    fadeProgress = 0.0f;
}

void ReverbEngine::resetTank (ReverbType type) noexcept
{
    switch (type)
    {
        case ReverbType::plate:  plateTank .reset(); break;
        case ReverbType::room:   roomTank  .reset(); break;
        case ReverbType::spring: springTank.reset(); break;
    }
}

void ReverbEngine::processRange (juce::AudioBuffer<float>& buffer, int startSample,
                                 int numSamples, int numChannels) noexcept
{
    if (numSamples <= 0 || numChannels <= 0)
        return;

    // ---- 湿路 ----
    // 干声同时作为腔体激励；Mix 归零时跳过，避免无谓运算与状态漂移。
    if (params.mix > 0.0f || wetGain.isSmoothing())
    {
        // 腔体输入 = 干声 + 上一块湿声的八度移调回灌（Shimmer），写入独立的 inputBuffer，
        // 不污染 buffer 中的干声（干湿混合阶段还要用）。
        auto* inL = inputBuffer.getWritePointer (0);
        auto* inR = inputBuffer.getWritePointer (1);

        const float* dryL = buffer.getReadPointer (0, startSample);
        const float* dryR = numChannels > 1 ? buffer.getReadPointer (1, startSample) : dryL;

        const bool shimmerActive = shimmerGain > 0.0f;

        if (shimmerActive)
        {
            const float* shimL = shimmerBuffer.getReadPointer (0);
            const float* shimR = shimmerBuffer.getReadPointer (1);

            for (int i = 0; i < numSamples; ++i)
            {
                inL[i] = dryL[i] + shimL[i];
                inR[i] = dryR[i] + shimR[i];
            }
        }
        else
        {
            for (int i = 0; i < numSamples; ++i)
            {
                inL[i] = dryL[i];
                inR[i] = dryR[i];
            }
        }

        auto* wetL = wetBuffer.getWritePointer (0);
        auto* wetR = wetBuffer.getWritePointer (1);

        processTank (activeType, inL, inR, wetL, wetR, numSamples);

        if (crossfading)
        {
            auto* oldL = fadeBuffer.getWritePointer (0);
            auto* oldR = fadeBuffer.getWritePointer (1);

            processTank (fadingType, inL, inR, oldL, oldR, numSamples);

            // 等功率淡变（sin/cos）：两个混响输出互不相关，线性相加会在淡变中段
            // 掉电平。淡变期间两个腔体同时运行，结束即清空旧腔体，不留陈旧尾音。
            for (int i = 0; i < numSamples; ++i)
            {
                const float p    = fadeProgress;
                const float wIn  = std::sin (juce::MathConstants<float>::halfPi * p);
                const float wOut = std::cos (juce::MathConstants<float>::halfPi * p);

                wetL[i] = wetL[i] * wIn + oldL[i] * wOut;
                wetR[i] = wetR[i] * wIn + oldR[i] * wOut;

                fadeProgress = juce::jmin (1.0f, fadeProgress + fadeIncrement);
            }

            if (fadeProgress >= 1.0f)
            {
                crossfading = false;
                resetTank (fadingType);   // 连同旧腔体的尾音一起清空

                // 淡变期间排队的切换：这一段结束后再开始下一段
                if (hasPendingType)
                {
                    hasPendingType = false;

                    if (pendingType != activeType)
                        beginCrossfade (pendingType);
                }
            }
        }

        // 统计湿声块 RMS 并推进电平包络（供动态着色器的 pLevel）
        {
            double sumSquares = 0.0;

            for (int i = 0; i < numSamples; ++i)
            {
                const double l = wetL[i];
                const double r = wetR[i];
                sumSquares += l * l + r * r;
            }

            updateWetLevel (static_cast<float> (std::sqrt (sumSquares / (2.0 * numSamples))),
                            numSamples);
        }

        // 为下一块准备 Shimmer 回灌：本块湿声升八度后乘以环路增益。
        // 块级反馈只引入一块延迟，对混响尾巴可忽略。
        if (shimmerActive)
        {
            auto* nextL = shimmerBuffer.getWritePointer (0);
            auto* nextR = shimmerBuffer.getWritePointer (1);

            for (int i = 0; i < numSamples; ++i)
            {
                // 回灌前先低切（斩直流）、再过两级柔化低通，然后才升八度
                // （抗混叠在 PitchShifter 内部完成，这里不再需要为折叠点做滤波）
                const float wl = shimmerLpf2L.process (shimmerLpf1L.process (shimmerHpfL.process (wetL[i])));
                const float wr = shimmerLpf2R.process (shimmerLpf1R.process (shimmerHpfR.process (wetR[i])));

                nextL[i] = softLimit (pitchShifterL.process (wl) * shimmerGain);
                nextR[i] = softLimit (pitchShifterR.process (wr) * shimmerGain);
            }
        }
        else
        {
            // 关闭时清零，避免下次开启瞬间灌入过期信号
            shimmerBuffer.clear (0, numSamples);
        }
    }

    // ---- 干湿混合 ----
    auto* dryRamp = gainRamp.getWritePointer (0);
    auto* wetRamp = gainRamp.getWritePointer (1);

    for (int i = 0; i < numSamples; ++i)
    {
        dryRamp[i] = dryGain.getNextValue();
        wetRamp[i] = wetGain.getNextValue();
    }

    for (int ch = 0; ch < numChannels; ++ch)
    {
        auto* data    = buffer  .getWritePointer (ch, startSample);
        auto* wetData = wetBuffer.getReadPointer  (ch);

        for (int i = 0; i < numSamples; ++i)
            data[i] = data[i] * dryRamp[i] + wetData[i] * wetRamp[i];
    }
}

void ReverbEngine::updateWetLevel (float blockRms, int numSamples) noexcept
{
    // 一阶释放包络：release = exp(-块时长 / 时间常数)。取 RMS_max 而非平滑上升，
    // 让电平随混响尾巴起落有"呼吸"感，而不是拖成一条缓慢的直线。
    constexpr float tau = 0.1f;

    const float blockSeconds = static_cast<float> (numSamples) / static_cast<float> (sr);
    const float release      = std::exp (-blockSeconds / tau);

    wetLevelEnvelope = juce::jmax (blockRms, wetLevelEnvelope * release);

    wetLevel.store (juce::jlimit (0.0f, 1.0f, wetLevelEnvelope), std::memory_order_relaxed);
}

float ReverbEngine::computeDryGain() const noexcept
{
    // 等功率混合律（听感标定 round1 选定）：√(1-mix) / √mix。
    // 线性律在 50% Mix 时整体比直通低约 2.5 dB，等功率则与直通基本持平，
    // 更接近多数混响效果器的行为。
    return std::sqrt (juce::jmax (0.0f, 1.0f - params.mix));
}

float ReverbEngine::computeWetLevelCompensation() const noexcept
{
    const float u     = params.decay - 0.2f;
    const float level = 1.0f + kDecayLevelSlope * u + kDecayLevelCurve * u * u;

    // 上限 1.0：不做额外提升；下限 0.5 防止极端 Decay 下过度压低
    return juce::jlimit (0.5f, 1.0f, 1.0f / juce::jmax (0.05f, level));
}

float ReverbEngine::computeWetGain() const noexcept
{
    // 补偿 Decay → 湿声电平漂移，使升 Decay 时响度基本不变
    return std::sqrt (juce::jmax (0.0f, params.mix)) * computeWetLevelCompensation();
}