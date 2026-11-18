#include "ui/SkinnedLookAndFeel.h"

#include <cmath>
#include <limits>

namespace
{
    /** 控件 → 皮肤元素 id（编辑器在创建控件时打的 "skinId" 属性）。 */
    juce::String skinIdOf (const juce::Component& component)
    {
        const auto own = component.getProperties().getWithDefault ("skinId", juce::var()).toString();

        if (own.isNotEmpty())
            return own;

        // 旋钮下方的数值框是 Slider 的内部 Label，身份跟随父滑块
        if (auto* parent = component.getParentComponent())
            if (dynamic_cast<const juce::Slider*> (parent) != nullptr)
                return parent->getProperties().getWithDefault ("skinId", juce::var()).toString();

        return {};
    }

    juce::String fontNodeKey() { return "font"; }

    /** 轨迹 t0…t1 一段描边成一条"带子"，用来裁剪贴图或直接填充。 */
    juce::Path travelRibbonPath (const skin::Travel& travel, juce::Rectangle<float> area,
                                 float thickness, float t0, float t1)
    {
        juce::Path ribbon;

        const auto curve = travel.pathIn (area, t0, t1);

        if (curve.isEmpty())
            return ribbon;

        const juce::PathStrokeType stroke (juce::jmax (1.0f, thickness),
                                           juce::PathStrokeType::curved,
                                           juce::PathStrokeType::rounded);
        stroke.createStrokedPath (ribbon, curve);
        return ribbon;
    }

    /**
        只给推子头（或 overlay）配了轨迹时的直轨道。

        轨道本身没配曲线，但它仍然要按"轨道区域"这条直线画；用同一条 Travel 表达，
        绘制与取值就只有一个代码路径。
    */
    skin::Travel straightTravel (bool vertical)
    {
        skin::Travel t;
        t.enabled = true;

        if (vertical)
        {
            t.p0 = { 0.5f, 1.0f };
            t.p1 = { 0.5f, 2.0f / 3.0f };
            t.p2 = { 0.5f, 1.0f / 3.0f };
            t.p3 = { 0.5f, 0.0f };
        }
        else
        {
            t.p0 = { 0.0f, 0.5f };
            t.p1 = { 1.0f / 3.0f, 0.5f };
            t.p2 = { 2.0f / 3.0f, 0.5f };
            t.p3 = { 1.0f, 0.5f };
        }

        return t;
    }

    /** 双值、三值、IncDec 这类滑块没有单一拖动位置，不接管鼠标。 */
    bool isTravelDragStyle (juce::Slider::SliderStyle style)
    {
        switch (style)
        {
            case juce::Slider::IncDecButtons:
            case juce::Slider::TwoValueHorizontal:
            case juce::Slider::TwoValueVertical:
            case juce::Slider::ThreeValueHorizontal:
            case juce::Slider::ThreeValueVertical:
                return false;

            default:
                return true;
        }
    }

    // ===============================================================
    // HUD 调色板与几何语言（docs/ui-style-guide.md）
    //
    // 颜色只在"皮肤 colors 里没写这个 token"时用这里的规范值 —— 于是
    // 一张贴图、一句颜色都没写的皮肤也自带机甲座舱的基线外观。
    // ===============================================================

    struct HudToken { const char* name; juce::uint32 argb; };

    const HudToken kHudPalette[] =
    {
        { "void",        0xff050408 },   // 最深黑：缝隙、输入区
        { "base",        0xff0b0914 },   // 座舱底色
        { "panel",       0xff161327 },   // 面板主色
        { "panelLit",    0xff232040 },   // 受光面板 / 内嵌块
        { "structure",   0xff6e6a88 },   // 结构线、分隔线
        { "structureHi", 0xffb4b0c9 },   // 边缘高光、倒角亮边
        { "text",        0xffd8d4e6 },   // 主文字、数值
        { "textDim",     0xff8a86a0 },   // 次要标签、刻度
        // 下面三个原来是铜/琥珀（暖色强调），按要求统一改成白色。
        // 名字保留是为了不破坏皮肤清单里已有的 token 名；含义现在就是"白色强调色"。
        { "copper",      0xffffffff },   // 白色强调：仪表外圈、推子已填充段、推子头
        { "copperHi",    0xffffffff },   // 白色强调：值弧、铜高光
        { "amber",       0xffffffff },   // 白色强调：指针、数值
        { "alert",       0xffb22f45 },   // 警戒细线
    };

    // 线宽层级（规范 §3.2）。坐标一律逻辑像素：写死物理像素在 125% 缩放下会糊。
    constexpr float kLineL0       = 2.5f;
    constexpr float kLineL1       = 1.5f;
    constexpr float kLineL2       = 1.0f;

    /** 标签字距（规范 §4.2 的 +1~2px；JUCE 的字距系数是"字号的倍数"）。 */
    constexpr float kTracking     = 0.12f;

    /**
        推子头的轮廓比例：长 : 宽 ≈ 9.85 : 1（照参考图量出来的细长"刀刃"）。

        归一化轮廓（x 沿行程 0…1，y 跨行程 0…1）：
        尾尖 (0.00, 0.66)、上肩 (0.70, 0.00)、头尖 (1.00, 0.53)、下肩 (0.95, 1.00)，
        即：尾部收成尖、上半边先展开到七成处、下半边拖得更长，头端再收成尖。
    */
    constexpr float kThumbAspect = 1.0f / 9.85f;

    /**
        刀片相对"切线方向"的额外倾斜（15°）。

        目的：让推子头看起来是斜插在轨道上的，而不是严格顺着轨道贴上去 ——
        配合比轨道更宽的刀面，刀尖与刀尾都会越出轨道轮廓。
    */
    constexpr float kThumbTilt = juce::MathConstants<float>::pi / 12.0f;

    /** 切角长度只有三档（规范 §3.1：同一个界面里不超过 3 种）。 */
    float hudCutFor (juce::Rectangle<float> area)
    {
        const auto shortest = juce::jmin (area.getWidth(), area.getHeight());

        return shortest < 40.0f ? 3.0f : (shortest < 160.0f ? 6.0f : 10.0f);
    }

    /**
        双层框的暗缝。规范给的是 6–10px，但那是**面板**尺度：26px 高的按钮/下拉框
        照搬会把控件挖空，所以小控件按短边比例收缩，下限 2px。
    */
    float hudGapFor (juce::Rectangle<float> area)
    {
        return juce::jlimit (2.0f, 8.0f, juce::jmin (area.getWidth(), area.getHeight()) * 0.12f);
    }

    /** 45° 切角矩形（八个顶点）；规范的圆角恒为 0，所以这里没有 cornerSize 参数。 */
    juce::Path hudCutPath (juce::Rectangle<float> area, float cut)
    {
        const auto maxCut = juce::jmax (0.0f, juce::jmin (area.getWidth(), area.getHeight()) * 0.5f - 1.0f);
        const auto c      = juce::jlimit (0.0f, maxCut, cut);

        juce::Path p;
        p.startNewSubPath (area.getX() + c, area.getY());
        p.lineTo (area.getRight() - c, area.getY());
        p.lineTo (area.getRight(), area.getY() + c);
        p.lineTo (area.getRight(), area.getBottom() - c);
        p.lineTo (area.getRight() - c, area.getBottom());
        p.lineTo (area.getX() + c, area.getBottom());
        p.lineTo (area.getX(), area.getBottom() - c);
        p.lineTo (area.getX(), area.getY() + c);
        p.closeSubPath();
        return p;
    }

