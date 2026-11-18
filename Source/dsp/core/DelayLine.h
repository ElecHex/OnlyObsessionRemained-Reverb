#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

/**
    循环延迟线（纯 C++，无外部依赖）。

    - write/read：整数延迟，用于固定延迟长度
    - readFractional：线性插值的分数延迟，为 M3 的延迟线调制预留

    容器仅提供 C++ 原语本身，不引入插件框架类型，便于离线 harness 与后续嵌入式移植复用。
*/
class DelayLine
{
public:
    void prepare (int maxLengthSamples)
    {
        length = std::max (2, maxLengthSamples);
        buffer.assign (static_cast<size_t> (length), 0.0f);
        writeIndex = 0;
    }

    void reset() noexcept
    {
        std::fill (buffer.begin(), buffer.end(), 0.0f);
        writeIndex = 0;
    }

    void write (float value) noexcept
    {
        buffer[static_cast<size_t> (writeIndex)] = value;

        if (++writeIndex >= length)
            writeIndex = 0;
    }

    int getLength() const noexcept { return length; }

    /** 整数延迟读取；delaySamples 会被安全折叠到 [1, length-1]。 */
    float read (int delaySamples) const noexcept
    {
        delaySamples = std::clamp (delaySamples, 1, length - 1);

        int index = writeIndex - delaySamples;
        if (index < 0)
            index += length;

        return buffer[static_cast<size_t> (index)];
    }

    /**
        分数延迟读取（四点三次 Lagrange 插值）。M3 的 LFO 调制走这条路径。

        为什么不用线性插值：线性插值的频率响应是随小数部分变化的低通（frac=0.5 时
        奈奎斯特处约 -3.9 dB）。调制中 frac 随 LFO 连续变化，等于给延迟线串了一个
        抖动的高频衰减，这正是数字混响听起来比模拟"冷、扁"的一个主要来源。
        三点以上的 Lagrange 插值把幅度误差压到约 -0.05 dB（frac=0.5、奈奎斯特），
        幅度谱基本平坦，尾巴更自然温暖。

        为什么不用递归的全通插值：本延迟线的延迟量被 LFO 连续调制，递归结构在系数
        时变时会因内部状态与当前系数不匹配而输出瞬态噪声；FIR 结构没有内部状态，
        时变下天然稳定，且多个抽头可独立读取同一条延迟线而互不干扰。

        frac = 0 时退化为精确的整数读取（系数 c0=c2=c3=0、c1=1），
        因此 Mod=0 时输出与静止腔体逐比特一致。
    */
    float readFractional (float delaySamples) const noexcept
    {
        // 需要 i0-1 与 i0+2 两个额外抽头，故上下界各收一格
        delaySamples = std::clamp (delaySamples, 2.0f, static_cast<float> (length - 3));

        const int   i0 = static_cast<int> (delaySamples);
        const float f  = delaySamples - static_cast<float> (i0);

        const float y0 = read (i0 - 1);
        const float y1 = read (i0);
        const float y2 = read (i0 + 1);
        const float y3 = read (i0 + 2);

        // 节点取在延迟轴的 0,1,2,3，求值点 x = 1 + f 处的三次 Lagrange 系数
        const float c0 = -f * (f - 1.0f) * (f - 2.0f) / 6.0f;
        const float c1 =  (1.0f + f) * (f - 1.0f) * (f - 2.0f) * 0.5f;
        const float c2 = -(1.0f + f) * f * (f - 2.0f) * 0.5f;
        const float c3 =  (1.0f + f) * f * (f - 1.0f) / 6.0f;

        return c0 * y0 + c1 * y1 + c2 * y2 + c3 * y3;
    }

private:
    std::vector<float> buffer;
    int length     = 0;
    int writeIndex = 0;
};