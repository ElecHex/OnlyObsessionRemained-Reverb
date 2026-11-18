#pragma once

#include "dsp/core/AllpassFilter.h"
#include "dsp/core/DelayLine.h"
#include "dsp/core/LFO.h"
#include "dsp/core/OnePoleFilter.h"
#include "dsp/core/Saturator.h"

/**
    弹簧混响腔体（M6）。

    与 Plate（figure-8）、Room（FDN）都不同，Spring 的核心特征是**色散**：
    信号在弹簧里传播时高频比低频快，脉冲被"抹开"成一段由高到低的啁啾（chirp），
    听感上就是弹簧特有的"啵/震颤"。实现方式：

        激励 → 48 级级联全通（色散链，制造频率相关群延迟）→ 回授延迟线 →
               混合式阻尼 → ×decayGain → 回灌色散链输入

    色散链放在反馈回路**内部**：信号每绕一圈就多累积一次色散，因此尾巴随圈数
    逐渐"拉开"，与真实弹簧来回传播的物理过程一致。

    两条弹簧（左右各一）使用互不相同的全通延迟与回授延迟长度，天然去相关，
    直接给出立体声，无需额外抽取矩阵。

    阻尼沿用 RoomTank 的「阻尼量混合」形式 y = x + k·(lp(x) - x)，k=0 时严格透明，
    便于 RT60 标定不被每圈滤波损失污染。

    预延迟为单声道，置于两条弹簧之前。
*/
class SpringTank
{
public:
    static constexpr int kNumSprings    = 2;   // 左右两条弹簧
    static constexpr int kNumDispersion = 48;  // 每条弹簧的色散全通级数

    void prepare (double sampleRate);
    void reset();

    /** decay 0..1 → RT60（与 Plate/Room 共用同一名义映射）。 */
    void setDecay (float decay01) noexcept;

    /** highDamp 0..1 → 回授回路内低通（高频衰减更快）。 */
    void setHighDamping (float highDamp01) noexcept;

    /** lowDamp 0..1 → 回授回路内高通（低频衰减更快）。 */
    void setLowDamping (float lowDamp01) noexcept;

    /** mod 0..1 → 回授延迟线读指针的 LFO 调制深度（弹簧的自然颤动）。 */
    void setModulation (float mod01) noexcept;

    /** preDelayMs 0..500 → 激励进入弹簧前的纯延迟（毫秒）。0 时不改变信号。 */
    void setPreDelay (float ms) noexcept;

    void process (const float* inL, const float* inR,
                  float* outL, float* outR, int numSamples) noexcept;

private:
    void updateLoopGains() noexcept;

    double sampleRate = 48000.0;
    float  srScale    = 1.0f;

    // 预延迟（单声道）
    DelayLine preDelayLine;
    float     preDelayMs       = 20.0f;
    float     preDelaySamplesF = 0.0f;   // 分数样本，供平滑时的无跳变读取
    bool      preDelayActive   = false;

    // 色散链：每条弹簧一组级联全通
    AllpassFilter dispersion[kNumSprings][kNumDispersion];
    int           dispersionDelay[kNumSprings][kNumDispersion] = {};

    // 回授延迟线
    DelayLine loopDelay[kNumSprings];
    int       loopDelaySamples [kNumSprings] = {};
    int       loopLengthSamples[kNumSprings] = {};   // 含色散全通相位延迟，用于 RT60 反推

    // 混合式阻尼（每弹簧一组）
    OnePoleLowpass  highDampFilter[kNumSprings];
    OnePoleHighpass lowDampFilter [kNumSprings];
    float           highDampAmount = 0.0f;
    float           lowDampAmount  = 0.0f;

    // 固定直流阻断。反馈路径必须自带，不能依赖用户参数：阻尼是「混合式」，
    // lowDamp=0 时高通完全透明，而回路在长 RT60 下的直流增益为 1/(1-g)（实测约 22），
    // 任何直流偏置都会在回路里逐圈累积。
    OnePoleHighpass dcBlock[kNumSprings];

    // 调制
    TriangleLFO lfo[kNumSprings];
    float       modDepthSamples = 0.0f;

    // 回授状态与增益
    float feedback  [kNumSprings] = {};
    float decayGain [kNumSprings] = {};

    float rt60 = 2.0f;
};