    /**
        推子槽上的一排刻度（规范 §3.3）：沿轨迹摆放、垂直于切线，每 5 根一个主刻度。

        轨迹的切线是"归一化坐标"里的导数，要乘上控件宽高才是真实方向 —— 否则
        非正方控件上的刻度会歪。
    */
    void drawHudTravelTicks (juce::Graphics& g, const skin::Travel& travel,
                             juce::Rectangle<float> elementArea, float spacing,
                             juce::Colour colour)
    {
        const auto span = juce::jmax (elementArea.getWidth(), elementArea.getHeight());

        if (span <= 1.0f || spacing <= 1.0f)
            return;

        const int count = juce::jlimit (2, 96, juce::roundToInt (span / spacing));

        g.setColour (colour);

        for (int i = 0; i <= count; ++i)
        {
            const float t = static_cast<float> (i) / static_cast<float> (count);
            const bool  major = (i % 5) == 0;
            const float len   = major ? 5.0f : 3.0f;

            const auto centre  = travel.pointIn (elementArea, t);
            auto       tangent = travel.tangentAt (t);
            tangent = { tangent.x * elementArea.getWidth(), tangent.y * elementArea.getHeight() };

            const auto magnitude = tangent.getDistanceFromOrigin();

            if (magnitude < 1.0e-6f)
                continue;

            const juce::Point<float> normal { -tangent.y / magnitude, tangent.x / magnitude };
            const auto half = normal * (len * 0.5f);

            g.drawLine ({ centre - half, centre + half }, kLineL2);
        }
    }
}

SkinnedLookAndFeel::SkinnedLookAndFeel()
{
    refreshFromSkin();
}

const skin::Skin& SkinnedLookAndFeel::skin()
{
    return skin::SkinBundle::getInstance().getCurrent();
}

juce::Colour SkinnedLookAndFeel::hudColour (const juce::String& token)
{
    for (const auto& entry : kHudPalette)
        if (token == entry.name)
            return skin().colourFor (juce::var (token), juce::Colour (entry.argb));

    return juce::Colours::transparentBlack;
}

void SkinnedLookAndFeel::refreshFromSkin()
{
    const auto& s = skin();

    // HUD 语义 token（规范 §11）：皮肤 colors 写了同名键就听皮肤，没写就用规范默认值
    const auto cVoid        = hudColour ("void");
    const auto cPanel       = hudColour ("panel");
    const auto cPanelLit    = hudColour ("panelLit");
    const auto cStructure   = hudColour ("structure");
    const auto cText        = hudColour ("text");
    const auto cTextDim     = hudColour ("textDim");
    const auto cCopper      = hudColour ("copper");
    const auto cCopperHi    = hudColour ("copperHi");
    const auto cAmber       = hudColour ("amber");
    const auto cAlert       = hudColour ("alert");

    // 颜色一律设在 LookAndFeel 上（而不是逐个控件），这样"皮肤换色"是全局一致的。
    //
    // 这里必须覆盖到**所有**会被控件取用的 id：V4/V3 的配色方案自带蓝
    // （Slider::thumbColourId 就是配色里的 defaultFill），漏掉一个就会从矢量回退里露出来。
    setColour (juce::ResizableWindow::backgroundColourId, s.backgroundColour);

    setColour (juce::Label::textColourId, cText);
    setColour (juce::Label::textWhenEditingColourId, cText);
    setColour (juce::Label::backgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Label::outlineColourId, juce::Colours::transparentBlack);

    setColour (juce::TextButton::buttonColourId, cPanel);
    setColour (juce::TextButton::buttonOnColourId, cPanelLit);
    setColour (juce::TextButton::textColourOffId, cText);
    setColour (juce::TextButton::textColourOnId, cAmber);

    setColour (juce::ToggleButton::textColourId, cText);
    setColour (juce::ToggleButton::tickColourId, cCopperHi);
    setColour (juce::ToggleButton::tickDisabledColourId, cTextDim);

    setColour (juce::ComboBox::backgroundColourId, cVoid);
    setColour (juce::ComboBox::textColourId, cText);
    setColour (juce::ComboBox::outlineColourId, cStructure);
    setColour (juce::ComboBox::focusedOutlineColourId, cAlert);
    setColour (juce::ComboBox::arrowColourId, cTextDim);
    setColour (juce::ComboBox::buttonColourId, cPanel);

    setColour (juce::PopupMenu::backgroundColourId, cPanel);
    setColour (juce::PopupMenu::textColourId, cText);
    setColour (juce::PopupMenu::headerTextColourId, cTextDim);
    // 高亮用一小块铜色而非饱和强调色：暖色合法的落点之一（规范 §2.2）
    setColour (juce::PopupMenu::highlightedBackgroundColourId, cCopper.withAlpha (0.30f));
    setColour (juce::PopupMenu::highlightedTextColourId, cText);

    setColour (juce::Slider::rotarySliderFillColourId, cCopper);
    setColour (juce::Slider::rotarySliderOutlineColourId, cStructure);
    setColour (juce::Slider::trackColourId, cVoid);
    setColour (juce::Slider::backgroundColourId, cVoid);
    setColour (juce::Slider::thumbColourId, cCopper);
    setColour (juce::Slider::textBoxTextColourId, cAmber);
    setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);
    setColour (juce::Slider::textBoxHighlightColourId, cCopper.withAlpha (0.30f));

    setColour (juce::TextEditor::textColourId, cText);
    setColour (juce::TextEditor::backgroundColourId, cVoid);
    setColour (juce::TextEditor::outlineColourId, cStructure);
    setColour (juce::TextEditor::focusedOutlineColourId, cAlert);
    setColour (juce::TextEditor::highlightColourId, cCopper.withAlpha (0.30f));
    setColour (juce::TextEditor::highlightedTextColourId, cText);

    setColour (juce::CaretComponent::caretColourId, cAmber);

    // 本 LookAndFeel 下还可能出现滚动条/列表/提示框（插件界面暂时没有，但换皮肤时
    // 这些 id 一旦没设就会掉回 V4 的默认色，包括那个蓝）——一起钉住
    setColour (juce::ScrollBar::backgroundColourId, cVoid);
    setColour (juce::ScrollBar::thumbColourId, cStructure);
    setColour (juce::ScrollBar::trackColourId, cPanel);

    setColour (juce::ListBox::backgroundColourId, cVoid);
    setColour (juce::ListBox::outlineColourId, cStructure);
    setColour (juce::ListBox::textColourId, cText);

    setColour (juce::TooltipWindow::backgroundColourId, cPanel);
    setColour (juce::TooltipWindow::textColourId, cText);
    setColour (juce::TooltipWindow::outlineColourId, cStructure);
}

