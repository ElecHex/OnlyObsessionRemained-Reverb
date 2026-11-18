#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

/**
    双颗粒交叉淡变式移调器（2× 过采样 + 半带抗混叠；纯 C++，无外部依赖）。

    用途：Shimmer —— 把混响湿声升高一个八度后反馈回腔体输入，形成逐层叠加的
    "闪烁"尾音。

    ---- 为什么必须过采样 ----

    读指针以 ratio 倍于写指针的速率推进，本质是「以 ratio 倍速重采样」：输入中
    频率为 f 的内容会在输出里出现在 ratio·f 处。升八度（ratio = 2）时，输入中
    高于 fs/4 的内容会被推到 fs/2 以上，直接在 fs 域工作就必然折叠回可听带
    （44.1 kHz 下即 11 kHz 以上全部折返，产生非谐波刺耳感）。

    本实现把颗粒引擎搬到 2× 采样率（fs2 = 2·fs）上运行：

      - 输入先经半带插值滤波升到 fs2，频带仍只到 fs/2；
      - 引擎在 fs2 域以 ratio 倍速读数，输入 fs/2 处的内容被推到 fs
        （= fs2/2，恰好是 fs2 域的奈奎斯特），**在 fs2 域内不产生任何折叠**；
      - 输出再经同一个半带滤波器抽取回 fs：高于 fs/2（即原信号高于 fs/4）的内容
        被直接滤掉，而不是折返进可听带。

    于是「折叠」被换成了「干净地丢弃」：丢掉的只是原信号 fs/4 以上的部分（升八度
    后本就落在 fs/2 以上、人耳听不到），换来的是可听带内不再出现任何非谐波折返
    产物。实测 14–22 kHz 输入在折叠点的残余比旧的两级一阶低通方案低 50 dB 以上
    （见 offline 第 21 节）。

    ---- 半带滤波器 ----

    零相位半带低通 g[n] = 0.5·sinc(n/2) 的非零抽头只落在 n = 0 与各奇数位置，
    多相分解后其中一路退化为**纯延迟**，另一路只是一条 FIR，实时开销很低。
    用 Kaiser 窗（β ≈ 9）加权后阻带可达约 −90 dB，过渡带落在 fs2/4 ≈ 0.5·fs 附近
    （96 kHz 下约 19.5–28.5 kHz），可听带内幅度误差 < 0.01 dB。这是它相比"一阶/
    两级一阶低通"的本质优势：既能把折叠点压到 −80 dB 量级，又不牺牲 4–10 kHz 的
    临场感（旧的 5 kHz 两级一阶在 11 kHz 只有约 −14 dB，且 8–10 kHz 已被削掉 2–3 dB）。

    ---- 颗粒引擎 ----

    读指针不断追近写指针，必须周期性回绕；回绕处的跳变会产生咔哒声，故用两路读
    指针错开半个窗口、以升余弦窗交叉淡变：一路增益归零时另一路正好在最大处，
    两者之和恒为 1，接缝听不出来。

    为什么用升余弦而不是线性：升余弦窗的斜率连续，接缝处一阶导也连续，移调过程中
    不会出现线性淡变特有的"音量抖动"。
*/
class PitchShifter
{
public:
    /**
        @param sampleRate     采样率
        @param pitchRatio     音高倍数（2 = 升八度）
        @param windowSeconds  颗粒窗口长度；越长接缝越平滑但音染越明显
    */
    void prepare (double sampleRate, float pitchRatio = 2.0f, double windowSeconds = 0.06);

    void reset() noexcept;

    /** 处理一个采样，返回移调后的输出。 */
    float process (float x) noexcept;

private:
    // 半带滤波器参数。kHalfPairs 决定过渡带宽度（越大越陡、开销越高）：
    // 15 对 → 61 抽头等效、阻带 −90 dB、过渡带约 0.20–0.30 倍 fs2。
    static constexpr int kHalfPairs   = 15;
    static constexpr int kHalfTaps    = 2 * kHalfPairs;        // 偶数相 FIR 抽头数
    static constexpr int kOddDelay    = kHalfPairs - 1;        // 奇数相：纯延迟的输入样本数
    static constexpr int kCenterDelay = 2 * kHalfPairs - 1;    // 2fs 域的中心延迟
    static constexpr int kRingSize    = 32;                    // 历史环形缓冲（2 的幂，掩码回绕）
    static constexpr int kRingMask    = kRingSize - 1;

