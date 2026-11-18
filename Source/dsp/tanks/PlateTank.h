#pragma once

#include "dsp/core/AllpassFilter.h"
#include "dsp/core/DelayLine.h"
#include "dsp/core/LFO.h"
#include "dsp/core/OnePoleFilter.h"
#include "dsp/core/Saturator.h"

/**
    Dattorro figure-8 板式混响腔体（M1 拓扑 / M2 阻尼与标定 / M3 调制）。

    拓扑严格对齐 Dattorro《Effect Design Part 2》原版 figure-8（并经 ValleyAudio
    Plateau 实现交叉核对），延迟长度基准采样率 29761 Hz，运行时等比缩放。

    单条支路的信号流：

        输入 → APF1 → Delay1 → [High Damp 低通 → Low Damp 高通] → ×decay → APF2 → Delay2 → ×decay → 灌入对侧

        左支路：APF 672, Delay 4453, APF 1800, Delay 3720
        右支路：APF 908, Delay 4217, APF 2656, Delay 3163

    关键点：阻尼只作用于反馈路径，而输出抽头取自阻尼**前**的节点，
    因此湿声高频比反馈回路更亮，这是板式混响"通透不闷"的来源。

    M2 的两处修正：
      - RT60 反推增益时，回路长度必须计入两支路的全通相位延迟（否则实测 RT60
        系统性偏长约 1.39 倍）；阻尼改为混合式后实测 vs 名义为 1.08–1.18
        （Damp 全关；短 RT60 端滤波器的额外损失消失，比值比旧版略高）。
      - 阻尼为「低通 + 高通」的混合式（y = x + k·(filtered − x)），High Damp 与
        Low Damp 分别控制高频与低频的衰减速度；k=0 时严格透明，与 Room/Spring
        同一口径，因此 Damp=0 的 RT60 可以跨类型比较。

    M3 的调制：
      - 四条长延迟线各配一路慢速三角波 LFO（约 0.1–0.18 Hz，相位 0/0.25/0.5/0.75），
        延迟读取改走分数延迟，等效于读写头整体轻微漂移，打散静态梳状结构；
      - 调制深度 ∝ Mod 参数，Mod=0 时深度为 0，分数延迟退化为整数读取。

    M4 的预延迟：
      - 激励在进入输入扩散前先经过一条单声道延迟线（0–500 ms），
        使湿声整体后移而干声不受影响；PreDelay=0 时信号逐比特直通。

    输出为 7 路抽头带符号求和（左 4 路本侧 + 3 路跨支路，右通道镜像），
    而不是只取支路末端——后者会让湿声延迟一整圈才出现，产生明显回声感。
*/
class PlateTank
{
public:
    void prepare (double sampleRate);
    void reset();

    /** decay 0..1 → RT60。 */
    void setDecay (float decay01) noexcept;

    /** highDamp 0..1 → 反馈回路内低通截止频率（高频衰减更快）。 */
    void setHighDamping (float highDamp01) noexcept;

    /** lowDamp 0..1 → 反馈回路内高通截止频率（低频衰减更快）。 */
    void setLowDamping (float lowDamp01) noexcept;

    /** mod 0..1 → 四条长延迟线的 LFO 调制深度（0 = 完全静止）。 */
    void setModulation (float mod01) noexcept;

    /** preDelayMs 0..500 → 激励进入腔体前的纯延迟（毫秒）。0 时不改变信号。 */
    void setPreDelay (float ms) noexcept;

    void process (const float* inL, const float* inR,
                  float* outL, float* outR, int numSamples) noexcept;

private:
    void updateFeedbackGain() noexcept;

    double sampleRate = 48000.0;
    float  srScale    = 1.0f;

    // 输入扩散（单声道链，4 级级联全通）
    AllpassFilter inputAllpass[4];

    // 预延迟：激励送入输入扩散前先延迟一段（0–500 ms）。
    // 激励本身是左右声道之和的单声道信号，因此单条延迟线即可。
    DelayLine preDelayLine;
    float     preDelayMs       = 20.0f;
    float     preDelaySamplesF = 0.0f;   // 分数样本，供平滑时的无跳变读取
    bool      preDelayActive   = false;

    // 左支路
    AllpassFilter leftApf1, leftApf2;
    DelayLine     leftDelay1, leftDelay2;
    int           leftDelay1Samples = 0;
    int           leftDelay2Samples = 0;

    // 右支路
    AllpassFilter rightApf1, rightApf2;
    DelayLine     rightDelay1, rightDelay2;
    int           rightDelay1Samples = 0;
    int           rightDelay2Samples = 0;

    // 反馈回路内阻尼：低通（High Damp）+ 高通（Low Damp），只作用于反馈路径，
    // 输出抽头仍取阻尼前的节点。与 Room/Spring 一样采用混合式（见下）。
    OnePoleLowpass  highDampL,  highDampR;
    OnePoleHighpass lowDampL,   lowDampR;

    // 混合式阻尼的重量（与 Room/Spring 同一口径）：y = x + k·(filtered − x)。
    // k=0 时严格透明，k=1 时等于直接串联滤波。三腔体统一后，Damp=0 在所有
    // 类型上都表示"零阻尼"，RT60 才能跨模式比较。
    float highDampAmount = 0.0f;
    float lowDampAmount  = 0.0f;

    // 固定直流阻断。阻尼改成混合式后，lowDamp=0 时低通/高通完全透明，
    // 反馈回路必须自己保证直流增益为 0（否则直流会按 1/(1-g) 累积）。
    OnePoleHighpass dcBlockL, dcBlockR;

    // M3 调制：四条长延迟线各配一路慢速三角波 LFO，频率互不整除、相位错开，
    // 使各支路的音高漂移去相关，打散静态梳状结构带来的金属味
    TriangleLFO lfoLeftDelay1, lfoLeftDelay2, lfoRightDelay1, lfoRightDelay2;

    // 当前调制深度（样本），由 Mod 参数与采样率换算
    float modDepthSamples = 0.0f;

    // 交叉耦合状态：由对侧支路末端回灌进来的信号
    float toLeft  = 0.0f;
    float toRight = 0.0f;

    float rt60      = 2.0f;
    float decayGain = 0.5f;
};