bool SkinnedLookAndFeel::drawImage (juce::Graphics& g, const juce::String& assetPath,
                                    juce::Rectangle<float> area, const skin::Transform& transform,
                                    bool stretchFallback)
{
    if (assetPath.isEmpty() || area.isEmpty())
        return false;

    const auto image = skin().imageFor (assetPath);

    if (image.isNull())
        return false;

    // 变换要还原：贴图是叠着画的，若不还原，后面画的东西会一直带着这个旋转。
    const juce::Graphics::ScopedSaveState save (g);

    if (! transform.isIdentity())
        g.addTransform (transform.toAffine (area));

    g.drawImage (image, area, transform.placement (stretchFallback));

    return true;
}

bool SkinnedLookAndFeel::drawSlot (juce::Graphics& g, const juce::String& id, const juce::String& slot,
                                   juce::Rectangle<float> area, bool stretchFallback)
{
    const auto& s = skin();

    return drawImage (g, s.assetFor (id, slot), area, s.transformForSlot (id, slot), stretchFallback);
}

bool SkinnedLookAndFeel::drawSlotAlongTravel (juce::Graphics& g, const juce::String& id,
                                              const juce::String& slot, const skin::Travel& travel,
                                              juce::Rectangle<float> elementArea,
                                              float thickness, float t0, float t1)
{
    if (! travel.enabled || elementArea.isEmpty())
        return false;

    const auto& s = skin();
    const auto assetPath = s.assetFor (id, slot);

    if (assetPath.isEmpty())
        return false;

    const auto image = s.imageFor (assetPath);

    if (image.isNull() || t1 - t0 < 1.0e-4f)
        return false;

    const auto segment = travelRibbonPath (travel, elementArea, thickness, t0, t1);

    if (segment.isEmpty())
        return false;

    // 纹理按整条行程展开，裁剪到这一段：track 与 trackFill 因此共用同一套纹理坐标
    const auto fullRibbon  = travelRibbonPath (travel, elementArea, thickness, 0.0f, 1.0f);
    const auto textureArea = fullRibbon.getBounds().expanded (1.0f);

    const juce::Graphics::ScopedSaveState save (g);
    g.reduceClipRegion (segment);

    return drawImage (g, assetPath, textureArea, s.transformForSlot (id, slot), true);
}

void SkinnedLookAndFeel::drawVectorTravelTrack (juce::Graphics& g, juce::Slider& slider,
                                                const skin::Travel& travel,
                                                juce::Rectangle<float> elementArea,
                                                float thickness, float travelT)
{
    // 轨道颜色已经不由 Slider 的 colour id 决定（见 refreshFromSkin 的说明），
    // 这里统一读 HUD token；参数留着是为了与贴图轨道共用同一处调用签名
    juce::ignoreUnused (slider);

    if (! travel.enabled || elementArea.isEmpty())
        return;

    const float slot = juce::jmax (3.0f, thickness);
    const float fill = juce::jmax (1.0f, slot - 2.0f * kLineL2);   // 槽内壁各留 1px 暗边

    const juce::PathStrokeType slotStroke (slot, juce::PathStrokeType::curved, juce::PathStrokeType::butt);
    const juce::PathStrokeType fillStroke (fill, juce::PathStrokeType::curved, juce::PathStrokeType::butt);

    // 嵌入式凹槽：先用结构线描一道略宽的底，再填近黑 —— 两侧各露 1px 结构线，读起来是"嵌进去的缝"
    g.setColour (hudColour ("structure").withAlpha (0.55f));
    g.strokePath (travel.pathIn (elementArea, 0.0f, 1.0f), slotStroke);

    g.setColour (hudColour ("void"));
    g.strokePath (travel.pathIn (elementArea, 0.0f, 1.0f), fillStroke);

    // 刻度贴着槽走（规范 §3.3：刻度永远贴着结构边，不要漂在空白里）
    drawHudTravelTicks (g, travel, elementArea, 7.0f, hudColour ("structure").withAlpha (0.8f));

    const float t = juce::jlimit (0.0f, 1.0f, travelT);

    if (t <= 0.0f)
        return;

    g.setColour (hudColour ("copper"));
    g.strokePath (travel.pathIn (elementArea, 0.0f, t), fillStroke);

    // 填充前端压一道铜高光：指到哪读到哪，弯轨上也成立
    const auto tip = travel.pointIn (elementArea, t);
    auto       tan = travel.tangentAt (t);
    tan = { tan.x * elementArea.getWidth(), tan.y * elementArea.getHeight() };

    if (const auto magnitude = tan.getDistanceFromOrigin(); magnitude > 1.0e-6f)
    {
        const juce::Point<float> normal { -tan.y / magnitude, tan.x / magnitude };
        const auto half = normal * (fill * 0.5f);

        g.setColour (hudColour ("copperHi"));
        g.drawLine ({ tip - half, tip + half }, kLineL2);
    }
}

void SkinnedLookAndFeel::drawHudFaceFrame (juce::Graphics& g, juce::Rectangle<float> area,
                                           float cornerRadius, float lineWidth)
{
    if (area.getWidth() < 12.0f || area.getHeight() < 12.0f)
        return;

    const auto outer = hudColour ("structure");
    const auto inner = hudColour ("structure").withAlpha (0.5f);
    const auto width = lineWidth > 0.0f ? lineWidth : kLineL0;

    // 旧清单写了 cornerRadius：几何保持不变（圆角单框），只把颜色换成结构线
    if (cornerRadius > 0.0f)
    {
        g.setColour (outer);
        g.drawRoundedRectangle (area.reduced (0.75f), cornerRadius, width);
        return;
    }

    // 默认（cornerRadius = 0）：切角双层框 —— 外粗框 + 暗缝 + 内 L2 细框
    g.setColour (outer);
    g.strokePath (hudCutPath (area.reduced (0.75f), hudCutFor (area)), juce::PathStrokeType (width));

    const auto innerArea = area.reduced (0.75f + hudGapFor (area));

    if (innerArea.getWidth() > 8.0f && innerArea.getHeight() > 8.0f)
    {
        g.setColour (inner);
        g.strokePath (hudCutPath (innerArea, hudCutFor (innerArea)), juce::PathStrokeType (kLineL2));
    }
}

