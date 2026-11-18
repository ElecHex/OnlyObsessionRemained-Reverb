#pragma once

#include "dsp/core/DelayLine.h"

/**
    Schroeder 全通滤波器（单延迟线实现）。

    传递函数 H(z) = (-g + z^-D) / (1 - g * z^-D)
    采用「存 v、取 v[n-D]」的写法，只需一条延迟线、一次乘法。

    用途：
      - 输入扩散级（M1 起）：多级级联把稀疏回声打散成密集回声
      - 腔体内扩散（M1 起）
*/
class AllpassFilter
{
public:
    void prepare (int maxDelaySamples)
    {
        line.prepare (maxDelaySamples + 2);
        delaySamples = std::min (std::max (1, delaySamples), line.getLength() - 2);
    }

    void reset() noexcept { line.reset(); }

    void setDelay (int samples) noexcept
    {
        delaySamples = std::min (std::max (1, samples), line.getLength() - 2);
    }

    void setGain (float g) noexcept { gain = g; }

    float process (float x) noexcept
    {
        const float delayed = line.read (delaySamples);
        const float v       = x + gain * delayed;

        line.write (v);

        return -gain * v + delayed;
    }

    /**
        读取全通内部延迟线的历史样本（n 个样本前）。

        腔体的输出抽头会跨支路、跨全通取点（见 Dattorro 原版输出矩阵），
        其中部分抽头偏移量大于全通自身的延迟长度，因此延迟线需预留额外容量。
    */
    float readDelayTap (int samples) const noexcept
    {
        return line.read (samples);
    }

private:
    DelayLine line;
    int   delaySamples = 1;
    float gain         = 0.5f;
};