    void  buildHalfbandTaps() noexcept;
    float processGrain (float x) noexcept;
    float readGrainInterpolated (float delaySamples) const noexcept;
    float readGrainAt (int delaySamples) const noexcept;

    double sr    = 44100.0;
    float  ratio = 2.0f;

    // ---- 半带滤波：偶数相 FIR 抽头（对称）+ 奇数相中心抽头 ----
    // evenTaps 与 centerTap 的直流增益之和恰为 1（抽取器），插值器整体再乘 2
    // 以补偿零填充带来的 −6 dB。
    float evenTaps [kHalfTaps] = {};
    float centerTap            = 0.5f;

    float interpRing[kRingSize] = {};   // 插值器历史：过去 kHalfTaps 个 fs 样本
    int   interpIndex           = 0;
    float evenPhaseRing[kRingSize] = {};   // 抽取器历史：偶数相（fs2 域 2n 处）
    int   evenPhaseIndex           = 0;
    float oddPhaseRing[kRingSize] = {};    // 抽取器历史：奇数相（fs2 域 2n+1 处）
    int   oddPhaseIndex           = 0;

    // ---- 颗粒引擎（运行在 2× 采样率上）----
    int    window     = 2048;
    int    minDelay   = 4;
    int    bufferSize = 0;
    int    writeIndex = 0;
    float  phase      = 0.0f;

    std::vector<float> buffer;
};

// ============================================================================
// 实现
// ============================================================================

inline void PitchShifter::prepare (double sampleRate, float pitchRatio, double windowSeconds)
{
    sr    = sampleRate > 0.0 ? sampleRate : 44100.0;
    ratio = std::max (1.0001f, pitchRatio);

    buildHalfbandTaps();

    // 颗粒引擎跑到 2× 采样率上：窗口时长不变、样本数翻倍；
    // 读数速率即 ratio（fs2 域每输出样本读 ratio 个样本 = 相对原信号 ratio 倍速）。
    const double sr2 = sr * 2.0;

    window     = std::max (64, static_cast<int> (std::lround (windowSeconds * sr2)));
    minDelay   = 4;
    bufferSize = window + minDelay + 4;

    buffer.assign (static_cast<size_t> (bufferSize), 0.0f);

    reset();
}

inline void PitchShifter::reset() noexcept
{
    std::fill (buffer.begin(), buffer.end(), 0.0f);
    writeIndex = 0;
    phase      = 0.0f;

    std::fill (interpRing,    interpRing    + kRingSize, 0.0f);
    std::fill (evenPhaseRing, evenPhaseRing + kRingSize, 0.0f);
    std::fill (oddPhaseRing,  oddPhaseRing  + kRingSize, 0.0f);

    interpIndex    = 0;
    evenPhaseIndex = 0;
    oddPhaseIndex  = 0;
}

/**
    生成半带系数。

    零相位理想半带低通 g[n] = 0.5·sinc(n/2)（直流增益 1）加 Kaiser 窗后，
    延迟 kCenterDelay 个样本使其因果化，则非零抽头落在：
      - 偶数延迟 2i（i = 0..kHalfTaps−1），值 = g[2i − kCenterDelay]
      - 中心延迟 kCenterDelay（奇数），值 = g[0] = 0.5

    偶数相抽头按「和 = 0.5」归一化（中心抽头固定 0.5），于是抽取器总直流增益
    恰为 1（电平严格透明），插值器的奇数相则退化为增益恰好为 1 的纯延迟。
*/
inline void PitchShifter::buildHalfbandTaps() noexcept
{
    constexpr double pi   = 3.14159265358979;
    constexpr double beta = 8.96;    // Kaiser 窗参数：阻带约 −90 dB

    auto besselI0 = [] (double v)
    {
        double sum = 1.0, term = 1.0;

        for (int k = 1; k < 40; ++k)
        {
            const double r = v / (2.0 * k);
            term *= r * r;
            sum  += term;

            if (term < 1.0e-15 * sum)
                break;
        }

        return sum;
    };

    const double i0Beta = besselI0 (beta);
    const double span   = static_cast<double> (kCenterDelay);

    auto kaiser = [&] (double t)   // |t| ≤ 1
    {
        const double u = std::sqrt (std::max (0.0, 1.0 - t * t));
        return besselI0 (beta * u) / i0Beta;
    };

    double sum = 0.0;

    for (int i = 0; i < kHalfTaps; ++i)
    {
        const double offset = static_cast<double> (2 * i - kCenterDelay);   // 奇数偏移
        const double x      = offset * 0.5;
        const double sinc   = std::sin (pi * x) / (pi * x);

        evenTaps[i] = static_cast<float> (0.5 * sinc * kaiser (offset / span));
        sum        += evenTaps[i];
    }

    // 偶数相归一化到 0.5，与中心抽头 0.5 相加后总增益为 1
    const float scale = static_cast<float> (0.5 / std::max (1.0e-12, sum));

    for (auto& t : evenTaps)
        t *= scale;

    centerTap = 0.5f;
}