void SkinnedLookAndFeel::drawHudPanel (juce::Graphics& g, juce::Rectangle<float> area, int state)
{
    if (area.getWidth() < 3.0f || area.getHeight() < 3.0f)
        return;

    // 0.5px 内缩：让 L1/L2 的描边落在像素边界上，125% 缩放下才不会发毛
    const auto face    = area.reduced (0.5f);
    const auto cut     = hudCutFor (face);
    const auto path    = hudCutPath (face, cut);

    const bool isDown  = state == 2;
    const bool isOver  = state == 1;
    const bool isInset = state == 3;   // 凹槽 / 输入区：面是近黑，倒角方向反过来

    g.setColour (isDown || isInset ? hudColour ("void")
                                   : (isOver ? hudColour ("panelLit") : hudColour ("panel")));
    g.fillPath (path);

    g.setColour (isOver ? hudColour ("structureHi") : hudColour ("structure"));
    g.strokePath (path, juce::PathStrokeType (kLineL1));

    // 外框—暗缝—内框（规范 §3.1 的双层框；间距按控件尺寸收，见 hudGapFor）
    const auto inner = face.reduced (hudGapFor (face));

    if (inner.getWidth() > 6.0f && inner.getHeight() > 6.0f)
    {
        g.setColour (hudColour ("void").withAlpha (0.85f));
        g.strokePath (hudCutPath (inner, hudCutFor (inner)), juce::PathStrokeType (kLineL2));

        g.setColour (hudColour ("structure").withAlpha (0.5f));
        g.strokePath (hudCutPath (inner.reduced (1.0f), hudCutFor (inner)), juce::PathStrokeType (kLineL2));
    }

    // 倒角（规范 §5：光从上方来）：凸起是"亮边在上/左、暗边在下"，凹进去正好反过来。
    // 按下态去掉亮边，做出"陷进去"。
    const juce::Line<float> topEdge    { face.getTopLeft() + juce::Point<float> (cut, kLineL2),
                                       face.getTopRight() - juce::Point<float> (cut, 0.0f) };
    const juce::Line<float> leftEdge   { face.getTopLeft() + juce::Point<float> (0.0f, cut),
                                       face.getBottomLeft() - juce::Point<float> (0.0f, cut) };
    const juce::Line<float> bottomEdge { face.getBottomLeft() + juce::Point<float> (cut, 0.0f),
                                       face.getBottomRight() - juce::Point<float> (cut, kLineL2) };

    const auto brightColour = hudColour ("structureHi").withAlpha (0.55f);
    const auto darkColour   = hudColour ("void").withAlpha (0.9f);

    if (isInset)
    {
        g.setColour (darkColour);
        g.drawLine (topEdge, kLineL2);
        g.drawLine (leftEdge, kLineL2);

        g.setColour (brightColour);
        g.drawLine (bottomEdge, kLineL2);
        return;
    }

    g.setColour (darkColour);
    g.drawLine (bottomEdge, kLineL2);

    if (! isDown)
    {
        g.setColour (brightColour);
        g.drawLine (topEdge, kLineL2);
        g.drawLine (leftEdge, kLineL2);
    }
}

void SkinnedLookAndFeel::drawHudKnob (juce::Graphics& g, juce::Rectangle<float> area,
                                      float rotaryStartAngle, float rotaryEndAngle, float pos)
{
    if (area.isEmpty())
        return;

    // 与 V4 旋钮同尺度（它也是从矩形中心缩 10px），所以换成 HUD 外观后大小不跳
    const auto radius = juce::jmin (area.getWidth(), area.getHeight()) * 0.5f - 10.0f;

    if (radius < 8.0f)
        return;

    const auto centre = area.getCentre();
    const auto start  = rotaryStartAngle;
    const auto end    = rotaryEndAngle;
    const auto angle  = start + juce::jlimit (0.0f, 1.0f, pos) * (end - start);

    // 角度约定与 JUCE 一致：0 = 正上方、顺时针为正
    const auto direction = [centre] (float radians)
    {
        return centre + juce::Point<float> (std::sin (radians), -std::cos (radians));
    };

    // 1) 铜外环（L1）—— 仪表的外圈
    {
        juce::Path ring;
        ring.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, start, end, true);

        g.setColour (hudColour ("copper"));
        g.strokePath (ring, juce::PathStrokeType (kLineL1, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::butt));
    }

    // 2) 环上刻度：每 6° 一根（规范 §3.3 的 3–6°），每 30° 一根主刻度并加长
    {
        const auto stepRadians = juce::MathConstants<float>::pi / 30.0f;
        const int  count       = juce::jmax (1, juce::roundToInt ((end - start) / stepRadians));

        g.setColour (hudColour ("textDim"));

        for (int i = 0; i <= count; ++i)
        {
            const float a       = start + (end - start) * static_cast<float> (i) / static_cast<float> (count);
            const float outer   = radius - 3.0f;
            const float tickLen = (i % 5) == 0 ? 4.5f : 3.0f;
            const auto  outerPt = direction (a) - centre;   // 单位方向

            g.drawLine ({ centre + outerPt * outer, centre + outerPt * (outer - tickLen) }, kLineL2);
        }
    }

    const auto glassRadius = radius - 10.0f;

    // 3) 近黑玻璃面 + 内圈（规范 §5「内圈是近黑的玻璃」）
    if (glassRadius > 4.0f)
    {
        g.setColour (hudColour ("void"));
        g.fillEllipse (juce::Rectangle<float> (glassRadius * 2.0f, glassRadius * 2.0f).withCentre (centre));

        g.setColour (hudColour ("panelLit"));
        g.drawEllipse (juce::Rectangle<float> ((glassRadius - 2.0f) * 2.0f,
                                               (glassRadius - 2.0f) * 2.0f).withCentre (centre),
                       kLineL2);
    }

    // 4) 值弧：压在环上的更亮一段。环与值弧现在都是白色，靠**更粗**来分辨（2.6px vs 1.5px）
    if (angle > start + 1.0e-4f)
    {
        juce::Path valueArc;
        valueArc.addCentredArc (centre.x, centre.y, radius, radius, 0.0f, start, angle, true);

        g.setColour (hudColour ("copperHi"));
        g.strokePath (valueArc, juce::PathStrokeType (2.6f, juce::PathStrokeType::curved,
                                                      juce::PathStrokeType::butt));
    }

    // 5) 指针：琥珀色短线 + 尖端小点（规范 §2.2「暖色只许出现在仪表、数字、指针」）
    {
        const auto dir = direction (angle) - centre;
        const auto tip = centre + dir * (radius - 4.5f);

        // 辉光半径 ≤3px：规范的 bloom 只给铜环与仪表数字
        g.setColour (hudColour ("amber").withAlpha (0.18f));
        g.fillEllipse (juce::Rectangle<float> (6.0f, 6.0f).withCentre (tip));

        g.setColour (hudColour ("amber"));
        g.drawLine ({ centre + dir * (radius - 16.0f), tip }, kLineL1);
        g.fillEllipse (juce::Rectangle<float> (4.0f, 4.0f).withCentre (tip));
    }
}

void SkinnedLookAndFeel::drawHudThumb (juce::Graphics& g, juce::Point<float> centre,
                                       float lengthAlongTrack, float widthAcrossTrack,
                                       float rotationRadians)
{
    if (lengthAlongTrack <= 1.0f || widthAcrossTrack <= 1.0f)
        return;

    const auto box = juce::Rectangle<float> (lengthAlongTrack, widthAcrossTrack).withCentre (centre);

    const juce::Graphics::ScopedSaveState save (g);

    // 弯轨上不跟着切线转会看着"断开"（设计器里把 track 拉成 S 形就能看出来）
    if (std::fabs (rotationRadians) > 1.0e-4f)
        g.addTransform (juce::AffineTransform::rotation (rotationRadians, centre.x, centre.y));

    // 参考图的"刀刃"轮廓：尾尖 → 上肩 → 头尖 → 下肩（比例见 kThumbAspect）。
    // 长边沿行程，所以滑块在弯轨上时这枚刀片也跟着切线摆。
    const auto at = [&box] (float nx, float ny)
    {
        return juce::Point<float> (box.getX() + nx * box.getWidth(),
                                   box.getY() + ny * box.getHeight());
    };

    juce::Path blade;
    blade.startNewSubPath (at (0.00f, 0.66f));
    blade.lineTo (at (0.70f, 0.00f));
    blade.lineTo (at (1.00f, 0.53f));
    blade.lineTo (at (0.95f, 1.00f));
    blade.closeSubPath();

    // 白色刀片 + 一圈近黑描边：已填充段也是白的，没有描边就分不出推子头在哪
    g.setColour (hudColour ("copper"));
    g.fillPath (blade);

    g.setColour (hudColour ("void"));
    g.strokePath (blade, juce::PathStrokeType (kLineL2));
}

