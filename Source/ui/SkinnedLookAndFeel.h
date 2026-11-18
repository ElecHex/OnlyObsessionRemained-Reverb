#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ui/SkinBundle.h"

/**
    贴图驱动的 LookAndFeel（阶段三 3a）。

    两条原则：

    1. **逐槽位回退**：每个 draw 函数先尝试皮肤贴图，取不到就画**内置的矢量外观**。
       这层矢量外观按 `docs/ui-style-guide.md`（机甲座舱 HUD）实现：切角双层框、冷灰结构线、
       铜/琥珀只出现在仪表与数字上，圆角恒为 0。它也是"皮肤清单什么都没写"时的基线。

    2. **分工边界**：控件一律走 juce::Graphics（CPU 侧，贴图驱动）；动态 shader 只用于
       整幅背景层（3b，见 OnlyObsessionRemainedEditor 的 paint）。两套渲染体系不重叠，避免同一个控件
       被两种方式互相盖写。

    控件如何知道自己对应哪个皮肤元素：编辑器创建控件时打上 `skinId` 属性
    （见 OnlyObsessionRemainedEditor::applySkin）。旋钮下方的数值框是 Slider 的内部 Label，
    身份跟随父滑块。
*/
class SkinnedLookAndFeel final : public juce::LookAndFeel_V4
{
public:
    SkinnedLookAndFeel();

    /** 皮肤切换后重新读取颜色与字体（贴图是绘制时按需取用，无需刷新）。 */
    void refreshFromSkin();

    /**
        HUD 语义色：皮肤 `colors` 里写了同名 token 就用它，没写就用规范（docs/ui-style-guide.md §2.1）
        给的默认值。token 名见规范 §11 的调色板。编辑器画脸框也要用，所以放在 public。
    */
    static juce::Colour hudColour (const juce::String& token);

    /**
        画布外框（"脸框"）：外 L0 粗框 + 暗缝 + 内 L2 细框（规范 §3.1 的双层框）。

        cornerRadius > 0 的旧清单保持圆角单框，只把颜色换成结构线；否则画切角双层框。
        lineWidth 是外框线宽（清单的 background.outline.thickness）；<= 0 时用规范的 L0。
        编辑器画脸框要用，所以放 public。
    */
    static void drawHudFaceFrame (juce::Graphics&, juce::Rectangle<float> area,
                                  float cornerRadius, float lineWidth);

    void drawRotarySlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPosProportional, float rotaryStartAngle,
                           float rotaryEndAngle, juce::Slider&) override;

    void drawLinearSlider (juce::Graphics&, int x, int y, int width, int height,
                           float sliderPos, float minSliderPos, float maxSliderPos,
                           juce::Slider::SliderStyle, juce::Slider&) override;

    void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour& backgroundColour,
                               bool shouldDrawButtonAsHighlighted,
                               bool shouldDrawButtonAsDown) override;

    // 按钮文字/下拉框箭头与布局也自己画：V4 的折线箭头与居中文本不符合规范的楔块与左对齐
    void drawButtonText (juce::Graphics&, juce::TextButton&,
                         bool shouldDrawButtonAsHighlighted, bool shouldDrawButtonAsDown) override;

    void drawComboBox (juce::Graphics&, int width, int height, bool isButtonDown,
                       int buttonX, int buttonY, int buttonW, int buttonH,
                       juce::ComboBox&) override;

    void positionComboBoxText (juce::ComboBox&, juce::Label&) override;

    // V4 在弹出菜单上叠了一层蓝扫描线，与规范"不要霓虹/不要大面积高光"冲突
    void drawPopupMenuBackground (juce::Graphics&, int width, int height) override;

    void fillTextEditorBackground (juce::Graphics&, int width, int height, juce::TextEditor&) override;
    void drawTextEditorOutline (juce::Graphics&, int width, int height, juce::TextEditor&) override;

    void drawLabel (juce::Graphics&, juce::Label&) override;

    juce::Font getLabelFont (juce::Label&) override;
    juce::Font getTextButtonFont (juce::TextButton&, int buttonHeight) override;
    juce::Font getComboBoxFont (juce::ComboBox&) override;
    juce::Font getPopupMenuFont() override;

    /**
        画一个元素槽位的贴图（只套该槽位自己的变换）；槽位没有贴图时返回 false。
        元素级的旋转/缩放由编辑器套在控件本体上（Component::setTransform），不在这里重复施加。
    */
    static bool drawSlot (juce::Graphics&, const juce::String& elementId, const juce::String& slot,
                          juce::Rectangle<float> area, bool stretchFallback);