inline float PitchShifter::process (float x) noexcept
{
    // ---- 1. 半带插值：1 个 fs 样本 → 2 个 fs2 样本 ----
    interpRing[interpIndex] = x;

    float evenOut = 0.0f;
    int   idx     = interpIndex;

    for (int i = 0; i < kHalfTaps; ++i)
    {
        evenOut += evenTaps[i] * interpRing[idx];
        idx      = (idx + kRingSize - 1) & kRingMask;
    }

    evenOut *= 2.0f;   // 补偿零填充导致的 −6 dB

    // 奇数相只用到插值滤波器的中心抽头：增益恰为 1 的纯延迟
    const float oddOut = interpRing[(interpIndex + kRingSize - kOddDelay) & kRingMask];

    interpIndex = (interpIndex + 1) & kRingMask;

    // ---- 2. 颗粒引擎（两次调用即 fs2 域的连续两个样本）----
    const float grainEven = processGrain (evenOut);
    const float grainOdd  = processGrain (oddOut);

    // ---- 3. 半带抽取：把 fs/2 以上滤掉后取偶数相 ----
    evenPhaseRing[evenPhaseIndex] = grainEven;
    oddPhaseRing [oddPhaseIndex]  = grainOdd;

    float out = 0.0f;
    idx       = evenPhaseIndex;

    for (int i = 0; i < kHalfTaps; ++i)
    {
        out += evenTaps[i] * evenPhaseRing[idx];
        idx  = (idx + kRingSize - 1) & kRingMask;
    }

    out += centerTap * oddPhaseRing[(oddPhaseIndex + kRingSize - kHalfPairs) & kRingMask];

    evenPhaseIndex = (evenPhaseIndex + 1) & kRingMask;
    oddPhaseIndex  = (oddPhaseIndex  + 1) & kRingMask;

    return out;
}

/**
    颗粒引擎单步（fs2 域）。

    读指针追赶写指针 → 延迟线性**减小**，一个窗口后回绕。延迟减小意味着读指针
    相对写指针前进更快 → 音高升高（若改为增大则降调）。
*/
inline float PitchShifter::processGrain (float x) noexcept
{
    buffer[static_cast<size_t> (writeIndex)] = x;

    if (++writeIndex >= bufferSize)
        writeIndex = 0;

    phase -= (ratio - 1.0f);

    if (phase < 0.0f)
        phase += static_cast<float> (window);

    const float phi1 = phase / static_cast<float> (window);          // [0, 1)
    float       phi2 = phi1 + 0.5f;

    if (phi2 >= 1.0f)
        phi2 -= 1.0f;

    // 升余弦交叉淡变：两路增益之和恒为 1，且在各自的回绕点严格为 0
    constexpr float twoPi = 6.2831853f;
    const float g1 = 0.5f * (1.0f - std::cos (twoPi * phi1));
    const float g2 = 0.5f * (1.0f - std::cos (twoPi * phi2));

    const float d1 = static_cast<float> (minDelay) + phi1 * static_cast<float> (window);
    const float d2 = static_cast<float> (minDelay) + phi2 * static_cast<float> (window);

    return g1 * readGrainInterpolated (d1) + g2 * readGrainInterpolated (d2);
}

/** 线性插值读取 writeIndex 之前 delaySamples 个样本处的值。 */
inline float PitchShifter::readGrainInterpolated (float delaySamples) const noexcept
{
    delaySamples = std::clamp (delaySamples, 1.0f, static_cast<float> (bufferSize - 2));

    const int   i0   = static_cast<int> (delaySamples);
    const float frac = delaySamples - static_cast<float> (i0);

    const float a = readGrainAt (i0);
    const float b = readGrainAt (i0 + 1);

    return a + (b - a) * frac;
}

inline float PitchShifter::readGrainAt (int delaySamples) const noexcept
{
    int index = writeIndex - delaySamples;

    while (index < 0)
        index += bufferSize;

    return buffer[static_cast<size_t> (index)];
}