bool SkinnedLookAndFeel::applyTravelToTarget (const juce::String& elementId, const juce::String& part,
                                              juce::Rectangle<float> elementArea, float t,
                                              juce::Rectangle<float>& target)
{
    const auto travel = skin().travelForPart (elementId, part);

    if (! travel.enabled || elementArea.isEmpty())
        return false;

    target.setCentre (travel.pointIn (elementArea, juce::jlimit (0.0f, 1.0f, t)));
    return true;
}

void SkinnedLookAndFeel::drawRotarySlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPosProportional, float rotaryStartAngle,
                                           float rotaryEndAngle, juce::Slider& slider)
{
    const auto id     = skinIdOf (slider);
    const auto area   = slider.getLocalBounds().toFloat();
    const auto travel = skin().travelForPart (id, "filmstrip");
    const float travelT = juce::jlimit (0.0f, 1.0f, sliderPosProportional);

    const auto image = skin().imageFor (skin().assetFor (id, "filmstrip"));

    if (image.isNull())
    {
        const auto knobRect = juce::Rectangle<int> (x, y, width, height).toFloat();

        if (! travel.enabled)
        {
            drawHudKnob (g, knobRect, rotaryStartAngle, rotaryEndAngle, sliderPosProportional);
            return;
        }

        // 矢量旋钮：铜环仪表以矩形中心为圆心，整体平移到轨迹点上就能沿曲线走
        const auto knobCentre = knobRect.getCentre();
        const auto offset     = travel.pointIn (area, travelT) - knobCentre;

        const juce::Graphics::ScopedSaveState save (g);
        g.addTransform (juce::AffineTransform::translation (offset.x, offset.y));

        drawHudKnob (g, knobRect, rotaryStartAngle, rotaryEndAngle, sliderPosProportional);
        return;
    }

    // 贴图只画在"旋钮区"里：数值框占住的那块要留出来，否则旋钮图会压住数字
    const auto layout = getSliderLayout (slider);

    auto target = layout.sliderBounds.toFloat();

    if (target.isEmpty())
        target = juce::Rectangle<float> (static_cast<float> (x), static_cast<float> (y),
                                         static_cast<float> (width), static_cast<float> (height));

    const auto framesRaw = skin().paramFor (id, "frames", juce::var (64)).toString().getIntValue();
    const int  frames    = juce::jlimit (1, 512, framesRaw);

    const auto layoutName = skin().paramFor (id, "layout", juce::var ("vertical")).toString();
    const bool vertical   = ! layoutName.startsWithIgnoreCase ("h");

    const int frameW = vertical ? image.getWidth() : image.getWidth() / frames;
    const int frameH = vertical ? image.getHeight() / frames : image.getHeight();

    if (frameW <= 0 || frameH <= 0)
    {
        // 贴图帧切不出来（尺寸写错等）：退回矢量仪表，别画一半
        drawHudKnob (g, juce::Rectangle<int> (x, y, width, height).toFloat(),
                     rotaryStartAngle, rotaryEndAngle, sliderPosProportional);
        return;
    }

    const int index = juce::jlimit (0, frames - 1,
                                    juce::roundToInt (juce::jlimit (0.0f, 1.0f, sliderPosProportional)
                                                      * static_cast<float> (frames - 1)));

    const auto frame = vertical
        ? image.getClippedImage ({ 0, index * frameH, frameW, frameH })
        : image.getClippedImage ({ index * frameW, 0, frameW, frameH });

    // 轨迹以控件自身矩形为参照系；编辑层画的就是同一个矩形里的同一条曲线。
    // 帧条是一张子图，不能走 drawSlot（它自己去取整张图），所以这里手动套同一个"槽位级变换"语义
    {
        auto frameTarget = target;
        applyTravelToTarget (id, "filmstrip", area, travelT, frameTarget);

        const auto t = skin().transformForSlot (id, "filmstrip");

        const juce::Graphics::ScopedSaveState save (g);

        if (! t.isIdentity())
            g.addTransform (t.toAffine (frameTarget));

        g.drawImage (frame, frameTarget, t.placement (false));
    }

    // 可选：叠在旋钮中心的装饰（指针、logo、发光环等）
    auto overlayTarget = target;
    applyTravelToTarget (id, "overlay", area, travelT, overlayTarget);

    drawSlot (g, id, "overlay", overlayTarget, false);
}

