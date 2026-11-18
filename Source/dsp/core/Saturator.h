#pragma once

#include <cmath>

/**
    反馈路径的微饱和（无状态、对称、单调、有界）。

    用途：三个腔体的反馈回路在把信号写回延迟线之前过一道极轻的软削波，给长尾染上
    一点低阶奇次谐波（以三次为主），听感上更"暖、更厚"，同时把极端工况（异常素材、
    宿主参数跳变）下的环路增长钳住。

    曲线（先取绝对值，符号最后补回）：

        |x| ≤ kKnee   :  y = |x|                               —— 逐比特透明
        |x| >  kKnee  :  y = kKnee + H·tanh((|x| − kKnee)/H)   —— 渐近到 kKnee + H

    三条性质是刻意选的：
      1. 拐点处一阶连续（两侧导数都是 1），不会在拐点附近留下"硬拐"特有的高次失真；
      2. 严格单调 → 不产生额外的非谐波寄生分量；
      3. 有界（渐近到 kKnee + H = 2.0）→ 无论输入多大，写进环路的信号都 ≤ 2.0，
         配合环路增益本身 ≤ 1，数学上不可能自激。

    为什么拐点取 0.7：湿声电平标定后，正常素材在腔体反馈点上的峰值大多落在
    0.3–0.6；取 0.7 意味着**正常电平下完全不染色**，只有长尾叠加把电平推上去
    （或素材本身很热）时才逐渐起弯——正是硬件混响"推得越狠越暖"的行为。

    为什么用 tanh 而不是三次多项式：三次软削波必须在很宽的区间之外再接硬限幅才
    有界，会留下拐点；tanh 的饱和是渐近的、处处光滑，且超出拐点后的压缩"先轻后重"，
    形状与磁带/电子管的压缩曲线更接近。
*/
inline float softSaturate (float x) noexcept
{
    constexpr float kKnee     = 0.7f;   // 起弯点
    constexpr float kHeadroom = 1.3f;   // 起弯点以上的压缩余量（渐近线 = 0.7 + 1.3 = 2.0）

    const float a = std::abs (x);

    if (a <= kKnee)
        return x;

    const float y = kKnee + kHeadroom * std::tanh ((a - kKnee) / kHeadroom);

    return x > 0.0f ? y : -y;
}

/**
    把反馈点信号换算到统一驱动刻度后做微饱和，再换算回原刻度。

    为什么需要驱动量：三个腔体的反馈点电平相对「湿声输出」的刻度各不相同
    （Plate 的交叉点只是支路末端的 decayGain 倍、Room 走 Householder 混合、
    Spring 经过 48 级色散链），若直接套用 softSaturate 的 0.7 拐点，三类型的
    起弯电平会相差数倍。驱动量把它们统一到「与湿声输出同刻度」上，拐点 0.7
    于是统一表示为「湿声峰值约 −3 dBFS」，符合"推热了才变暖"的预期；
    具体数值由 offline 第 22 节的输入电平扫描实测标定。

    线性区严格可逆：|x·drive| ≤ 0.7 时 softSaturate 原样返回，乘除抵消，
    因此小信号（以及所有 IR 类测试）的精度不受影响。
*/
inline float saturateFeedback (float x, float drive) noexcept
{
    return softSaturate (x * drive) / drive;
}