private:
    static const skin::Skin& skin();

    /**
        运行时的轨迹绑定：轨迹启用时把 target 的中心移到曲线在 t 处的位置。
        elementArea 是控件自身矩形（轨迹坐标的参照系），返回是否移动过。
    */
    static bool applyTravelToTarget (const juce::String& elementId, const juce::String& part,
                                     juce::Rectangle<float> elementArea, float t,
                                     juce::Rectangle<float>& target);

    /**
        把槽位贴图沿轨迹 t0…t1 一段铺开（轨道、已填充段用它）。

        贴图仍按整条行程的外接矩形展开，再用轨迹"带子"裁剪——这样 track 与 trackFill
        取到的是同一张纹理的对应部分，弯曲后也不会错位。
    */
    static bool drawSlotAlongTravel (juce::Graphics&, const juce::String& elementId,
                                     const juce::String& slot, const skin::Travel& travel,
                                     juce::Rectangle<float> elementArea,
                                     float thickness, float t0, float t1);

    /** 无贴图时用矢量画一条沿轨迹的推子槽（凹槽底 + 刻度 + 铜色已填充段）。 */
    static void drawVectorTravelTrack (juce::Graphics&, juce::Slider&,
                                       const skin::Travel& travel,
                                       juce::Rectangle<float> elementArea,
                                       float thickness, float travelT);

    /**
        HUD 双层面板：切角外框 + 暗缝 + 内框 + 内侧倒角高光（规范 §3.1 / §5）。

        state：0 = 常态、1 = 悬停（面提亮一档）、2 = 按下（面用近黑、去亮边，像"陷进去"）、
        3 = 凹槽 / 输入区常态（面用近黑，倒角方向反过来 —— 光从上方来，凹进去时亮边在下方）。
    */
    static void drawHudPanel (juce::Graphics&, juce::Rectangle<float> area, int state);

    /**
        旋钮的矢量外观：铜环圆形仪表（规范 §10.3）。

        外铜环 + 环上刻度 + 近黑玻璃面 + 内圈 + 铜高光值弧 + 琥珀指针。
        rotaryStart/EndAngle 与 pos 由 Slider 传来，和 V4 的旋钮角度语义一致。
    */
    static void drawHudKnob (juce::Graphics&, juce::Rectangle<float> area,
                             float rotaryStartAngle, float rotaryEndAngle, float pos);

    /**
        推子头（楔形切角块）。rotationRadians 是轨迹在该点的切线角，直轨时为 0；
        尺寸按控件短边自适应，所以竖推子与条形共用同一个头。
    */
    static void drawHudThumb (juce::Graphics&, juce::Point<float> centre,
                              float lengthAlongTrack, float widthAcrossTrack, float rotationRadians);

    /**
        画一张贴图，并按 transform 旋转 / 缩放 / 平移（恒等变换时就是一次普通绘制）。
        stretchFallback 是"这个槽位历史上怎么摆"的默认值：transform.fit 为空时用它，
        所以老皮肤不写 fit 时外观与以前完全一致。
        路径为空或解码失败返回 false（调用方回退到矢量画法）。
    */
    static bool drawImage (juce::Graphics&, const juce::String& assetPath,
                           juce::Rectangle<float> area, const skin::Transform& transform,
                           bool stretchFallback);
};

/**
    带轨迹取值的滑块。

    普通 Slider 的鼠标拖动只在一条直线上映射；控件配了 `travel` 之后，鼠标位置要先投影到
    曲线上最近的 t，再把 t 当比例换算成值。这样"轨道弯了，拖动也跟着弯"。

    没有配置轨迹时完全走 Slider 原逻辑，外观与行为都不变。
*/
class SkinnedSlider final : public juce::Slider
{
public:
    SkinnedSlider() = default;

    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;

private:
    /** 取当前控件用于拖动的轨迹：轨道优先，其次推子头。取不到返回 false。 */
    bool findDragTravel (skin::Travel& travel) const;

    /** 把鼠标位置投影到轨迹上并写回数值；没有轨迹时返回 false。 */
    bool setValueFromTravel (juce::Point<float> position);
};