void SkinnedLookAndFeel::drawLinearSlider (juce::Graphics& g, int x, int y, int width, int height,
                                           float sliderPos, float minSliderPos, float maxSliderPos,
                                           juce::Slider::SliderStyle style, juce::Slider& slider)
{
    const auto id = skinIdOf (slider);

    const bool vertical = style == juce::Slider::LinearVertical
                       || style == juce::Slider::LinearBarVertical;
    const bool bar = style == juce::Slider::LinearBar
                  || style == juce::Slider::LinearBarVertical;

    // 轨迹坐标一律以控件自身矩形为参照系：设计器里画的就是这个矩形里的同一条曲线
    const auto elementArea = slider.getLocalBounds().toFloat();

    const auto trackTravel   = skin().travelForPart (id, "track");
    const auto overlayTravel = skin().travelForPart (id, "overlay");

    // 推子头：按下 → 悬停 → 常态；部位轨迹按同一优先级回落
    juce::String stateSlot = "thumb";

    if (slider.isMouseButtonDown())          stateSlot = "thumbDown";
    else if (slider.isMouseOverOrDragging()) stateSlot = "thumbOver";

    auto thumbTravel = skin().travelForPart (id, stateSlot);

    if (! thumbTravel.enabled && ! thumbTravel.partDisabled && stateSlot != "thumb")
        thumbTravel = skin().travelForPart (id, "thumb");

    // 只配了轨道的曲线：推子头跟着轨道走（部位被显式关掉时不回落）
    if (! thumbTravel.enabled && ! thumbTravel.partDisabled)
        thumbTravel = trackTravel;

    const bool hasTrackImage = skin().assetFor (id, "track").isNotEmpty();

    // 这里原本有一条"既没有轨道贴图、也没有轨迹 → 交给 LookAndFeel_V4"的早返回。
    // 现在不再走 V4：它的轨道/推子头用的是配色方案里的蓝色（V4 的 trackColourId /
    // thumbColourId 就是 defaultFill / highlightedFill），与规范的调色板冲突。
    // 没有轨迹时下面用 straightTravel() 兜住，画法与贴图轨道共用同一处代码。

    auto area = juce::Rectangle<float> (static_cast<float> (x), static_cast<float> (y),
                                        static_cast<float> (width), static_cast<float> (height));

    // 行程：轨道两端各留 trackInset 像素。推子"行程"= 控件长边 − 2×trackInset，
    // 因此改控件矩形的长边或改这个内缩都能调行程。
    const auto inset = static_cast<int> (skin().paramFor (id, "trackInset", juce::var (0))
                                              .toString().getIntValue());
    auto trackArea = vertical ? area.reduced (0.0f, static_cast<float> (inset))
                              : area.reduced (static_cast<float> (inset), 0.0f);

    if (trackArea.isEmpty())
        trackArea = area;

    const auto trackThickness = vertical ? trackArea.getWidth() : trackArea.getHeight();

    // 矢量回退用的线宽：贴图轨道铺满控件宽度，纯矢量时细一点更像"轨道"
    const auto vectorThickness = juce::jlimit (2.0f, 12.0f,
                                               juce::jmin (elementArea.getWidth(),
                                                           elementArea.getHeight()) * 0.16f);

    // 行程比例：从滑块的绘制区域直接换算。
    // 不要用 minSliderPos / maxSliderPos——这两个值只有 min/max 两端都显式设过才有效，
    // 否则会双双等于轨道起点（span 为 0），V4 自己的外观也不用它们。
    juce::ignoreUnused (minSliderPos, maxSliderPos);

    const float travelSpan = vertical ? static_cast<float> (height)
                                      : static_cast<float> (width);
    const float travelOrigin = vertical ? static_cast<float> (y)
                                        : static_cast<float> (x);

    // 竖直要翻一下：JUCE 的 sliderPos 是"从上往下"量（getLinearSliderPos 里做了 1 - proportion），
    // 而轨迹的 t=0 在小端（竖直轨迹的底端），不翻的话值越大推子头越往下、填充也反过来。
    const float travelT = travelSpan > 1.0e-6f
                            ? juce::jlimit (0.0f, 1.0f, vertical
                                                ? 1.0f - (sliderPos - travelOrigin) / travelSpan
                                                : (sliderPos - travelOrigin) / travelSpan)
                            : 0.0f;

    // 轨道
    if (hasTrackImage)
    {
        if (! trackTravel.enabled || ! drawSlotAlongTravel (g, id, "track", trackTravel,
                                                            elementArea, trackThickness, 0.0f, 1.0f))
            drawSlot (g, id, "track", trackArea, true);
    }
    else if (trackTravel.enabled)
    {
        drawVectorTravelTrack (g, slider, trackTravel, elementArea, vectorThickness, travelT);
    }
    else
    {
        // 轨道本身没配曲线，但仍要按轨道区域这条直线画（与贴图路径共用同一套绘制代码）
        drawVectorTravelTrack (g, slider, straightTravel (vertical), trackArea,
                               vectorThickness, travelT);
    }

    // 已填充段：从行程起点画到推子头当前位置
    if (skin().assetFor (id, "trackFill").isNotEmpty())
    {
        if (trackTravel.enabled)
        {
            drawSlotAlongTravel (g, id, "trackFill", trackTravel, elementArea,
                                 trackThickness, 0.0f, travelT);
        }
        else
        {
            auto fillArea = vertical ? trackArea.withTop (sliderPos)
                                     : trackArea.withRight (sliderPos);

            fillArea = fillArea.getIntersection (trackArea);

            if (! fillArea.isEmpty())
                drawSlot (g, id, "trackFill", fillArea, true);
        }
    }

    // 推子头位置：轨迹启用时取曲线在 t 处的点，否则仍按直线映射
    const auto thumbCentre = thumbTravel.enabled
        ? thumbTravel.pointIn (elementArea, travelT)
        : (vertical ? juce::Point<float> (trackArea.getCentreX(), sliderPos)
                    : juce::Point<float> (sliderPos, trackArea.getCentreY()));

    // 贴图：状态槽位缺图时退回常态；尺寸取贴图原始尺寸，居中在行程位置上
    auto imageSlot = stateSlot;
    auto thumbPath = skin().assetFor (id, imageSlot);

    if (thumbPath.isEmpty())
    {
        imageSlot = "thumb";
        thumbPath = skin().assetFor (id, "thumb");
    }

    if (thumbPath.isNotEmpty())
    {
        if (const auto thumb = skin().imageFor (thumbPath); ! thumb.isNull())
        {
            auto thumbArea = juce::Rectangle<float> (static_cast<float> (thumb.getWidth()),
                                                      static_cast<float> (thumb.getHeight()))
                                 .withCentre (thumbCentre);

            // 推子头要按贴图原始尺寸定位，drawSlot 拿不到这个尺寸，所以这里同样手动套变换
            const auto t = skin().transformForSlot (id, imageSlot);
            const juce::Graphics::ScopedSaveState save (g);

            if (! t.isIdentity())
                g.addTransform (t.toAffine (thumbArea));

            g.drawImage (thumb, thumbArea, t.placement (false));
        }
    }
    else if (! bar && (thumbTravel.enabled || ! hasTrackImage))
    {
        // 没有推子头贴图时的矢量回退：细长"刀刃"（参考图轮廓，见 kThumbAspect）。
        // 尺寸比之前放大一档，且**刀面宽度刻意超过轨道**（1.9× 轨宽），看起来像斜插在槽里。
        const auto longest = juce::jmax (elementArea.getWidth(), elementArea.getHeight());
        const auto length  = juce::jlimit (22.0f, 80.0f, longest * 0.42f);
        const auto across  = juce::jlimit (4.0f, 22.0f,
                                           juce::jmax (length * kThumbAspect, vectorThickness * 1.9f));

        // 朝向：有轨迹就跟切线（弯轨上不转会看着"断开"），没轨迹就按滑块是竖的还是横的
        float rotation = vertical ? -juce::MathConstants<float>::halfPi : 0.0f;

        if (thumbTravel.enabled)
        {
            auto tangent = thumbTravel.tangentAt (travelT);
            tangent = { tangent.x * elementArea.getWidth(), tangent.y * elementArea.getHeight() };

            // 让刀片的长边对齐切线：JUCE 的旋转在屏幕坐标里是顺时针为正
            rotation = std::atan2 (tangent.y, tangent.x);
        }

        // 在"对齐切线"之上再单独斜 15°：视觉上是斜插进轨道，刀尖与刀尾都会越出轨道轮廓
        drawHudThumb (g, thumbCentre, length, across, rotation + kThumbTilt);
    }

    // 可选：叠一张整体装饰（刻度、发光条等）
    auto overlayArea = area;

    if (overlayTravel.enabled)
        overlayArea.setCentre (overlayTravel.pointIn (elementArea, travelT));

    drawSlot (g, id, "overlay", overlayArea, false);
}

void SkinnedLookAndFeel::drawButtonBackground (juce::Graphics& g, juce::Button& button,
                                               const juce::Colour& backgroundColour,
                                               bool shouldDrawButtonAsHighlighted,
                                               bool shouldDrawButtonAsDown)
{
    const auto id   = skinIdOf (button);
    const auto area = button.getLocalBounds().toFloat();

    // 状态优先级：开关态 → 按下 → 悬停 → 常态；具体状态缺图时退回常态贴图
    juce::String state = "normal";

    if (button.getToggleState())                 state = "toggled";
    else if (shouldDrawButtonAsDown)             state = "down";
    else if (shouldDrawButtonAsHighlighted)      state = "over";

    if (drawSlot (g, id, state, area, false))
        return;

    if (state != "normal" && drawSlot (g, id, "normal", area, false))
        return;

    // 矢量回退：切角双层面板（规范 §3.1）。开关态用"受光面板"表示选中
    const int panelState = button.getToggleState() ? 1
                         : (shouldDrawButtonAsDown ? 2
                         : (shouldDrawButtonAsHighlighted ? 1 : 0));

    drawHudPanel (g, area, panelState);

    juce::ignoreUnused (backgroundColour);
}

