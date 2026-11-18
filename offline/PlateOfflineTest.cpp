// 离线验证 harness：只依赖纯 C++ 的 DSP 核心，不需要 JUCE / 插件宿主。
//
// 用途：
//   1. M0 —— 验证腔体直通与稳定性（无 NaN / 无自激）
//   2. M1 —— 用脉冲响应评估回声密度、RT60、早期能量分布，判断是否"密集且不金属"
//   3. M2 —— 标定 Decay→RT60 映射、High/Low Damp 的频带衰减差异、湿干电平
//   4. M3 —— 调制（Mod=0 与静止腔体一致、Mod>0 去相关、RT60 保持）
//   5. M4 —— 预延迟（首到达精确后移对应毫秒、PreDelay=0 逐比特直通）
//   6. M5 —— Room 腔体（RT60 标定、阻尼频带差异、预延迟、稳定性、湿干电平）
//   7. M6 —— Spring 腔体（色散：高频带先于低频带到达）与 PitchShifter（升八度频谱）
//   8. M8 —— 抗混叠（移调器 2× 过采样 + 半带）与反馈路微饱和（曲线性质 / THD /
//            在腔体内的压缩量 / 热信号稳定性）
//
// 编译：cmake --build build --target plate_offline_test

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

#include "dsp/core/OnePoleFilter.h"
#include "dsp/core/PitchShifter.h"
#include "dsp/core/Saturator.h"
#include "dsp/tanks/PlateTank.h"
#include "dsp/tanks/RoomTank.h"
#include "dsp/tanks/SpringTank.h"

namespace
{
    constexpr double kSampleRate  = 48000.0;
    constexpr int    kBlockSize   = 512;

    struct IRMetrics
    {
        float  peak           = 0.0f;
        bool   finite         = true;
        int    nonzeroSamples = 0;
        int    firstArrival   = -1;    // 首个超过阈值的样本下标
        int    peakIndex      = -1;
        double rt60           = 0.0;
        double energyEarly    = 0.0;   // 0 – 50 ms
        double energyMid      = 0.0;   // 50 – 200 ms
        double energyLate     = 0.0;   // 200 ms – 1 s
        double energyTail     = 0.0;   // 1 – 5 s
    };

    /** Schroeder 反向积分求 EDC，再由 -5 dB / -35 dB 两点外推 RT60。 */
    double rt60FromEDC (const std::vector<float>& ir, double sampleRate)
    {
        const size_t n = ir.size();
        std::vector<double> edc (n, 0.0);

        double running = 0.0;
        for (size_t i = n; i-- > 0;)
        {
            running += static_cast<double> (ir[i]) * static_cast<double> (ir[i]);
            edc[i] = running;
        }

        if (edc.front() <= 0.0)
            return 0.0;

        const double total = edc.front();

        auto timeAtDb = [&] (double db)
        {
            const double target = total * std::pow (10.0, db / 10.0);

            for (size_t i = 0; i < n; ++i)
                if (edc[i] <= target)
                    return static_cast<double> (i) / sampleRate;

            return static_cast<double> (n) / sampleRate;
        };

        return 2.0 * (timeAtDb (-35.0) - timeAtDb (-5.0));
    }

    /** 把 IR 过滤到某个频带后再测 RT60：lowBand=true 取低频带，false 取高频带。 */
    double bandRT60 (const std::vector<float>& ir, bool lowBand)
    {
        std::vector<float> filtered (ir.size(), 0.0f);

        if (lowBand)
        {
            OnePoleLowpass lp;
            lp.setCutoff (500.0f, kSampleRate);
            for (size_t i = 0; i < ir.size(); ++i)
                filtered[i] = lp.process (ir[i]);
        }
        else
        {
            OnePoleHighpass hp;
            hp.setCutoff (4000.0f, kSampleRate);
            for (size_t i = 0; i < ir.size(); ++i)
                filtered[i] = hp.process (ir[i]);
        }

        return rt60FromEDC (filtered, kSampleRate);
    }

    IRMetrics analyse (const std::vector<float>& ir, double sampleRate)
    {
        IRMetrics m;

        constexpr float activityThreshold = 1.0e-4f;

        for (size_t i = 0; i < ir.size(); ++i)
        {
            const float v = ir[i];

            if (! std::isfinite (v))
            {
                m.finite = false;
                continue;
            }

            const float a = std::abs (v);

            if (a > m.peak)
            {
                m.peak      = a;
                m.peakIndex = static_cast<int> (i);
            }

            if (m.firstArrival < 0 && a > activityThreshold)
                m.firstArrival = static_cast<int> (i);
        }

        for (float v : ir)
            if (std::abs (v) > 1.0e-6f)
                ++m.nonzeroSamples;

        m.rt60 = rt60FromEDC (ir, sampleRate);

        auto bandEnergy = [&] (double fromSec, double toSec)
        {
            const size_t n = ir.size();
            const size_t a = std::min (n, static_cast<size_t> (fromSec * sampleRate));
            const size_t b = std::min (n, static_cast<size_t> (toSec   * sampleRate));

            double sum = 0.0;
            for (size_t i = a; i < b; ++i)
                sum += static_cast<double> (ir[i]) * static_cast<double> (ir[i]);

            return sum;
        };

        m.energyEarly = bandEnergy (0.0,   0.05);
        m.energyMid   = bandEnergy (0.05,  0.2);
        m.energyLate  = bandEnergy (0.2,   1.0);
        m.energyTail  = bandEnergy (1.0,   5.0);

        return m;
    }

    void printMetrics (const char* label, const IRMetrics& m, double sampleRate)
    {
        const double duration = static_cast<double> (m.nonzeroSamples) / sampleRate;

        std::printf ("[%s]\n", label);
        std::printf ("  peak            : %.6f %s\n", m.peak, m.finite ? "" : "  <-- 出现 NaN/Inf！");
        std::printf ("  首次到达        : %d 样本 (%.1f ms)\n",
                     m.firstArrival, m.firstArrival < 0 ? -1.0
                                                        : 1000.0 * m.firstArrival / sampleRate);
        std::printf ("  峰值位置        : %d 样本 (%.1f ms)\n",
                     m.peakIndex, 1000.0 * m.peakIndex / sampleRate);
        std::printf ("  有效样本        : %d (约 %.2f s)\n", m.nonzeroSamples, duration);
        std::printf ("  RT60 (EDC 外推) : %.3f s\n", m.rt60);
        std::printf ("  能量 0-50ms     : %.6e\n", m.energyEarly);
        std::printf ("  能量 50-200ms   : %.6e\n", m.energyMid);
        std::printf ("  能量 200ms-1s   : %.6e\n", m.energyLate);
        std::printf ("  能量 1-5s       : %.6e\n", m.energyTail);
        std::printf ("\n");
    }

    struct Case
    {
        const char* label;
        float decay;
        float highDamp;
        float lowDamp;
    };

    /**
        通用 IR 渲染：Tank 需提供 prepare / setDecay / setHighDamping / setLowDamping /
        setModulation / setPreDelay / process，PlateTank 与 RoomTank 接口一致。
        mod 默认 0，即静止腔体。
    */
    template <typename Tank>
    std::vector<float> renderIRTank (float decay, float highDamp, float lowDamp,
                                     double durationSec, float mod = 0.0f,
                                     float preDelayMs = 0.0f)
    {
        const int numSamples = static_cast<int> (kSampleRate * durationSec);

        std::vector<float> inL (numSamples, 0.0f), inR (numSamples, 0.0f);
        std::vector<float> outL (numSamples, 0.0f), outR (numSamples, 0.0f);

        inL[0] = 1.0f;
        inR[0] = 1.0f;

        Tank tank;
        tank.prepare (kSampleRate);
        tank.setDecay (decay);
        tank.setHighDamping (highDamp);
        tank.setLowDamping (lowDamp);
        tank.setModulation (mod);
        tank.setPreDelay (preDelayMs);

        for (int offset = 0; offset < numSamples; offset += kBlockSize)
        {
            const int n = std::min (kBlockSize, numSamples - offset);

            tank.process (inL.data() + offset, inR.data() + offset,
                          outL.data() + offset, outR.data() + offset, n);
        }

        return outL;
    }

