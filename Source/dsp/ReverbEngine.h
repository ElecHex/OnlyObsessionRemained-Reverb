#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_core/juce_core.h>

#include <atomic>

#include "dsp/core/OnePoleFilter.h"
#include "dsp/core/PitchShifter.h"
#include "dsp/tanks/PlateTank.h"
#include "dsp/tanks/RoomTank.h"
#include "dsp/tanks/SpringTank.h"

/**
    混响引擎顶层调度器。

    架构思路参考了经典硬件混响的公开设计（Dattorro figure-8 板式、FDN 房间、
    弹簧色散等），但并非对某一具体产品的复刻。

    对外接口刻意保持稳定：M1/M2/M3 都只在内部接入新的 DSP 模块，不改变调用方代码。

    当前进度：
      - 干路：零处理直通，Mix=0 时逐比特等于输入（模拟旁通）
      - M1：Dattorro figure-8 Plate 腔体写入 wetBuffer（7 路抽头立体声输出）
      - M2：反馈回路内 High/Low Damp 滤波器组 + Decay → RT60 增益标定
      - M3：四条长延迟线多相位 LFO 调制（分数延迟）
      - M4：湿路预延迟（preDelayMs 0–500 ms），干路不受影响
      - M5：Room 类型（独立 RoomTank，8 线 FDN + Householder 无损混合）
      - M6：Spring 类型（色散链弹簧）+ Shimmer（湿声升八度回灌腔体，对各类型通用）
      - M7（听感标定）：等功率 Mix 律 + Decay → 湿声电平补偿
      - M8（算法深化）：移调器改 2× 过采样 + 半带抗混叠；三腔体反馈写入前加微饱和
*/
class ReverbEngine
{
public:
    enum class ReverbType
    {
        plate = 0,
        room,
        spring
    };

    struct Parameters
    {
        ReverbType type   = ReverbType::plate;
        float mix         = 0.0f;    // 0..1
        float decay       = 0.4f;    // 0..1
        float preDelayMs  = 20.0f;   // 0..500
        float mod         = 0.5f;    // 0..1（默认 50%，听感标定 round4 确定）
        float highDamp    = 0.5f;    // 0..1
        float lowDamp     = 0.2f;    // 0..1
        float shimmer     = 0.0f;    // 0..1
    };

    ReverbEngine();

    /** 在 prepareToPlay 中调用；内部完成全部缓冲分配，process 内不得再分配。 */
    void prepare (double sampleRate, int maximumBlockSize, int numChannels);

    /** 清空所有内部状态（换预设、复位等场景）。 */
    void reset();

    /** 由音频线程每块调用一次，只做原子读取与目标值设置，不做重计算。 */
    void setParameters (const Parameters& newParameters) noexcept;

    /** 就地处理；buffer 为干湿混合后的最终输出。 */
    void process (juce::AudioBuffer<float>& buffer);

    /** 湿声输出电平（0…1，带约 100 ms 释放包络）。音频线程写、界面线程读，故用原子发布。 */
    float getWetLevel() const noexcept { return wetLevel.load (std::memory_order_relaxed); }

private:
    /**
        process 的切块入口：保证每块 ≤ maxBlock。

        宿主（含离线渲染）偶尔会送来超过 prepare 时最大块长的块；过去这里直接
        return，会让整块变成"未处理的干声直通"——不崩，但静默出错。
    */
    void processRange (juce::AudioBuffer<float>& buffer, int startSample,
                       int numSamples, int numChannels) noexcept;

    /** 把平滑后的参数推给三个腔体（每块一次）。 */
    void applySmoothedParameters (int numSamples) noexcept;

    /** 把输入跑进指定腔体。 */
    void processTank (ReverbType type, const float* inL, const float* inR,
                      float* outL, float* outR, int numSamples) noexcept;

    /** 清空指定腔体的内部状态。 */
    void resetTank (ReverbType type) noexcept;

    /** 从当前腔体向 next 开始一次等功率淡变（next 先清空）。 */
    void beginCrossfade (ReverbType next) noexcept;

    float computeDryGain() const noexcept;
    float computeWetGain() const noexcept;

    /** Decay → 湿声电平漂移的补偿增益（也用于约束 Shimmer 环路增益）。 */
    float computeWetLevelCompensation() const noexcept;

    double sr         = 44100.0;
    int    maxBlock   = 512;
    int    channels   = 2;

    Parameters params;

    PlateTank  plateTank;                 // M1：Plate 腔体
    RoomTank   roomTank;                  // M5：Room 腔体（FDN）
    SpringTank springTank;                // M6：Spring 腔体（色散链）