void SkinnedLookAndFeel::drawButtonText (juce::Graphics& g, juce::TextButton& button,
                                         bool shouldDrawButtonAsHighlighted,
                                         bool shouldDrawButtonAsDown)
{
    // 排版照抄 V2（缩进与行数限制），只把文案统一大写（规范 §4.2）
    const auto font = getTextButtonFont (button, button.getHeight());

    g.setFont (font);
    g.setColour (button.findColour (button.getToggleState() ? juce::TextButton::textColourOnId
                                                            : juce::TextButton::textColourOffId)
                       .withMultipliedAlpha (button.isEnabled() ? 1.0f : 0.5f));

    const int yIndent     = juce::jmin (4, button.proportionOfHeight (0.3f));
    const int cornerSize  = juce::jmin (button.getHeight(), button.getWidth()) / 2;
    const int fontHeight  = juce::roundToInt (font.getHeight() * 0.6f);
    const int leftIndent  = juce::jmin (fontHeight, 2 + cornerSize / (button.isConnectedOnLeft() ? 4 : 2));
    const int rightIndent = juce::jmin (fontHeight, 2 + cornerSize / (button.isConnectedOnRight() ? 4 : 2));
    const int textWidth   = button.getWidth() - leftIndent - rightIndent;

    if (textWidth > 0)
        g.drawFittedText (button.getButtonText().toUpperCase(),
                          leftIndent, yIndent, textWidth, button.getHeight() - yIndent * 2,
                          juce::Justification::centred, 1);

    juce::ignoreUnused (shouldDrawButtonAsHighlighted, shouldDrawButtonAsDown);
}

void SkinnedLookAndFeel::drawComboBox (juce::Graphics& g, int width, int height, bool isButtonDown,
                                       int buttonX, int buttonY, int buttonW, int buttonH,
                                       juce::ComboBox& box)
{
    const auto id = skinIdOf (box);

    if (drawSlot (g, id, "background", box.getLocalBounds().toFloat(), true))
    {
        // 注意用 juce::String：写成 const char* 会退化成指针比较（MSVC C4130）
        const juce::String arrowSlot = isButtonDown ? "arrowDown" : "arrow";

        if (const auto arrowArea = juce::Rectangle<int> (buttonX, buttonY, buttonW, buttonH).toFloat();
            ! drawSlot (g, id, arrowSlot, arrowArea, false) && arrowSlot != "arrow")
            drawSlot (g, id, "arrow", arrowArea, false);

        return;
    }

    // 矢量回退：凹进去的输入区 + 一条竖缝分开的箭头区（规范 §3.1 的楔块按钮）
    drawHudPanel (g, juce::Rectangle<int> (0, 0, width, height).toFloat(), 3);

    const auto arrowArea = juce::Rectangle<int> (buttonX, buttonY, buttonW, buttonH).toFloat();

    if (arrowArea.getWidth() < 3.0f || arrowArea.getHeight() < 3.0f)
        return;

    g.setColour (hudColour ("structure").withAlpha (0.7f));
    g.drawLine ({ arrowArea.getX(), arrowArea.getY() + 3.0f,
                  arrowArea.getX(), arrowArea.getBottom() - 3.0f }, kLineL2);

    const auto cx        = arrowArea.getCentreX();
    const auto cy        = arrowArea.getCentreY();
    const auto halfWidth = juce::jmin (4.5f, arrowArea.getWidth() * 0.32f);

    juce::Path wedge;
    wedge.startNewSubPath (cx - halfWidth, cy - halfWidth * 0.6f);
    wedge.lineTo (cx + halfWidth, cy - halfWidth * 0.6f);
    wedge.lineTo (cx, cy + halfWidth * 0.9f);
    wedge.closeSubPath();

    g.setColour (isButtonDown ? hudColour ("structureHi") : hudColour ("structure"));
    g.fillPath (wedge);

    // 内套一小圈近黑描边：楔块也做"亮面 + 暗描边"（规范 §3.1）
    g.setColour (hudColour ("void"));
    g.strokePath (wedge, juce::PathStrokeType (kLineL2));
}

void SkinnedLookAndFeel::positionComboBoxText (juce::ComboBox& box, juce::Label& label)
{
    // 规范 §4.1：标签左对齐到列。V4 只留 1px，读起来像"贴在框上"
    label.setBounds (8, 0, juce::jmax (10, box.getWidth() - box.getHeight() - 11), box.getHeight());
    label.setFont (getComboBoxFont (box));
}

void SkinnedLookAndFeel::drawPopupMenuBackground (juce::Graphics& g, int width, int height)
{
    // V4 在这层叠了一条斜向"扫描线"（用配色方案的高亮色），颜色不可控，与规范
    // "不要霓虹、不要大面积高光"冲突；这里改成一块安静的面板
    drawHudPanel (g, juce::Rectangle<int> (0, 0, width, height).toFloat(), 0);
}

void SkinnedLookAndFeel::fillTextEditorBackground (juce::Graphics& g, int width, int height,
                                                   juce::TextEditor& editor)
{
    if (drawSlot (g, skinIdOf (editor), "background",
                  juce::Rectangle<int> (0, 0, width, height).toFloat(), true))
        return;

    // 输入区是"凹进去的槽"（state 3），不是凸起面板
    drawHudPanel (g, juce::Rectangle<int> (0, 0, width, height).toFloat(), 3);
}

void SkinnedLookAndFeel::drawTextEditorOutline (juce::Graphics& g, int width, int height,
                                                juce::TextEditor& editor)
{
    const auto area = juce::Rectangle<int> (0, 0, width, height).toFloat();

    if (drawSlot (g, skinIdOf (editor), "outline", area, true))
        return;

    // 只重描外框：聚焦时换警戒红（规范 §2.2-3「红色只做一根细线」）。
    // 面与双层框由 fillTextEditorBackground 画，这里不重复铺底。
    const bool focused = editor.hasKeyboardFocus (true) && ! editor.isReadOnly();

    g.setColour (editor.findColour (focused ? juce::TextEditor::focusedOutlineColourId
                                            : juce::TextEditor::outlineColourId));
    g.strokePath (hudCutPath (area.reduced (0.5f), hudCutFor (area)), juce::PathStrokeType (kLineL1));
}