    /** Plate 腔体 IR（左通道）。 */
    std::vector<float> renderIR (float decay, float highDamp, float lowDamp,
                                 double durationSec, float mod = 0.0f,
                                 float preDelayMs = 0.0f)
    {
        return renderIRTank<PlateTank> (decay, highDamp, lowDamp, durationSec, mod, preDelayMs);
    }

    /** Room 腔体 IR（左通道，M5）。 */
    std::vector<float> renderRoomIR (float decay, float highDamp, float lowDamp,
                                     double durationSec, float mod = 0.0f,
                                     float preDelayMs = 0.0f)
    {
        return renderIRTank<RoomTank> (decay, highDamp, lowDamp, durationSec, mod, preDelayMs);
    }

    /** Spring 腔体 IR（左通道，M6）。 */
    std::vector<float> renderSpringIR (float decay, float highDamp, float lowDamp,
                                       double durationSec, float mod = 0.0f,
                                       float preDelayMs = 0.0f)
    {
        return renderIRTank<SpringTank> (decay, highDamp, lowDamp, durationSec, mod, preDelayMs);
    }

    /**
        用窄带音爆测弹簧的群延迟，从而验证色散。

        为什么不能用冲激响应：全通级联的**冲激能量重心**约等于其延迟本身，与群延迟
        无关（推导：单级全通 h[0]=-g, h[mD]=(1-g²)g^(m-1)，重心 ≈ D 而与 g 无关），
        因此无论色散多强，冲激的能量重心都测不出差异。

        群延迟只对窄带信号才直接表现为到达时间：低频群延迟远大于高频，故低频音爆
        应明显晚到。做法是分别送一段 300 Hz 与 6 kHz 的窄带音爆，用 5 ms 滑窗 RMS
        作包络，取包络首次超过峰值 10% 的时刻作为到达时间。
    */
    double springToneOnsetMs (double freq)
    {
        const int total = static_cast<int> (0.35 * kSampleRate);
        const int burst = static_cast<int> (0.040 * kSampleRate);
        const int ramp  = static_cast<int> (0.004 * kSampleRate);

        std::vector<float> inL (total, 0.0f), inR (total, 0.0f);
        std::vector<float> outL (total, 0.0f), outR (total, 0.0f);

        for (int i = 0; i < burst; ++i)
        {
            float env = 1.0f;

            if (i < ramp)
                env = 0.5f * (1.0f - std::cos (3.14159265f * static_cast<float> (i) / static_cast<float> (ramp)));
            else if (i >= burst - ramp)
                env = 0.5f * (1.0f - std::cos (3.14159265f * static_cast<float> (burst - i) / static_cast<float> (ramp)));

            const float s = env * std::sin (static_cast<float> (2.0 * 3.14159265358979 * freq * i / kSampleRate));
            inL[i] = s;
            inR[i] = s;
        }

        SpringTank tank;
        tank.prepare (kSampleRate);
        tank.setDecay (0.5f);
        tank.setHighDamping (0.0f);
        tank.setLowDamping (0.0f);
        tank.setModulation (0.0f);
        tank.setPreDelay (0.0f);

        for (int offset = 0; offset < total; offset += kBlockSize)
        {
            const int n = std::min (kBlockSize, total - offset);

            tank.process (inL.data() + offset, inR.data() + offset,
                          outL.data() + offset, outR.data() + offset, n);
        }

        // 5 ms 滑动窗 RMS 作为包络
        const int win = static_cast<int> (0.005 * kSampleRate);
        std::vector<double> envelope (static_cast<size_t> (total), 0.0);

        double acc = 0.0;
        for (int i = 0; i < total; ++i)
        {
            acc += static_cast<double> (outL[static_cast<size_t> (i)]) * outL[static_cast<size_t> (i)];

            if (i >= win)
                acc -= static_cast<double> (outL[static_cast<size_t> (i - win)]) * outL[static_cast<size_t> (i - win)];

            envelope[static_cast<size_t> (i)] = std::sqrt (acc / win);
        }

        double peak = 0.0;
        for (double e : envelope)
            peak = std::max (peak, e);

        if (peak <= 0.0)
            return -1.0;

        const double threshold = peak * 0.1;

        for (int i = 0; i < total; ++i)
            if (envelope[static_cast<size_t> (i)] > threshold)
                return 1000.0 * i / kSampleRate;

        return -1.0;
    }

    /** 两条 IR 的归一化差异（差值 RMS / 参考 RMS）。 */
    double relativeDifference (const std::vector<float>& a, const std::vector<float>& b)
    {
        const size_t n = std::min (a.size(), b.size());

        double diff = 0.0, ref = 0.0;
        for (size_t i = 0; i < n; ++i)
        {
            const double d = static_cast<double> (a[i]) - static_cast<double> (b[i]);
            diff += d * d;
            ref  += static_cast<double> (a[i]) * static_cast<double> (a[i]);
        }

        return ref > 0.0 ? std::sqrt (diff / ref) : 0.0;
    }

    /** 名义 Decay→RT60 映射，与 PlateTank::setDecay 保持一致。 */
    double nominalRT60 (float decay01)
    {
        return 0.25 * std::pow (120.0, static_cast<double> (decay01));
    }

    struct LevelRatio
    {
        double inputRms  = 0.0;
        double outputRms = 0.0;
        double ratio     = 0.0;
    };

    /**
        用白噪声测湿路稳态电平，用于标定 kOutputScale。

        输入取 ±noiseAmp 的均匀白噪声（默认 ±0.25，RMS ≈ 0.1443），跳过前 1 s 让腔体
        进入稳态，再统计后 3 s 的 RMS。ratio ≈ 1 意味着 Mix=100% 时湿声与干声等响。
        放大噪音幅度可观察反馈路微饱和带来的电平压缩（热电平下 ratio 会略降）。
    */
    template <typename Tank>
    LevelRatio measureWetDryLevelTank (float decay, float highDamp, float lowDamp,
                                       float noiseAmp = 0.25f)
    {
        constexpr double durationSec = 4.0;
        constexpr double skipSec     = 1.0;

        const int numSamples = static_cast<int> (kSampleRate * durationSec);
        const int skipSamples = static_cast<int> (kSampleRate * skipSec);

        std::vector<float> inL (numSamples, 0.0f);
        std::vector<float> outL (numSamples, 0.0f), outR (numSamples, 0.0f);

        // 固定种子的 LCG，保证结果可复现
        unsigned int seed = 0x12345678u;
        auto noise = [&seed]()
        {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float> (seed >> 8) / 8388608.0f - 1.0f;
        };

        for (auto& s : inL)
            s = noise() * noiseAmp;

        Tank tank;
        tank.prepare (kSampleRate);
        tank.setDecay (decay);
        tank.setHighDamping (highDamp);
        tank.setLowDamping (lowDamp);

        for (int offset = 0; offset < numSamples; offset += kBlockSize)
        {
            const int n = std::min (kBlockSize, numSamples - offset);

            tank.process (inL.data() + offset, inL.data() + offset,
                          outL.data() + offset, outR.data() + offset, n);
        }

        LevelRatio r;

        auto rms = [] (const std::vector<float>& v, int from, int to)
        {
            double sum = 0.0;
            for (int i = from; i < to; ++i)
                sum += static_cast<double> (v[i]) * static_cast<double> (v[i]);

            return std::sqrt (sum / static_cast<double> (to - from));
        };

        r.inputRms  = rms (inL,  skipSamples, numSamples);
        r.outputRms = rms (outL, skipSamples, numSamples);
        r.ratio     = r.inputRms > 0.0 ? r.outputRms / r.inputRms : 0.0;

        return r;
    }

