#pragma once

#include "dsp/core/AllpassFilter.h"
#include "dsp/core/DelayLine.h"
#include "dsp/core/LFO.h"
#include "dsp/core/OnePoleFilter.h"
#include "dsp/core/Saturator.h"

/**
    FDN（反馈延迟网络）房间混响腔体（M5）。

    与 PlateTank 的 Dattorro figure-8 不同，Room 采用 8 条并行反馈延迟线 +
    Householder 无损混合矩阵：每条延迟线读出的信号经阻尼与衰减后，由一个正交矩阵
    重新分配到所有延迟线。正交矩阵不损失能量，等效于回声密度随每次混合迅速增长，
    比 figure-8 的"双支路交叉"更接近真实房间的密度增长曲线，听感上更自然铺开、
    不出现明显的金属共振。

    单条延迟线的环路：

        读取 → High Damp 低通 → Low Damp 高通 → ×decayGain → Householder 混合 → 写回(+ 激励)

    其中 decayGain 逐线按各自延迟长度单独标定，使所有延迟线在 RT60 时刻同时衰减
    到 -60 dB（见 updateLoopGains），避免长短线衰减速率不一致拖尾发浑。

    激励路径：单声道和 → 预延迟 → 4 级全通扩散 → 按交替符号注入各延迟线。

    输出：取各延迟线阻尼**前**的抽头，按两组正交符号模式加权求和，得到去相关的
    立体声（与 PlateTank 一致，湿声比反馈回路更亮）。

    M5 复用 Plate 已有的全部核心件，但环路信号流位置与 Plate 不同，因此阻尼、预延迟、
    调制等逻辑在本类内独立实现，不与 PlateTank 共享代码。
*/
class RoomTank
{
public:
    /** FDN 延迟线条数。8 线在模态密度与 CPU 之间取得平衡，尾巴比 4/6 线更平滑。 */
    static constexpr int kNumLines = 8;

    void prepare (double sampleRate);
    void reset();

    /** decay 0..1 → RT60（与 Plate 共用同一名义映射，保证 Decay 手感跨模式一致）。 */
    void setDecay (float decay01) noexcept;

    /** highDamp 0..1 → 反馈回路内低通截止频率（高频衰减更快）。 */
    void setHighDamping (float highDamp01) noexcept;

    /** lowDamp 0..1 → 反馈回路内高通截止频率（低频衰减更快）。 */
    void setLowDamping (float lowDamp01) noexcept;

    /** mod 0..1 → 各延迟线读指针的 LFO 调制深度（0 = 完全静止）。 */
    void setModulation (float mod01) noexcept;

    /** preDelayMs 0..500 → 激励进入扩散网络前的纯延迟（毫秒）。0 时不改变信号。 */
    void setPreDelay (float ms) noexcept;

    void process (const float* inL, const float* inR,
                  float* outL, float* outR, int numSamples) noexcept;

private:
    void updateLoopGains() noexcept;

    double sampleRate = 48000.0;
    float  srScale    = 1.0f;

    // 输入扩散（单声道链，4 级级联全通）
    AllpassFilter inputAllpass[4];

    // 预延迟：激励送入扩散前先延迟一段（0–500 ms）
    DelayLine preDelayLine;
    float     preDelayMs       = 20.0f;
    float     preDelaySamplesF = 0.0f;   // 分数样本，供平滑时的无跳变读取
    bool      preDelayActive   = false;

    // FDN 延迟线本体
    DelayLine line[kNumLines];
    int       lineSamples[kNumLines] = {};   // 当前采样率下的延时样本数
    float     lineGain   [kNumLines] = {};   // 逐线标定的反馈增益

    // 反馈回路内阻尼：每线一组「低通 + 高通」，仅作用于反馈路径。
    // 采用「阻尼量」混合而非直接串联滤波：fdn 的每条延迟线每个往返都过一次阻尼，
    // 圈数远多于 Plate，若直接串联，即使截止频率拉满也会累加可观的每圈损失、
    // 使 RT60 系统性偏短。改为 y = x + k·(lp(x) - x)：k=0 时严格透明（无任何损失），
    // k=1 时为完整低通，k 即 High/Low Damp 参数本身。
    OnePoleLowpass  highDampFilter[kNumLines];
    OnePoleHighpass lowDampFilter [kNumLines];
    float           highDampAmount = 0.0f;
    float           lowDampAmount  = 0.0f;

    // 固定直流阻断。反馈路径必须自带，不能依赖用户参数：阻尼是「混合式」，
    // lowDamp=0 时高通完全透明，而 FDN 在长 RT60 下的直流增益为 1/(1-g)（实测约 22），
    // 任何直流偏置都会在回路里逐圈累积（这正是 Shimmer 反馈环此前自激的根因之一）。
    OnePoleHighpass dcBlock[kNumLines];

    // 调制：每线一路慢速三角波 LFO，频率互不整除、相位均分，使各线漂移去相关
    TriangleLFO lfo[kNumLines];
    float       modDepthSamples = 0.0f;

    float rt60 = 2.0f;
};