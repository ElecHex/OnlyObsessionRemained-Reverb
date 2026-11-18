#pragma once

#include <cmath>

/**
    慢速三角波 LFO（纯 C++），用于腔体延迟线的调制（M3）。

    - 输出范围 [-1, 1]：phase 0 → +1，phase 0.5 → -1，phase 1 → +1
    - 多路 LFO 取不同相位偏移即可去相关，避免同相调制导致的周期性音染
    - 调制深度由调用方（PlateTank）按 Mod 参数与采样率换算，LFO 本身只出波形

    三角波而非正弦：斜率恒定，等价于对延迟线施加方波式的"音高偏移"，
    在 ±十几样本的浅调制下比正弦更容易打散金属味，也无需查表/三角函数。
*/
class TriangleLFO
{
public:
    void prepare (double newSampleRate, float frequencyHz, float phase01) noexcept
    {
        sampleRate = newSampleRate > 0.0 ? newSampleRate : 44100.0;
        increment  = frequencyHz / static_cast<float> (sampleRate);

        setPhase (phase01);
    }

    void reset() noexcept { phase = initialPhase; }

    void setPhase (float phase01) noexcept
    {
        initialPhase = phase01 - std::floor (phase01);   // 折叠到 [0, 1)
        phase        = initialPhase;
    }

    /** 推进一个采样并返回当前值，范围 [-1, 1]。 */
    float process() noexcept
    {
        phase += increment;

        if (phase >= 1.0f)
            phase -= 1.0f;

        return triangle (phase);
    }

    /** 不推进，读取当前值。 */
    float current() const noexcept { return triangle (phase); }

private:
    static float triangle (float p) noexcept
    {
        return 4.0f * std::abs (p - 0.5f) - 1.0f;
    }

    double sampleRate   = 44100.0;
    float  phase        = 0.0f;
    float  initialPhase = 0.0f;
    float  increment    = 0.0f;
};