    /** Plate 腔体湿干电平。 */
    LevelRatio measureWetDryLevel (float decay, float highDamp, float lowDamp, float noiseAmp = 0.25f)
    {
        return measureWetDryLevelTank<PlateTank> (decay, highDamp, lowDamp, noiseAmp);
    }

    /** Room 腔体湿干电平（M5）。 */
    LevelRatio measureRoomWetDryLevel (float decay, float highDamp, float lowDamp, float noiseAmp = 0.25f)
    {
        return measureWetDryLevelTank<RoomTank> (decay, highDamp, lowDamp, noiseAmp);
    }

    /** Spring 腔体湿干电平（M6）。 */
    LevelRatio measureSpringWetDryLevel (float decay, float highDamp, float lowDamp, float noiseAmp = 0.25f)
    {
        return measureWetDryLevelTank<SpringTank> (decay, highDamp, lowDamp, noiseAmp);
    }

    /** 热白噪声压力用例的输出（左通道），用于确认微饱和钳住了环路（不自激）。
        只在前 inputSec 秒注入激励，之后静音，便于观察尾巴是否继续衰减。 */
    template <typename Tank>
    std::vector<float> renderNoiseTank (float decay, float mod, float preDelayMs,
                                        double durationSec, float amp, double inputSec = 1.0e9)
    {
        const int numSamples = static_cast<int> (kSampleRate * durationSec);
        const int inputSamples = static_cast<int> (std::min (durationSec, inputSec) * kSampleRate);

        std::vector<float> inL (numSamples, 0.0f), inR (numSamples, 0.0f);
        std::vector<float> outL (numSamples, 0.0f), outR (numSamples, 0.0f);

        // 与 measureWetDryLevelTank 不同的种子，避免与电平用例相关
        unsigned int seed = 0x9e3779b9u;
        auto noise = [&seed]()
        {
            seed = seed * 1664525u + 1013904223u;
            return static_cast<float> (seed >> 8) / 8388608.0f - 1.0f;
        };

        for (int i = 0; i < inputSamples; ++i)
            inL[static_cast<size_t> (i)] = noise() * amp;

        inR = inL;

        Tank tank;
        tank.prepare (kSampleRate);
        tank.setDecay (decay);
        tank.setHighDamping (0.0f);
        tank.setLowDamping (0.0f);
        tank.setModulation (mod);
        tank.setPreDelay (preDelayMs);

        for (int offset = 0; offset < numSamples; offset += kBlockSize)
        {
            const int n = std::min (kBlockSize, numSamples - offset);

            tank.process (inL.data() + offset, inR.data() + offset,
                          outL.data() + offset, outR.data() + offset, n);
        }

        return outL;
    }

    /** 指定时间段上的 RMS（用于观察尾巴是否继续衰减）。 */
    double windowRms (const std::vector<float>& v, double fromSec, double toSec)
    {
        const int a = static_cast<int> (fromSec * kSampleRate);
        const int b = std::min (static_cast<int> (v.size()),
                                static_cast<int> (toSec * kSampleRate));

        if (b <= a)
            return 0.0;

        double sum = 0.0;
        for (int i = a; i < b; ++i)
            sum += static_cast<double> (v[static_cast<size_t> (i)]) * v[static_cast<size_t> (i)];

        return std::sqrt (sum / static_cast<double> (b - a));
    }

    /** 单频点 DFT 幅度（Goertzel），用于验证移调器输出中目标频率分量的强弱。 */
    double goertzelMagnitude (const std::vector<float>& x, int from, int count, double freq)
    {
        const double w  = 2.0 * 3.14159265358979 * freq / kSampleRate;
        const double cw = std::cos (w);
        const double coeff = 2.0 * cw;

        double s0 = 0.0, s1 = 0.0, s2 = 0.0;

        for (int i = from; i < from + count; ++i)
        {
            s0 = static_cast<double> (x[static_cast<size_t> (i)]) + coeff * s1 - s2;
            s2 = s1;
            s1 = s0;
        }

        const double real = s1 - s2 * cw;
        const double imag = s2 * std::sin (w);

        return std::sqrt (real * real + imag * imag);
    }

    /**
        移调器（升八度）对单一正弦的响应：返回输出中 measureFreq 分量相对输入的电平（dB）。

        抗混叠的关键判据：输入频率 f 高于 fs/4 时，升八度后的「正确」输出落在 fs/2
        以上（人耳听不到），滤波不到位就会折返到 fs−2f 处。旧方案（fs 域直接 2 倍速
        读取 + 两级一阶 5 kHz 低通）在折返点只剩低通在该频段的衰减量；2× 过采样 +
        半带之后，「正确」输出被干净滤掉，折返点应接近本底。
    */
    double shifterResponseDb (double inFreq, double measureFreq)
    {
        constexpr double pi    = 3.14159265358979;
        const int        total = static_cast<int> (kSampleRate);

        std::vector<float> in (total, 0.0f), out (total, 0.0f);

        for (int i = 0; i < total; ++i)
            in[static_cast<size_t> (i)] = static_cast<float> (
                std::sin (2.0 * pi * inFreq * i / kSampleRate));

        PitchShifter shifter;
        shifter.prepare (kSampleRate);

        for (int i = 0; i < total; ++i)
            out[static_cast<size_t> (i)] = shifter.process (in[static_cast<size_t> (i)]);

        // 跳过前 0.2 s 让颗粒进入稳态，在随后 0.5 s 上做 DFT
        const int from  = static_cast<int> (0.2 * kSampleRate);
        const int count = static_cast<int> (0.5 * kSampleRate);

        const double amp = goertzelMagnitude (out, from, count, measureFreq) / (0.5 * count);

        return 20.0 * std::log10 (std::max (1.0e-9, amp));
    }

    /** 旧方案在折叠点的残余（解析）：两级一阶低通（5 kHz）在输入频率处的总衰减。 */
    double legacyTwoPoleDb (double freq)
    {
        const double r = freq / 5000.0;
        return -20.0 * std::log10 (1.0 + r * r);
    }

    /** softSaturate 在给定幅度正弦下的总谐波失真（3/5/7 次相对基波）。 */
    double saturationThd (double amplitude)
    {
        constexpr double pi   = 3.14159265358979;
        constexpr double freq = 1000.0;    // 整数周期，避免频谱泄漏
        const int        total = static_cast<int> (kSampleRate);

        std::vector<float> y (total, 0.0f);

        for (int i = 0; i < total; ++i)
            y[static_cast<size_t> (i)] = softSaturate (static_cast<float> (
                amplitude * std::sin (2.0 * pi * freq * i / kSampleRate)));

        const double h1 = goertzelMagnitude (y, 0, total, freq);
        const double h3 = goertzelMagnitude (y, 0, total, 3.0 * freq);
        const double h5 = goertzelMagnitude (y, 0, total, 5.0 * freq);
        const double h7 = goertzelMagnitude (y, 0, total, 7.0 * freq);

        return h1 > 0.0 ? std::sqrt (h3 * h3 + h5 * h5 + h7 * h7) / h1 : 0.0;
    }
}