    juce::AudioBuffer<float> wetBuffer;   // 湿路输出暂存，始终双声道（腔体为立体声）
    juce::AudioBuffer<float> gainRamp;    // 每块的干/湿增益斜坡，避免多声道重复推进平滑器

    // 切换 Type 的交叉淡变：新腔体从静默起步，120 ms 等功率淡入；旧腔体继续
    // 跑到淡变结束再清空。没有这一段，切换会硬切湿声；而且非活动腔体会冻结，
    // 下次切回时把上一次的旧尾音"复现"出来。
    ReverbType activeType    = ReverbType::plate;   // 当前输出所用的腔体
    ReverbType fadingType    = ReverbType::plate;   // 淡出中的旧腔体
    bool       crossfading   = false;
    bool       typeInitialised = false;             // 首次收到参数时直接落位，不做淡变
    bool       hasPendingType  = false;             // 淡变中途又切换：排队等这一段结束
    ReverbType pendingType     = ReverbType::plate;
    float      fadeProgress  = 1.0f;                // 0…1
    float      fadeIncrement = 0.0f;                // 每样本推进量
    juce::AudioBuffer<float> fadeBuffer;            // 淡出腔体的本块输出

    static constexpr float kTypeCrossfadeSeconds = 0.12f;

    // M6 Shimmer：把湿声升高八度后回灌腔体输入。置于引擎层而非各腔体内部，
    // 因为 Shimmer 对 Plate/Room/Spring 都生效，且只有一个参数，不应在三个腔体里重复实现。
    juce::AudioBuffer<float> inputBuffer;     // 腔体输入（干声 + shimmer 回灌），始终双声道
    juce::AudioBuffer<float> shimmerBuffer;   // 上一块湿声移调后的回灌量，始终双声道
    PitchShifter             pitchShifterL, pitchShifterR;
    float                    shimmerGain = 0.0f;

    // 湿声电平（阶段三 3c 的动态着色器用它做"视觉呼吸"）：音频线程每块更新，
    // 界面线程每帧读。包络带约 100 ms 释放，否则画面会随波形抖成噪声。
    std::atomic<float> wetLevel { 0.0f };
    float              wetLevelEnvelope = 0.0f;

    /** 用块 RMS 推进电平包络并发布。 */
    void updateWetLevel (float blockRms, int numSamples) noexcept;

    // Shimmer 回灌的低切 + 柔化低通。
    //
    // 低切必须做：Room/Spring 的阻尼是「混合式」，lowDamp 小的时候高通几乎透明，
    // 腔体直流增益可达 1/(1-g) ≈ 22；而移调器对直流是"不动"的（升八度后直流仍是
    // 直流），于是直流会在反馈环里按 环路增益×22 累积而发散（实测 Room/decay=0.6
    // 时 RMS 涨到 1.6）。Plate 现在也改成了混合式阻尼，三个腔体统一依靠反馈路径上
    // 固定的 15 Hz 直流阻断把直流增益钉在 0；这一层低切保留为双保险，并兼作
    // Shimmer 环路的极低频抑制。
    //
    // 低通的作用在 M8 后发生了变化：抗混叠已由移调器内部的 2× 过采样 + 半带滤波
    // 完成（折叠点残余从旧方案的 −14 dB 量级压到 −80 dB 量级），这两级一阶低通
    // 不再承担抗混叠职责，而是保留为**刻意的音色塑形**——每升一次八度就更暗一层，
    // 符合经典 shimmer 的自然衰减方式，也是听感标定中已确认的"柔化程度"。
    // 想要更亮的 shimmer，把这两级去掉或提高截止即可（此时可听带内仍是干净的，
    // 只是内容会一直延伸到 fs/4 以上对应的八度位置）。
    OnePoleHighpass           shimmerHpfL, shimmerHpfR;
    OnePoleLowpass            shimmerLpf1L, shimmerLpf1R;   // 柔化低通第一级（5 kHz）
    OnePoleLowpass            shimmerLpf2L, shimmerLpf2R;   // 柔化低通第二级（5 kHz）

    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> dryGain { 1.0f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> wetGain { 0.0f };

    // 阻尼 / 调制 / 预延迟在引擎层做块级平滑。这三个参数直接改变滤波器系数或
    // 延迟读指针：宿主自动化时若逐块硬跳，会产生 zipper 与延迟读指针的咔哒。
    // 腔体的 setter 仍按"收到就重算系数"，所以这里每块只推进一次、推平滑后的值。
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedHighDamp { 0.5f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedLowDamp  { 0.2f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedMod      { 0.5f };
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> smoothedPreDelay { 20.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ReverbEngine)
};