void SkinnedLookAndFeel::drawLabel (juce::Graphics& g, juce::Label& label)
{
    // 旋钮下方的数值框：允许用 defaults.valueBox 换底/换边框，文字仍走常规绘制
    if (dynamic_cast<juce::Slider*> (label.getParentComponent()) != nullptr)
    {
        const auto area = label.getLocalBounds().toFloat();

        if (const auto node = skin().defaultForType ("valueBox", "colour", juce::var()); ! node.isVoid())
        {
            g.setColour (skin().colourFor (node, juce::Colours::transparentBlack));
            g.fillRect (area);
        }

        drawImage (g, skin().assetForType ("valueBox", "background"), area, {}, true);
        LookAndFeel_V4::drawLabel (g, label);
        drawImage (g, skin().assetForType ("valueBox", "outline"), area, {}, true);
        return;
    }

    // 下拉框里显示"当前选中项"的那个 Label：内容是数据（Plate / 预设名），
    // 大写会与弹出菜单里的项对不上，所以照常绘制
    if (dynamic_cast<juce::ComboBox*> (label.getParentComponent()) != nullptr)
    {
        LookAndFeel_V4::drawLabel (g, label);
        return;
    }

    // 普通标签：按规范排版（全大写 + 字距见 getLabelFont，颜色层级走 Label::textColourId）
    g.fillAll (label.findColour (juce::Label::backgroundColourId));

    if (! label.isBeingEdited())
    {
        const auto alpha = label.isEnabled() ? 1.0f : 0.5f;
        const auto font  = getLabelFont (label);

        g.setColour (label.findColour (juce::Label::textColourId).withMultipliedAlpha (alpha));
        g.setFont (font);

        const auto textArea = getLabelBorderSize (label).subtractedFrom (label.getLocalBounds());

        g.drawFittedText (label.getText().toUpperCase(), textArea, label.getJustificationType(),
                          juce::jmax (1, static_cast<int> (static_cast<float> (textArea.getHeight())
                                                           / font.getHeight())),
                          label.getMinimumHorizontalScale());
    }

    g.setColour (label.findColour (juce::Label::outlineColourId));
    g.drawRect (label.getLocalBounds());
}

juce::Font SkinnedLookAndFeel::getLabelFont (juce::Label& label)
{
    const auto fallback = LookAndFeel_V4::getLabelFont (label);

    // 数值框的字体单独放在 defaults.valueBox.font；没写就按规范的"数值 12–14px、等宽"
    if (dynamic_cast<juce::Slider*> (label.getParentComponent()) != nullptr)
    {
        const auto node = skin().defaultForType ("valueBox", fontNodeKey(), juce::var());

        const auto font = node.isVoid()
                            ? juce::Font (juce::FontOptions (juce::Font::getDefaultMonospacedFontName(),
                                                             12.5f, juce::Font::plain))
                            : skin().fontFromNode (node, fallback);

        return font.withExtraKerningFactor (0.06f);
    }

    const auto node = skin().fontNodeFor (skinIdOf (label));
    const auto font = node.isVoid() ? fallback : skin().fontFromNode (node, fallback);

    // 规范 §4.2：标签字距 +1~2px
    return font.withExtraKerningFactor (kTracking);
}

juce::Font SkinnedLookAndFeel::getTextButtonFont (juce::TextButton& button, int buttonHeight)
{
    const auto fallback = LookAndFeel_V4::getTextButtonFont (button, buttonHeight);
    const auto node     = skin().fontNodeFor (skinIdOf (button));

    // 规范 §4.2：11–13px。按钮多在 26px 高，取一半刚好落在这一档（V4 默认给到 15px，偏大）
    const auto font = node.isVoid()
                        ? juce::Font (juce::FontOptions (static_cast<float> (juce::jlimit (11, 13,
                                                                                           buttonHeight / 2))))
                        : skin().fontFromNode (node, fallback);

    return font.withExtraKerningFactor (kTracking);
}
juce::Font SkinnedLookAndFeel::getComboBoxFont (juce::ComboBox& box)
{
    const auto fallback = LookAndFeel_V4::getComboBoxFont (box);
    const auto node     = skin().fontNodeFor (skinIdOf (box));

    const auto font = node.isVoid()
                        ? juce::Font (juce::FontOptions (static_cast<float> (juce::jlimit (11, 13,
                                                                                           box.getHeight() / 2))))
                        : skin().fontFromNode (node, fallback);

    return font.withExtraKerningFactor (0.08f);
}

juce::Font SkinnedLookAndFeel::getPopupMenuFont()
{
    // V2 默认给 17px，配规范的字号偏大：12px + 字距，与下拉框同一档
    return juce::Font (juce::FontOptions (12.0f)).withExtraKerningFactor (0.10f);
}

// ===================================================================
// SkinnedSlider（沿轨迹拖动取值）
// ===================================================================

bool SkinnedSlider::findDragTravel (skin::Travel& travel) const
{
    if (! isEnabled() || ! isTravelDragStyle (getSliderStyle()))
        return false;

    const auto id = skinIdOf (*this);

    if (id.isEmpty())
        return false;

    const auto& s = skin::SkinBundle::getInstance().getCurrent();

    // 轨道有曲线时，它才是"鼠标该走的路"；否则退到推子头自己的轨迹
    travel = s.travelForPart (id, "track");

    if (travel.enabled)
        return true;

    travel = s.travelForPart (id, "thumb");
    return travel.enabled;
}

bool SkinnedSlider::setValueFromTravel (juce::Point<float> position)
{
    skin::Travel travel;

    if (! findDragTravel (travel))
        return false;

    const auto area = getLocalBounds().toFloat();

    if (area.isEmpty())
        return false;

    // 曲线没有解析解，先粗采样找最近点，再在相邻区间细化
    constexpr int coarseSteps = 128;

    float bestT    = 0.0f;
    float bestDist = std::numeric_limits<float>::max();

    for (int i = 0; i <= coarseSteps; ++i)
    {
        const float t = static_cast<float> (i) / static_cast<float> (coarseSteps);

        if (const float d = travel.pointIn (area, t).getDistanceSquaredFrom (position); d < bestDist)
        {
            bestDist = d;
            bestT    = t;
        }
    }

    const float step = 1.0f / static_cast<float> (coarseSteps);
    const float lo   = juce::jmax (0.0f, bestT - step);
    const float hi   = juce::jmin (1.0f, bestT + step);

    constexpr int refineSteps = 24;

    for (int i = 0; i <= refineSteps; ++i)
    {
        const float t = lo + (hi - lo) * static_cast<float> (i) / static_cast<float> (refineSteps);

        if (const float d = travel.pointIn (area, t).getDistanceSquaredFrom (position); d < bestDist)
        {
            bestDist = d;
            bestT    = t;
        }
    }

    const auto value = snapValue (proportionOfLengthToValue (juce::jlimit (0.0f, 1.0f, bestT)),
                                  juce::Slider::absoluteDrag);

    setValue (value, juce::sendNotificationSync);
    return true;
}

void SkinnedSlider::mouseDown (const juce::MouseEvent& e)
{
    juce::Slider::mouseDown (e);

    // 右键菜单、禁用、无轨迹时完全保持 Slider 原行为
    if (e.mods.isPopupMenu() || ! isEnabled() || ! isTravelDragStyle (getSliderStyle()))
        return;

    setValueFromTravel (e.position);
}

void SkinnedSlider::mouseDrag (const juce::MouseEvent& e)
{
    if (! e.mods.isPopupMenu() && setValueFromTravel (e.position))
        return;

    juce::Slider::mouseDrag (e);
}