int main()
{
    std::printf ("=== OnlyObsessionRemained 离线腔体验证 ===\n");
    std::printf ("采样率 %.0f Hz，块大小 %d\n\n", kSampleRate, kBlockSize);

    bool allFinite = true;
    bool allPassed = true;

    // ---- 1. 基准用例：回声密度 / 早期能量分布 ----
    std::printf ("--- 1. 基准脉冲响应 ---\n\n");

    const Case cases[] = {
        { "Decay 0.20 / HiDamp 0.30", 0.20f, 0.30f, 0.00f },
        { "Decay 0.50 / HiDamp 0.40", 0.50f, 0.40f, 0.00f },
        { "Decay 0.80 / HiDamp 0.60", 0.80f, 0.60f, 0.00f },
    };

    for (const auto& c : cases)
    {
        const auto ir = renderIR (c.decay, c.highDamp, c.lowDamp, 12.0);
        const auto m  = analyse (ir, kSampleRate);
        allFinite = allFinite && m.finite;

        printMetrics (c.label, m, kSampleRate);
    }

    // ---- 2. Decay → RT60 标定（隔离阻尼，验证映射准确性）----
    std::printf ("--- 2. Decay → RT60 标定（High/Low Damp 全关）---\n\n");
    std::printf ("  %-8s %-12s %-12s %-8s\n", "Decay", "名义 RT60", "实测 RT60", "比值");

    const float calibDecays[] = { 0.2f, 0.4f, 0.6f, 0.8f };

    for (float d : calibDecays)
    {
        const auto   ir     = renderIR (d, 0.0f, 0.0f, 15.0);
        const double        nominal = nominalRT60 (d);
        const double        measured = rt60FromEDC (ir, kSampleRate);
        const double        ratio    = measured / nominal;

        std::printf ("  %-8.2f %-12.3f %-12.3f %-8.3f\n", d, nominal, measured, ratio);

        // 容差 ±20%：EDC 外推本身有噪声。低 decay 端必然偏差更大——腔体绕一圈就要
        // 约 0.73 s（含全通相位延迟），当名义 RT60 低于单圈时间时脉冲响应退化为
        // 稀疏的几次弹跳，Schroeder 积分不再平滑，外推值天然偏高。
        if (ratio < 0.80 || ratio > 1.20)
        {
            std::printf ("      ^^ 超出 ±20%% 容差\n");
            allPassed = false;
        }
    }
    std::printf ("\n");

    // ---- 3. RT60 随 Decay 单调递增 ----
    std::printf ("--- 3. RT60 单调性 ---\n\n");

    double prevRT60 = 0.0;
    bool   monotonic = true;

    for (float d : calibDecays)
    {
        const double rt = rt60FromEDC (renderIR (d, 0.0f, 0.0f, 15.0), kSampleRate);

        if (rt <= prevRT60)
            monotonic = false;

        prevRT60 = rt;
    }

    std::printf ("  RT60 随 Decay 单调递增：%s\n\n", monotonic ? "是" : "否 <-- 失败");
    allPassed = allPassed && monotonic;

    // ---- 4. High / Low Damp 的频带衰减差异 ----
    std::printf ("--- 4. 阻尼频带差异（Decay 0.60 固定）---\n\n");
    std::printf ("  %-28s %-12s %-12s\n", "用例", "低频带 RT60", "高频带 RT60");

    const struct { const char* label; float highDamp; float lowDamp; } dampCases[] = {
        { "无阻尼",              0.00f, 0.00f },
        { "High Damp 0.80",      0.80f, 0.00f },
        { "Low Damp 0.80",       0.00f, 0.80f },
    };

    for (const auto& dc : dampCases)
    {
        const auto   ir = renderIR (0.60f, dc.highDamp, dc.lowDamp, 12.0);
        const double lo = bandRT60 (ir, true);
        const double hi = bandRT60 (ir, false);

        std::printf ("  %-28s %-12.3f %-12.3f\n", dc.label, lo, hi);
    }
    std::printf ("\n");

    // 高频阻尼应当让高频带 RT60 相对无阻尼显著缩短；低频阻尼反之
    const auto irRef    = renderIR (0.60f, 0.00f, 0.00f, 12.0);
    const auto irHiDamp = renderIR (0.60f, 0.80f, 0.00f, 12.0);
    const auto irLoDamp = renderIR (0.60f, 0.00f, 0.80f, 12.0);

    const double hiRef  = bandRT60 (irRef,    false);
    const double hiDamp = bandRT60 (irHiDamp, false);
    const double loRef  = bandRT60 (irRef,    true);
    const double loDamp = bandRT60 (irLoDamp, true);

    const bool highDampWorks = hiDamp < hiRef * 0.85;
    const bool lowDampWorks  = loDamp < loRef * 0.85;

    std::printf ("  High Damp 缩短高频带 RT60（%.3f → %.3f）：%s\n",
                 hiRef, hiDamp, highDampWorks ? "是" : "否 <-- 失败");
    std::printf ("  Low  Damp 缩短低频带 RT60（%.3f → %.3f）：%s\n\n",
                 loRef, loDamp, lowDampWorks ? "是" : "否 <-- 失败");

    allPassed = allPassed && highDampWorks && lowDampWorks;

    // ---- 5. 湿干电平标定 ----
    std::printf ("--- 5. 湿路电平（白噪声，kOutputScale=0.5）---\n\n");
    std::printf ("  %-34s %-12s %-12s\n", "用例", "输入 RMS", "wet/dry");

    const struct { const char* label; float decay; float highDamp; float lowDamp; } levelCases[] = {
        { "Decay 0.30 / HiDamp 0.50 / LoDamp 0.20 (默认)", 0.30f, 0.50f, 0.20f },
        { "Decay 0.50 / HiDamp 0.50 / LoDamp 0.20",       0.50f, 0.50f, 0.20f },
        { "Decay 0.80 / HiDamp 0.60 / LoDamp 0.20",       0.80f, 0.60f, 0.20f },
    };

    for (const auto& lc : levelCases)
    {
        const auto r = measureWetDryLevel (lc.decay, lc.highDamp, lc.lowDamp);
        std::printf ("  %-34s %-12.4f %-12.3f\n", lc.label, r.inputRms, r.ratio);
    }
    std::printf ("\n");

    // ---- 6. M3 调制 ----
    std::printf ("--- 6. M3 调制（Decay 0.60 / 无阻尼）---\n\n");
    std::printf ("  %-8s %-12s %-14s %-10s\n", "Mod", "RT60", "差异 RMS", "首样本");

    const auto irMod0   = renderIR (0.60f, 0.0f, 0.0f, 12.0, 0.0f);
    const auto irMod35  = renderIR (0.60f, 0.0f, 0.0f, 12.0, 0.35f);
    const auto irMod70  = renderIR (0.60f, 0.0f, 0.0f, 12.0, 0.70f);

    const double rtMod0  = rt60FromEDC (irMod0,  kSampleRate);
    const double rtMod35 = rt60FromEDC (irMod35, kSampleRate);
    const double rtMod70 = rt60FromEDC (irMod70, kSampleRate);

    const double diff35 = relativeDifference (irMod0, irMod35);
    const double diff70 = relativeDifference (irMod0, irMod70);

    std::printf ("  %-8.2f %-12.3f %-14.4f %-10.6f\n", 0.00f, rtMod0,  0.0,    irMod0[0]);
    std::printf ("  %-8.2f %-12.3f %-14.4f %-10.6f\n", 0.35f, rtMod35, diff35, irMod35[0]);
    std::printf ("  %-8.2f %-12.3f %-14.4f %-10.6f\n", 0.70f, rtMod70, diff70, irMod70[0]);
    std::printf ("\n");

    // 首样本只经输入扩散 + APF1 直通，与调制无关，三档必须逐比特一致
    const bool directPathStable = irMod0[0] == irMod35[0] && irMod0[0] == irMod70[0];

    // Mod>0 必须真实改变输出；同时调制是浅层的，不应破坏衰减时间
    const bool modulationActive = diff35 > 0.02 && diff70 > 0.05;
    const bool rt60Preserved    = std::abs (rtMod70 / rtMod0 - 1.0) < 0.25;

    std::printf ("  直通首样本不受调制影响（逐比特一致）：%s\n", directPathStable ? "是" : "否 <-- 失败");
    std::printf ("  Mod>0 改变输出（差异 RMS %.4f / %.4f > 阈值）：%s\n",
                 diff35, diff70, modulationActive ? "是" : "否 <-- 失败");
    std::printf ("  Mod=0.70 的 RT60 相对 Mod=0 变化 < 25%%（%.3f vs %.3f）：%s\n\n",
                 rtMod70, rtMod0, rt60Preserved ? "是" : "否 <-- 失败");

    allPassed = allPassed && directPathStable && modulationActive && rt60Preserved;

    // ---- 7. M4 预延迟 ----
    std::printf ("--- 7. M4 预延迟（Decay 0.60 / 无阻尼 / 无调制）---\n\n");
    std::printf ("  %-10s %-14s %-14s %-12s\n", "PreDelay", "首到达样本", "理论位移", "实测-理论");

    const auto irPre0   = renderIR (0.60f, 0.0f, 0.0f, 12.0, 0.0f, 0.0f);
    const auto irPre50  = renderIR (0.60f, 0.0f, 0.0f, 12.0, 0.0f, 50.0f);
    const auto irPre150 = renderIR (0.60f, 0.0f, 0.0f, 12.0, 0.0f, 150.0f);

    const int firstPre0   = analyse (irPre0,   kSampleRate).firstArrival;
    const int firstPre50  = analyse (irPre50,  kSampleRate).firstArrival;
    const int firstPre150 = analyse (irPre150, kSampleRate).firstArrival;

    const int expected50  = static_cast<int> (std::lround (0.050 * kSampleRate));
    const int expected150 = static_cast<int> (std::lround (0.150 * kSampleRate));

    std::printf ("  %-10.0f %-14d %-14d %-12d\n", 0.0f,   firstPre0,   0,               0);
    std::printf ("  %-10.0f %-14d %-14d %-12d\n", 50.0f,  firstPre50,  expected50,      firstPre50  - firstPre0 - expected50);
    std::printf ("  %-10.0f %-14d %-14d %-12d\n", 150.0f, firstPre150, expected150,     firstPre150 - firstPre0 - expected150);
    std::printf ("\n");

    // 预延迟是整数样本的纯延迟，首到达必须精确后移对应毫秒数
    const bool preDelayExact = (firstPre50  - firstPre0 == expected50)
                            && (firstPre150 - firstPre0 == expected150);

    // PreDelay=0 时必须与默认（不设预延迟）逐比特一致，即不破坏 M3 行为
    const auto irNoPre = renderIR (0.60f, 0.0f, 0.0f, 12.0, 0.0f, 0.0f);
    const bool zeroPreTransparent = relativeDifference (irPre0, irNoPre) == 0.0;

    std::printf ("  首到达精确后移对应毫秒数：%s\n", preDelayExact ? "是" : "否 <-- 失败");
    std::printf ("  PreDelay=0 逐比特直通（与 M3 一致）：%s\n\n", zeroPreTransparent ? "是" : "否 <-- 失败");

    allPassed = allPassed && preDelayExact && zeroPreTransparent;

    // ---- 8. 稳定性 ----
    std::printf ("--- 8. 数值稳定性 ---\n\n");

    const auto irMaxMod = renderIR (0.90f, 0.0f, 0.0f, 12.0, 1.0f);
    const bool maxModFinite = analyse (irMaxMod, kSampleRate).finite;

    // 预延迟拉满 + 满调制 + 长衰减的压力用例
    const auto irStress = renderIR (0.90f, 0.0f, 0.0f, 12.0, 1.0f, 500.0f);
    const bool stressFinite = analyse (irStress, kSampleRate).finite;

    std::printf ("  无 NaN/Inf（含 Decay 0.90 / Mod 1.0 压力用例）：%s\n",
                 (allFinite && maxModFinite) ? "是" : "否 <-- 失败");
    std::printf ("  无 NaN/Inf（PreDelay 500ms / Mod 1.0 / Decay 0.90）：%s\n\n",
                 stressFinite ? "是" : "否 <-- 失败");
    allPassed = allPassed && allFinite && maxModFinite && stressFinite;

    // ================================================================
    // 以下为 M5：Room 腔体（8 线 FDN）
    // ================================================================

    // ---- 9. Room 基准脉冲响应 ----
    std::printf ("--- 9. Room 基准脉冲响应（8 线 FDN）---\n\n");

    const Case roomCases[] = {
        { "Room Decay 0.20 / HiDamp 0.30", 0.20f, 0.30f, 0.00f },
        { "Room Decay 0.50 / HiDamp 0.40", 0.50f, 0.40f, 0.00f },
        { "Room Decay 0.80 / HiDamp 0.60", 0.80f, 0.60f, 0.00f },
    };

    for (const auto& c : roomCases)
    {
        const auto ir = renderRoomIR (c.decay, c.highDamp, c.lowDamp, 12.0);
        const auto m  = analyse (ir, kSampleRate);
        allFinite = allFinite && m.finite;

        printMetrics (c.label, m, kSampleRate);
    }

    // ---- 10. Room Decay → RT60 标定（隔离阻尼）----
    std::printf ("--- 10. Room Decay → RT60 标定（High/Low Damp 全关）---\n\n");
    std::printf ("  %-8s %-12s %-12s %-8s\n", "Decay", "名义 RT60", "实测 RT60", "比值");

    bool roomMonotonic = true;
    double prevRoomRT60 = 0.0;

    for (float d : calibDecays)
    {
        const auto   ir      = renderRoomIR (d, 0.0f, 0.0f, 15.0);
        const double nominal  = nominalRT60 (d);
        const double measured = rt60FromEDC (ir, kSampleRate);
        const double ratio    = measured / nominal;

        std::printf ("  %-8.2f %-12.3f %-12.3f %-8.3f\n", d, nominal, measured, ratio);

        if (ratio < 0.80 || ratio > 1.20)
        {
            std::printf ("      ^^ 超出 ±20%% 容差\n");
            allPassed = false;
        }

        if (measured <= prevRoomRT60)
            roomMonotonic = false;

        prevRoomRT60 = measured;
    }

    std::printf ("\n  Room RT60 随 Decay 单调递增：%s\n\n", roomMonotonic ? "是" : "否 <-- 失败");
    allPassed = allPassed && roomMonotonic;

    // ---- 11. Room 阻尼频带差异（Decay 0.60 固定）----
    std::printf ("--- 11. Room 阻尼频带差异（Decay 0.60 固定）---\n\n");
    std::printf ("  %-28s %-12s %-12s\n", "用例", "低频带 RT60", "高频带 RT60");

    const struct { const char* label; float highDamp; float lowDamp; } roomDampCases[] = {
        { "无阻尼",              0.00f, 0.00f },
        { "High Damp 0.80",      0.80f, 0.00f },
        { "Low Damp 0.80",       0.00f, 0.80f },
    };

    for (const auto& dc : roomDampCases)
    {
        const auto   ir = renderRoomIR (0.60f, dc.highDamp, dc.lowDamp, 12.0);
        const double lo = bandRT60 (ir, true);
        const double hi = bandRT60 (ir, false);

        std::printf ("  %-28s %-12.3f %-12.3f\n", dc.label, lo, hi);
    }

    const auto roomRef    = renderRoomIR (0.60f, 0.00f, 0.00f, 12.0);
    const auto roomHiDamp = renderRoomIR (0.60f, 0.80f, 0.00f, 12.0);
    const auto roomLoDamp = renderRoomIR (0.60f, 0.00f, 0.80f, 12.0);

    const bool roomHighDampWorks = bandRT60 (roomHiDamp, false) < bandRT60 (roomRef, false) * 0.85;
    const bool roomLowDampWorks  = bandRT60 (roomLoDamp, true)  < bandRT60 (roomRef, true)  * 0.85;

    std::printf ("\n  High Damp 缩短高频带 RT60：%s\n", roomHighDampWorks ? "是" : "否 <-- 失败");
    std::printf ("  Low  Damp 缩短低频带 RT60：%s\n\n", roomLowDampWorks ? "是" : "否 <-- 失败");
    allPassed = allPassed && roomHighDampWorks && roomLowDampWorks;

    // ---- 12. Room 湿干电平（标定 kOutputScale）----
    std::printf ("--- 12. Room 湿路电平（白噪声）---\n\n");
    std::printf ("  %-34s %-12s %-12s\n", "用例", "输入 RMS", "wet/dry");

    const struct { const char* label; float decay; float highDamp; float lowDamp; } roomLevelCases[] = {
        { "Decay 0.30 / HiDamp 0.50 / LoDamp 0.20 (默认)", 0.30f, 0.50f, 0.20f },
        { "Decay 0.50 / HiDamp 0.50 / LoDamp 0.20",       0.50f, 0.50f, 0.20f },
        { "Decay 0.80 / HiDamp 0.60 / LoDamp 0.20",       0.80f, 0.60f, 0.20f },
    };

    double roomDefaultRatio = 0.0;

    for (const auto& lc : roomLevelCases)
    {
        const auto r = measureRoomWetDryLevel (lc.decay, lc.highDamp, lc.lowDamp);
        std::printf ("  %-34s %-12.4f %-12.3f\n", lc.label, r.inputRms, r.ratio);

        if (roomDefaultRatio == 0.0)
            roomDefaultRatio = r.ratio;
    }

    // 默认参数下湿声应与干声大致等响（kOutputScale 的标定依据）
    const bool roomLevelOk = roomDefaultRatio > 0.85 && roomDefaultRatio < 1.20;
    std::printf ("\n  默认参数 wet/dry 接近 1（%.3f）：%s\n\n",
                 roomDefaultRatio, roomLevelOk ? "是" : "否 <-- 失败");
    allPassed = allPassed && roomLevelOk;

    // ---- 13. Room 预延迟与稳定性 ----
    std::printf ("--- 13. Room 预延迟与稳定性 ---\n\n");

    const int roomFirst0   = analyse (renderRoomIR (0.60f, 0.0f, 0.0f, 12.0, 0.0f, 0.0f),   kSampleRate).firstArrival;
    const int roomFirst80  = analyse (renderRoomIR (0.60f, 0.0f, 0.0f, 12.0, 0.0f, 80.0f),  kSampleRate).firstArrival;
    const int roomExpect80 = static_cast<int> (std::lround (0.080 * kSampleRate));

    const bool roomPreDelayExact = (roomFirst80 - roomFirst0 == roomExpect80);

    std::printf ("  预延迟 80 ms 使首到达后移 %d 样本（理论 %d）：%s\n",
                 roomFirst80 - roomFirst0, roomExpect80, roomPreDelayExact ? "是" : "否 <-- 失败");

    const auto roomStress     = renderRoomIR (0.90f, 0.0f, 0.0f, 12.0, 1.0f, 500.0f);
    const bool roomStressFin  = analyse (roomStress, kSampleRate).finite;
    const bool roomModFinite  = analyse (renderRoomIR (0.60f, 0.0f, 0.0f, 12.0, 1.0f), kSampleRate).finite;

    std::printf ("  无 NaN/Inf（Mod 1.0）：%s\n", roomModFinite ? "是" : "否 <-- 失败");
    std::printf ("  无 NaN/Inf（PreDelay 500ms / Mod 1.0 / Decay 0.90）：%s\n\n",
                 roomStressFin ? "是" : "否 <-- 失败");

    allPassed = allPassed && roomPreDelayExact && roomModFinite && roomStressFin && allFinite;

    // ================================================================
    // 以下为 M6：Spring 腔体（色散链）+ PitchShifter（Shimmer 用）
    // ================================================================

    // ---- 14. Spring 基准脉冲响应 ----
    std::printf ("--- 14. Spring 基准脉冲响应（48 级色散链）---\n\n");

    const Case springCases[] = {
        { "Spring Decay 0.20 / HiDamp 0.30", 0.20f, 0.30f, 0.00f },
        { "Spring Decay 0.50 / HiDamp 0.40", 0.50f, 0.40f, 0.00f },
        { "Spring Decay 0.80 / HiDamp 0.60", 0.80f, 0.60f, 0.00f },
    };

    for (const auto& c : springCases)
    {
        const auto ir = renderSpringIR (c.decay, c.highDamp, c.lowDamp, 12.0);
        const auto m  = analyse (ir, kSampleRate);
        allFinite = allFinite && m.finite;

        printMetrics (c.label, m, kSampleRate);
    }

    // ---- 15. Spring Decay → RT60 标定 + 单调性 ----
    std::printf ("--- 15. Spring Decay → RT60 标定（High/Low Damp 全关）---\n\n");
    std::printf ("  %-8s %-12s %-12s %-8s\n", "Decay", "名义 RT60", "实测 RT60", "比值");

    bool   springMonotonic = true;
    double prevSpringRT60  = 0.0;

    for (float d : calibDecays)
    {
        const auto   ir      = renderSpringIR (d, 0.0f, 0.0f, 15.0);
        const double nominal  = nominalRT60 (d);
        const double measured = rt60FromEDC (ir, kSampleRate);
        const double ratio    = measured / nominal;

        std::printf ("  %-8.2f %-12.3f %-12.3f %-8.3f\n", d, nominal, measured, ratio);

        if (ratio < 0.80 || ratio > 1.20)
        {
            std::printf ("      ^^ 超出 ±20%% 容差\n");
            allPassed = false;
        }

        if (measured <= prevSpringRT60)
            springMonotonic = false;

        prevSpringRT60 = measured;
    }

    std::printf ("\n  Spring RT60 随 Decay 单调递增：%s\n\n", springMonotonic ? "是" : "否 <-- 失败");
    allPassed = allPassed && springMonotonic;

    // ---- 16. Spring 色散验证（低频群延迟 > 高频）----
    std::printf ("--- 16. Spring 色散验证（窄带音爆到达时间）---\n\n");

    const double springOnsetLow  = springToneOnsetMs (200.0);
    const double springOnsetHigh = springToneOnsetMs (2000.0);

    // 要求高频至少早到 8 ms，避免把测量噪声当成色散
    const bool dispersionOk = springOnsetLow > 0.0 && springOnsetHigh > 0.0
                           && springOnsetHigh < springOnsetLow - 8.0;

    std::printf ("  2 kHz 音爆到达 : %.2f ms\n", springOnsetHigh);
    std::printf ("  200 Hz 音爆到达 : %.2f ms\n", springOnsetLow);
    std::printf ("  低频显著晚于高频（色散特征，差值 %.2f ms）：%s\n\n",
                 springOnsetLow - springOnsetHigh, dispersionOk ? "是" : "否 <-- 失败");
    allPassed = allPassed && dispersionOk;

    // ---- 17. Spring 阻尼频带差异（Decay 0.60 固定）----
    std::printf ("--- 17. Spring 阻尼频带差异（Decay 0.60 固定）---\n\n");
    std::printf ("  %-28s %-12s %-12s\n", "用例", "低频带 RT60", "高频带 RT60");

    const struct { const char* label; float highDamp; float lowDamp; } springDampCases[] = {
        { "无阻尼",              0.00f, 0.00f },
        { "High Damp 0.80",      0.80f, 0.00f },
        { "Low Damp 0.80",       0.00f, 0.80f },
    };

    for (const auto& dc : springDampCases)
    {
        const auto   ir = renderSpringIR (0.60f, dc.highDamp, dc.lowDamp, 12.0);
        const double lo = bandRT60 (ir, true);
        const double hi = bandRT60 (ir, false);

        std::printf ("  %-28s %-12.3f %-12.3f\n", dc.label, lo, hi);
    }

    const auto springRef    = renderSpringIR (0.60f, 0.00f, 0.00f, 12.0);
    const auto springHiDamp = renderSpringIR (0.60f, 0.80f, 0.00f, 12.0);
    const auto springLoDamp = renderSpringIR (0.60f, 0.00f, 0.80f, 12.0);

    const bool springHighDampWorks = bandRT60 (springHiDamp, false) < bandRT60 (springRef, false) * 0.85;
    const bool springLowDampWorks  = bandRT60 (springLoDamp, true)  < bandRT60 (springRef, true)  * 0.85;

    std::printf ("\n  High Damp 缩短高频带 RT60：%s\n", springHighDampWorks ? "是" : "否 <-- 失败");
    std::printf ("  Low  Damp 缩短低频带 RT60：%s\n\n", springLowDampWorks ? "是" : "否 <-- 失败");
    allPassed = allPassed && springHighDampWorks && springLowDampWorks;

    // ---- 18. Spring 湿干电平（标定 kOutputScale）----
    std::printf ("--- 18. Spring 湿路电平（白噪声）---\n\n");
    std::printf ("  %-34s %-12s %-12s\n", "用例", "输入 RMS", "wet/dry");

    const struct { const char* label; float decay; float highDamp; float lowDamp; } springLevelCases[] = {
        { "Decay 0.30 / HiDamp 0.50 / LoDamp 0.20 (默认)", 0.30f, 0.50f, 0.20f },
        { "Decay 0.50 / HiDamp 0.50 / LoDamp 0.20",       0.50f, 0.50f, 0.20f },
        { "Decay 0.80 / HiDamp 0.60 / LoDamp 0.20",       0.80f, 0.60f, 0.20f },
    };

    double springDefaultRatio = 0.0;

    for (const auto& lc : springLevelCases)
    {
        const auto r = measureSpringWetDryLevel (lc.decay, lc.highDamp, lc.lowDamp);
        std::printf ("  %-34s %-12.4f %-12.3f\n", lc.label, r.inputRms, r.ratio);

        if (springDefaultRatio == 0.0)
            springDefaultRatio = r.ratio;
    }

    const bool springLevelOk = springDefaultRatio > 0.85 && springDefaultRatio < 1.20;
    std::printf ("\n  默认参数 wet/dry 接近 1（%.3f）：%s\n\n",
                 springDefaultRatio, springLevelOk ? "是" : "否 <-- 失败");
    allPassed = allPassed && springLevelOk;

    // ---- 19. Spring 预延迟与稳定性 ----
    std::printf ("--- 19. Spring 预延迟与稳定性 ---\n\n");

    const int springFirst0   = analyse (renderSpringIR (0.60f, 0.0f, 0.0f, 12.0, 0.0f, 0.0f),  kSampleRate).firstArrival;
    const int springFirst80  = analyse (renderSpringIR (0.60f, 0.0f, 0.0f, 12.0, 0.0f, 80.0f), kSampleRate).firstArrival;
    const int springExpect80 = static_cast<int> (std::lround (0.080 * kSampleRate));

    const bool springPreDelayExact = (springFirst80 - springFirst0 == springExpect80);

    std::printf ("  预延迟 80 ms 使首到达后移 %d 样本（理论 %d）：%s\n",
                 springFirst80 - springFirst0, springExpect80, springPreDelayExact ? "是" : "否 <-- 失败");

    const bool springModFinite    = analyse (renderSpringIR (0.60f, 0.0f, 0.0f, 12.0, 1.0f),        kSampleRate).finite;
    const bool springStressFinite = analyse (renderSpringIR (0.90f, 0.0f, 0.0f, 12.0, 1.0f, 500.0f), kSampleRate).finite;

    std::printf ("  无 NaN/Inf（Mod 1.0）：%s\n", springModFinite ? "是" : "否 <-- 失败");
    std::printf ("  无 NaN/Inf（PreDelay 500ms / Mod 1.0 / Decay 0.90）：%s\n\n",
                 springStressFinite ? "是" : "否 <-- 失败");

    allPassed = allPassed && springPreDelayExact && springModFinite && springStressFinite;

    // ---- 20. PitchShifter（Shimmer 升八度）----
    std::printf ("--- 20. PitchShifter 升八度验证 ---\n\n");

    {
        constexpr double testFreq = 440.0;
        const int total = static_cast<int> (kSampleRate);

        std::vector<float> psIn (total, 0.0f), psOut (total, 0.0f);

        for (int i = 0; i < total; ++i)
            psIn[static_cast<size_t> (i)] = 0.5f * std::sin (static_cast<float> (2.0 * 3.14159265358979
                                                                                    * testFreq * i / kSampleRate));

        PitchShifter shifter;
        shifter.prepare (kSampleRate);

        for (int i = 0; i < total; ++i)
            psOut[static_cast<size_t> (i)] = shifter.process (psIn[static_cast<size_t> (i)]);

        // 跳过前 0.2 s 让颗粒进入稳态，在随后 0.5 s 上做 DFT
        const int from  = static_cast<int> (0.2 * kSampleRate);
        const int count = static_cast<int> (0.5 * kSampleRate);

        const double mag440 = goertzelMagnitude (psOut, from, count, testFreq);
        const double mag880 = goertzelMagnitude (psOut, from, count, testFreq * 2.0);

        bool psFinite = true;
        for (float v : psOut)
            psFinite = psFinite && std::isfinite (v);

        std::printf ("  输入 440 Hz 正弦，输出 440 Hz 分量 : %.2f\n", mag440);
        std::printf ("  输入 440 Hz 正弦，输出 880 Hz 分量 : %.2f\n", mag880);
        std::printf ("\n");

        const bool octaveUp = mag880 > mag440 * 5.0;

        std::printf ("  输出以 880 Hz（升八度）为主：%s\n", octaveUp ? "是" : "否 <-- 失败");
        std::printf ("  无 NaN/Inf：%s\n\n", psFinite ? "是" : "否 <-- 失败");

        allPassed = allPassed && octaveUp && psFinite;
    }

    // ---- 21. M8 抗混叠（移调器 2× 过采样 + 半带滤波）----
    std::printf ("--- 21. PitchShifter 抗混叠（升八度的折返）---\n\n");

    // 升八度后「正确」输出落在奈奎斯特之上（f > fs/4 时）的输入，若滤波不到位就会
    // 折返到 fs−2f。判据分两组：
    //   A 组：折返落点在可闻带内（< 20 kHz）—— 必须压到 −60 dB 以下；
    //   B 组：折返落点已在 20 kHz 以上 —— 本身就在不可闻区，只记录、不设硬指标。
    const double audibleAliasInputs[] = { 15000.0, 16000.0, 18000.0, 20000.0, 22000.0 };
    const double edgeAliasInputs[]    = { 12500.0, 13000.0, 14000.0 };

    std::printf ("  A 组：折返落在可闻带内（要求 ≤ −60 dB）\n\n");
    std::printf ("  %-9s %-11s %-11s %-13s %-14s\n",
                 "输入", "八度输出", "折返落点", "实测折返", "旧方案(解析)");

    bool   aliasOk      = true;
    double worstAliasDb = -200.0;

    for (double f : audibleAliasInputs)
    {
        const double foldFreq = kSampleRate - 2.0 * f;
        const double measured = shifterResponseDb (f, foldFreq);

        std::printf ("  %-9.0f %-11.0f %-11.0f %-13.1f %-14.1f\n",
                     f, 2.0 * f, foldFreq, measured, legacyTwoPoleDb (f));

        worstAliasDb = std::max (worstAliasDb, measured);

        if (measured > -60.0)
            aliasOk = false;
    }

    std::printf ("\n  B 组：折返落点已在 20 kHz 以上（不可闻区，仅记录）\n\n");
    std::printf ("  %-9s %-11s %-11s %-13s %-14s\n",
                 "输入", "八度输出", "折返落点", "实测折返", "旧方案(解析)");

    double worstEdgeDb = -200.0;

    for (double f : edgeAliasInputs)
    {
        const double foldFreq = kSampleRate - 2.0 * f;
        const double measured = shifterResponseDb (f, foldFreq);

        std::printf ("  %-9.0f %-11.0f %-11.0f %-13.1f %-14.1f\n",
                     f, 2.0 * f, foldFreq, measured, legacyTwoPoleDb (f));

        worstEdgeDb = std::max (worstEdgeDb, measured);
    }

    // 说明：12–14 kHz 的「正确」八度输出（25–28 kHz）本身已越过输出奈奎斯特 24 kHz，
    // 恰好落在半带滤波器的过渡带内（过渡带上缘 28.6 kHz），因此会有一小部分折回
    // 20–23 kHz。这是「折叠点紧邻奈奎斯特」时的固有代价，且完全落在不可闻区；
    // 真正重要的是可闻带内（A 组）已被压到 −90 dB 量级。实际 shimmer 链路里，
    // 该频段的内容还会先被 5 kHz 柔化低通额外压低约 17–19 dB。
    std::printf ("  （最差 %.1f dB，落点 23 kHz，人耳不可闻）\n", worstEdgeDb);

    std::printf ("\n  %-9s %-12s %-14s\n", "输入", "八度输出", "实测增益");

    double worstPassband = 0.0;

    for (double f : { 8000.0, 10000.0 })
    {
        const double measured = shifterResponseDb (f, 2.0 * f);

        std::printf ("  %-9.0f %-12.0f %-14.2f dB\n", f, 2.0 * f, measured);
        worstPassband = std::min (worstPassband, measured);
    }

    const bool passbandOk = worstPassband > -1.5;

    std::printf ("\n  可闻带内折返全部 ≤ −60 dB（最差 %.1f dB）：%s\n",
                 worstAliasDb, aliasOk ? "是" : "否 <-- 失败");
    std::printf ("  带内八度输出几乎无损失（最差 %.2f dB）：%s\n\n",
                 worstPassband, passbandOk ? "是" : "否 <-- 失败");

    allPassed = allPassed && aliasOk && passbandOk;

    // ---- 22. M8 反馈路微饱和（softSaturate）----
    std::printf ("--- 22. 反馈路微饱和（softSaturate）---\n\n");

    bool satTransparent = true, satMonotone = true, satBounded = true;
    float prevY = -1.0e9f;

    for (int k = -3000; k <= 3000; ++k)
    {
        const float v = static_cast<float> (k) * 0.001f;
        const float y = softSaturate (v);

        if (std::abs (v) <= 0.7f && y != v)              satTransparent = false;
        if (y < prevY)                                   satMonotone    = false;
        if (! std::isfinite (y) || std::abs (y) >= 2.0f) satBounded     = false;

        prevY = y;
    }

    std::printf ("  |x| ≤ 0.7 逐比特透明：%s\n", satTransparent ? "是" : "否 <-- 失败");
    std::printf ("  单调不减、有界（渐近 2.0）：%s\n\n",
                 (satMonotone && satBounded) ? "是" : "否 <-- 失败");

    std::printf ("  1 kHz 正弦的 THD：\n");

    for (double a : { 0.35, 0.70, 1.00, 1.50, 2.00 })
        std::printf ("    输入幅度 %.2f -> %6.3f %%\n", a, 100.0 * saturationThd (a));

    allPassed = allPassed && satTransparent && satMonotone && satBounded;

    // 起弯电平标定：同一腔体在不同输入电平下的 wet/dry 比。
    // 线性系统下该比值与电平无关；一旦反馈路开始起弯，热电平下的比值就会下降，
    // 下降的起点即「饱和拐点」在输入侧的位置。
    std::printf ("\n  起弯电平标定（白噪声，Decay 0.70 / HiDamp 0.50 / LoDamp 0.20）：\n\n");
    std::printf ("  %-10s %-12s %-12s %-12s\n", "输入峰值", "Plate", "Room", "Spring");

    const float levelSweep[] = { 0.10f, 0.25f, 0.50f, 0.75f, 1.00f };

    double hotComp[3] = { 1.0, 1.0, 1.0 };
    double refRatio[3] = { 0.0, 0.0, 0.0 };

    for (float amp : levelSweep)
    {
        const double ratios[3] = {
            measureWetDryLevel       (0.70f, 0.50f, 0.20f, amp).ratio,
            measureRoomWetDryLevel   (0.70f, 0.50f, 0.20f, amp).ratio,
            measureSpringWetDryLevel (0.70f, 0.50f, 0.20f, amp).ratio
        };

        for (int i = 0; i < 3; ++i)
        {
            if (amp == levelSweep[0])
                refRatio[i] = ratios[i];

            hotComp[i] = refRatio[i] > 0.0 ? ratios[i] / refRatio[i] : 0.0;
        }

        std::printf ("  %-10.2f %-12.3f %-12.3f %-12.3f\n", amp, ratios[0], ratios[1], ratios[2]);
    }

    std::printf ("\n  最热电平（±1.00）相对冷端（±0.10）的 wet/dry 比："
                 "Plate %.3f / Room %.3f / Spring %.3f\n",
                 hotComp[0], hotComp[1], hotComp[2]);

    // 必须真的起弯（比值下降），但幅度必须轻微（未变成限幅）
    const bool satEngaged = hotComp[0] < 0.999 && hotComp[1] < 0.999 && hotComp[2] < 0.999;
    const bool satSubtle  = hotComp[0] > 0.85 && hotComp[1] > 0.85 && hotComp[2] > 0.85;

    std::printf ("  热电平下确实起弯：%s\n", satEngaged ? "是" : "否 <-- 失败");
    std::printf ("  压缩轻微（比值 > 0.85，未变成限幅）：%s\n\n", satSubtle ? "是" : "否 <-- 失败");

    allPassed = allPassed && satEngaged && satSubtle;

    // 热信号压力用例：±1.0 白噪声注入 3 s 后静音 2 s，
    // 最长衰减 + 满调制 + 满预延迟。判据是「激励停止后尾巴必须继续衰减」。
    std::printf ("  热信号压力用例（±1.0 白噪声 3 s + 静音 2 s / Decay 0.90 / Mod 1.0 / PreDelay 500ms）：\n");

    const auto hotPlateSig  = renderNoiseTank<PlateTank>  (0.90f, 1.0f, 500.0f, 5.0, 1.0f, 3.0);
    const auto hotRoomSig   = renderNoiseTank<RoomTank>   (0.90f, 1.0f, 500.0f, 5.0, 1.0f, 3.0);
    const auto hotSpringSig = renderNoiseTank<SpringTank> (0.90f, 1.0f, 500.0f, 5.0, 1.0f, 3.0);

    const auto hotPlate  = analyse (hotPlateSig,  kSampleRate);
    const auto hotRoom   = analyse (hotRoomSig,   kSampleRate);
    const auto hotSpring = analyse (hotSpringSig, kSampleRate);

    const bool hotFinite = hotPlate.finite && hotRoom.finite && hotSpring.finite;

    // 尾/头电平比：静音后 1.5–2.0 s 的 RMS / 激励期 1.5–3.0 s 的 RMS
    const double tailPlate  = windowRms (hotPlateSig,  4.5, 5.0) / std::max (1.0e-9, windowRms (hotPlateSig,  1.5, 3.0));
    const double tailRoom   = windowRms (hotRoomSig,   4.5, 5.0) / std::max (1.0e-9, windowRms (hotRoomSig,   1.5, 3.0));
    const double tailSpring = windowRms (hotSpringSig, 4.5, 5.0) / std::max (1.0e-9, windowRms (hotSpringSig, 1.5, 3.0));

    const bool hotDecays = tailPlate < 0.7 && tailRoom < 0.7 && tailSpring < 0.7;

    std::printf ("    Plate  峰值 %.3f，尾/头 %.3f\n", hotPlate.peak,  tailPlate);
    std::printf ("    Room   峰值 %.3f，尾/头 %.3f\n", hotRoom.peak,   tailRoom);
    std::printf ("    Spring 峰值 %.3f，尾/头 %.3f\n", hotSpring.peak, tailSpring);
    std::printf ("    无 NaN/Inf：%s\n", hotFinite ? "是" : "否 <-- 失败");
    std::printf ("    激励停止后尾巴继续衰减（未自激）：%s\n\n", hotDecays ? "是" : "否 <-- 失败");

    allPassed = allPassed && hotFinite && hotDecays;

    std::printf ("结论：%s\n", allPassed ? "全部检查通过" : "存在未通过项，需要排查");

    return allPassed ? 0 : 1;
}