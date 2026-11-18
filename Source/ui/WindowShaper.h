#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace skin
{
    /**
        异形窗口（3e）：把一张 alpha 蒙版变成**窗口区域**，交给操作系统裁剪。

        为什么用"区域"而不是逐像素 alpha：JUCE 只在「顶层窗口没有原生标题栏、且顶层组件
        non-opaque」时才走 per-pixel 分层窗口（见 juce_Windowing_windows.cpp 的
        computeTransparencyKind），而本插件的编辑器挂着 OpenGL 着色器上下文（直接画在窗口
        表面上），两者互斥。区域裁剪是硬边，但能与 GL 共存，VST3 侧也适用。
        想要羽化/半透明边，就得放弃 GL 背景通路——那是另一件事，不在本次范围内。

        非 Windows 平台：isSupported() 返回 false，调用方按"视觉异形"降级（只裁绘制，不动窗口）。
    */
    namespace window_shape
    {
        /** 当前平台能不能真的裁窗口区域。 */
        bool isSupported();

        /**
            把蒙版施加到 `target` 所在窗口上（区域按"alpha > 128 属于窗口，其余裁掉"生成）。

            @param target          承载窗口的那个组件：Standalone 传顶层窗口组件，
                                   VST3 传编辑器自己（它自己就是一个 peer）
            @param mask            蒙版图；按 maskArea 拉伸映射（宽高比不匹配就是拉伸）
            @param maskArea        蒙版在 target 本地坐标里对应的矩形（逻辑像素）
            @param extraArea       除蒙版区外还要保留的区域，例如标题栏条（逻辑像素；空 = 不要）

            @returns false = 没做成（平台不支持 / 没有 peer / 系统调用失败），调用方自行降级；
                     失败原因见 lastError()。成功时区域归操作系统所有，不要自己删。
        */
        bool applyToWindow (juce::Component& target, const juce::Image& mask,
                            juce::Rectangle<int> maskArea, juce::Rectangle<int> extraArea);

        /** 清掉窗口区域，回到普通的矩形窗口。 */
        void clearFromWindow (juce::Component& target);

        /** 上一次失败的原因（给状态条与设计器提示用）。 */
        juce::String lastError();
    }
}
