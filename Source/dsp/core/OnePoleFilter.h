#pragma once

#include <algorithm>
#include <cmath>

/**
    一阶低通 / 高通滤波器（纯 C++）。

    低通（High Damp）与高通（Low Damp）串联构成 M2 的反馈回路阻尼：
    低通让长尾高频先衰减，高通让低频先衰减，是 damp 参数的两条独立轴。
*/
class OnePoleLowpass
{
public:
    void setCutoff (float cutoffHz, double sampleRate) noexcept
    {
        const float sr = static_cast<float> (sampleRate > 0.0 ? sampleRate : 44100.0);
        const float fc = std::clamp (cutoffHz, 1.0f, sr * 0.49f);

        a = std::clamp (std::exp (-2.0f * 3.14159265f * fc / sr), 0.0f, 0.999999f);
        b = 1.0f - a;
    }

    void reset() noexcept { z = 0.0f; }

    float process (float x) noexcept
    {
        z = b * x + a * z;
        return z;
    }

private:
    float a = 0.0f;
    float b = 1.0f;
    float z = 0.0f;
};

/** 一阶高通（DC 阻断 / Low Damp 的基础单元）。 */
class OnePoleHighpass
{
public:
    void setCutoff (float cutoffHz, double sampleRate) noexcept
    {
        const float sr = static_cast<float> (sampleRate > 0.0 ? sampleRate : 44100.0);
        const float fc = std::clamp (cutoffHz, 1.0f, sr * 0.49f);

        a = std::clamp (std::exp (-2.0f * 3.14159265f * fc / sr), 0.0f, 0.999999f);
    }

    void reset() noexcept { z = 0.0f; }

    float process (float x) noexcept
    {
        z = a * z + (1.0f - a) * x;
        return x - z;
    }

private:
    float a = 0.0f;
    float z = 0.0f;
};