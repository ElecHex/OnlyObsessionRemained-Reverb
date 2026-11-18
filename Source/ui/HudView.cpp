#include "HudView.h"

#include "HudSceneShader.h"
#include "params/ParameterIDs.h"
#include "params/ParameterSchema.h"

#include <cmath>

namespace
{
    const juce::Colour kBackground (0xff0b0b0d);
    const juce::Colour kText       (0xfff5f7fb);
    const juce::Colour kDim        (0xff9aa0ab);
    const juce::Colour kMicro      (0xff5a606b);
    const juce::Colour kGem        (0xff1e5bff);   // 只有 hover 小标签用蓝
    const juce::Colour kAlert      (0xffff3b4e);

    constexpr float kPi = 3.14159265358979323846f;

    /** hud-layout.json 的版本。旧文件没有该字段时按 v1 读取（向后兼容）。 */
    constexpr int kLayoutVersion = 2;

    const char* const kKnobShortNames[] = { "MIX", "SHMR" };
    const char* const kFaderShortNames[] = { "DECAY", "PRE", "MOD", "LOW", "HIGH" };
    const char* const kBlockNames[] = { "FADERS", "MIX", "SHMR", "DATA A", "DATA B" };

    /** 刀带半宽：根部 / 72% 处，末端收成尖。 */
    constexpr float kBladeHalfRoot = 6.0f;
    constexpr float kBladeHalfMid  = 4.5f;

    juce::Font hudFont (float height, float tracking = 0.0f)
    {
        auto font = juce::Font (juce::FontOptions ("Bahnschrift", height, juce::Font::plain));

        if (tracking > 0.0f)
            font = font.withExtraKerningFactor (tracking);

        return font;
    }

    float textWidth (const juce::Font& font, const juce::String& text)
    {
        return juce::GlyphArrangement::getStringWidth (font, text);
    }

    void drawTextAtBaseline (juce::Graphics& g, const juce::String& text,
                             float x, float baseline, const juce::Font& font, juce::Colour colour)
    {
        g.setFont (font);
        g.setColour (colour);
        g.drawText (text,
                    juce::Rectangle<float> (x, baseline - font.getAscent() - 0.5f,
                                            textWidth (font, text) + 3.0f, font.getHeight() + 3.0f),
                    juce::Justification::topLeft, false);
    }

    void drawCentredText (juce::Graphics& g, const juce::String& text,
                          juce::Rectangle<float> area, const juce::Font& font, juce::Colour colour)
    {
        g.setFont (font);
        g.setColour (colour);
        g.drawText (text, area, juce::Justification::centred, false);
    }

    juce::Point<float> pointOnCircle (juce::Point<float> centre, float radius, float degrees)
    {
        const auto radians = degrees * kPi / 180.0f;
        return { centre.x + std::cos (radians) * radius,
                 centre.y + std::sin (radians) * radius };
    }

    /** 从 fromDeg 沿顺时针（屏幕坐标）采样到 toDeg，端点保持精确。 */
    juce::Path makeArcPath (juce::Point<float> centre, float radius, float fromDeg, float toDeg)
    {
        juce::Path path;

        const float delta = toDeg - fromDeg;
        const int steps = juce::jmax (2, static_cast<int> (std::abs (delta) / 2.0f) + 1);

        for (int i = 0; i <= steps; ++i)
        {
            const auto p = pointOnCircle (centre, radius, fromDeg + delta * static_cast<float> (i)
                                                                   / static_cast<float> (steps));

            if (i == 0)
                path.startNewSubPath (p.x, p.y);
            else
                path.lineTo (p.x, p.y);
        }

        return path;
    }

    juce::String formatNumber (float value, int decimals)
    {
        if (decimals <= 0)
            return juce::String (juce::roundToInt (value));

        return juce::String (value, decimals);
    }

    juce::String knobValueText (float denormValue)
    {
        return formatNumber (denormValue, 1);
    }

    juce::String faderValueText (float denormValue, int decimals)
    {
        return formatNumber (denormValue, decimals);
    }

    juce::Rectangle<float> rect (float x, float y, float w, float h)
    {
        return { x, y, w, h };
    }

    float springStep (float current, float target, double dt)
    {
        // 临界阻尼的指数逼近：零过冲，约 90ms 收敛（与 ui-motion-guide §5.5 对齐）
        constexpr float tau = 0.030f;
        const auto k = 1.0f - std::exp (-static_cast<float> (dt) / tau);
        auto next = current + (target - current) * k;

        if (std::abs (target - next) < 0.0004f)
            next = target;

        return next;
    }

    /**
        一片实心刀片：尾部钉在锚点，宽肩在 26%，中段一个凹口，其余收成长尖，
        末段轻微折向。amount 0→1 同时驱动"生长"和"绕尾部旋转"。
    */
    void drawHoverBlade (juce::Graphics& g, juce::Point<float> anchor, float angleRad,
                         float length, float halfWidth, float amount, juce::Colour colour)
    {
        if (amount <= 0.0f || length <= 0.0f)
            return;

        // 旋转进入：从 −40° 拧回最终角度
        const auto rotated = angleRad - (1.0f - amount) * 0.70f;
        const auto dir = juce::Point<float> (std::cos (rotated), std::sin (rotated));
        const auto perp = juce::Point<float> (-dir.y, dir.x);

        const auto len = length * amount;
        const auto w = halfWidth;
        const auto bend = perp * (len * 0.09f);

        const auto tail = anchor;
        const auto shoulder = anchor + dir * (len * 0.22f) - perp * w;
        const auto notch = anchor + dir * (len * 0.33f) - perp * (w * 0.16f);
        const auto tip = anchor + dir * len + bend;
        const auto back = anchor + dir * (len * 0.56f) + perp * (w * 0.22f) + bend * 0.6f;
        const auto back2 = anchor + dir * (len * 0.06f) + perp * (w * 0.16f);

        juce::Path path;
        path.startNewSubPath (tail.x, tail.y);
        path.lineTo (shoulder.x, shoulder.y);
        path.lineTo (notch.x, notch.y);
        path.lineTo (tip.x, tip.y);
        path.lineTo (back.x, back.y);
        path.lineTo (back2.x, back2.y);
        path.closeSubPath();

        g.setColour (colour);
        g.fillPath (path);
    }

    /**
        悬停展开：三片不同大小的实心刀片。
        刀片以 0/1/2 帧错峰、各自绕尾部旋转着展开；退出时 amount 反向，
        同一套几何自动收回。
    */
    void drawHoverBlades (juce::Graphics& g, juce::Point<float> anchor,
                          float baseAngle, float amount, float alpha, float scale)
    {
        struct Blade { float offset; float length; float width; int delay; };

        static const Blade blades[] =
        {
            { -0.24f, 176.0f, 11.0f, 0 },
            {  0.06f, 140.0f,  9.0f, 1 },
            {  0.36f, 106.0f,  6.5f, 2 },
        };

        const auto fanPerp = juce::Point<float> (-std::sin (baseAngle), std::cos (baseAngle));

        for (int i = 0; i < 3; ++i)
        {
            const auto& blade = blades[i];
            const auto p = juce::jlimit (0.0f, 1.0f,
                                         amount * 2.0f - 0.33f * static_cast<float> (blade.delay));

            // 尾部沿扇面错开，不再全部汇成一个点
            const auto tail = anchor + fanPerp * ((static_cast<float> (i) - 1.0f) * 9.0f * scale);

            drawHoverBlade (g, tail, baseAngle + blade.offset, blade.length * scale,
                            blade.width * scale, p, kText.withAlpha (0.92f * alpha));
        }
    }
}

// =======================================================================
// 板块变换
// =======================================================================

juce::AffineTransform HudView::BlockTransform::matrix() const noexcept
{
    return juce::AffineTransform::translation (-pivot.x, -pivot.y)
        .scaled (scale, scale)
        .rotated (rotation * kPi / 180.0f)
        .translated (pivot.x + offset.x, pivot.y + offset.y);
}

juce::Point<float> HudView::BlockTransform::toLocal (juce::Point<float> p) const noexcept
{
    float x = p.x;
    float y = p.y;
    matrix().inverted().transformPoint (x, y);
    return { x, y };
}

const HudView::BlockTransform& HudView::transformFor (int block) const
{
    return blockTransform[static_cast<size_t> (juce::jlimit (0, static_cast<int> (blockCount) - 1, block))];
}

juce::AffineTransform HudView::matrixFor (int block) const
{
    return transformFor (block).matrix();
}

juce::Rectangle<float> HudView::blockDesignBounds (int block) const
{
    const auto local = blockBounds[static_cast<size_t> (juce::jlimit (0, static_cast<int> (blockCount) - 1, block))];
    return local.transformedBy (matrixFor (block));
}

juce::File HudView::layoutFile() const
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("OnlyObsessionRemained")
        .getChildFile ("hud-layout.json");
}

void HudView::resetLayout()
{
    blockBounds[blockFaders]  = rect (236.0f, 220.0f, 540.0f, 298.0f);
    blockBounds[blockMix]     = rect (104.0f, 102.0f, 220.0f, 220.0f);
    blockBounds[blockShimmer] = rect (248.0f, 236.0f, 104.0f, 104.0f);
    blockBounds[blockDataA]   = rect (0.0f, 0.0f, 96.0f, 56.0f);
    blockBounds[blockDataB]   = rect (0.0f, 0.0f, 96.0f, 56.0f);

    // 编译期内置版面。数值取自设计器里确定下来的那一版布局（2026-11-18），
    // 直接写死在这里，于是新设备、删掉 hud-layout.json 之后都回到这一版，
    // 而不是更早的 6:3:1 初始排布。
    // 用户本机已有的 hud-layout.json 仍然优先生效（loadLayout 会覆盖这组值）。
    blockTransform[blockFaders] = BlockTransform { { 560.0f, 232.0f },
                                                   { -122.399993896484375f, -23.959976196289062f },
                                                   1.080000042915344f, 0.0f };
    blockTransform[blockMix]    = BlockTransform { { 214.0f, 212.0f },
                                                   { 115.999969482421875f, -42.4000244140625f },
                                                   0.976205945014954f, 0.0f };
    blockTransform[blockShimmer]= BlockTransform { { 300.0f, 288.0f },
                                                   { -112.400039672851562f, -53.60003662109375f },
                                                   1.013171553611755f, 0.0f };
    blockTransform[blockDataA]  = BlockTransform { { 48.0f, 28.0f },
                                                   { 842.39996337890625f, 64.79998779296875f },
                                                   1.0f, 0.0f };
    blockTransform[blockDataB]  = BlockTransform { { 48.0f, 28.0f },
                                                   { 24.0f, 412.0f },
                                                   1.0f, 0.0f };
}

void HudView::loadLayout()
{
    readLayout (layoutFile());
}

void HudView::saveLayout() const
{
    writeLayout (layoutFile());
}

void HudView::readLayout (const juce::File& file)
{
    // 注意：必须让解析结果活到读完属性之后，否则 getDynamicObject() 返回悬空指针。
    const auto parsed = juce::JSON::parse (file.loadFileAsString());
    auto* obj = parsed.getDynamicObject();

    if (obj == nullptr)
        return;

    // 版本保护：旧文件没有 version 字段，按 v1 继续读（向后兼容）。
    // 更高版本文件里可能有本程序不认识的版面字段，直接忽略，避免用半份数据
    // 覆盖当前布局。
    if (static_cast<int> (obj->getProperty ("version")) > kLayoutVersion)
        return;

    for (int block = 0; block < blockCount; ++block)
    {
        const auto stored = obj->getProperty ("block" + juce::String (block));

        if (! stored.isArray() || stored.size() != 6)
            continue;

        auto& target = blockTransform[static_cast<size_t> (block)];
        target.pivot    = { static_cast<float> (stored[0]), static_cast<float> (stored[1]) };
        target.offset   = { static_cast<float> (stored[2]), static_cast<float> (stored[3]) };
        target.scale    = juce::jlimit (0.3f, 3.0f, static_cast<float> (stored[4]));
        target.rotation = static_cast<float> (stored[5]);
    }
}

void HudView::writeLayout (const juce::File& file) const
{
    juce::DynamicObject::Ptr obj (new juce::DynamicObject());

    obj->setProperty ("version", kLayoutVersion);

    for (int block = 0; block < blockCount; ++block)
    {
        const auto& t = blockTransform[static_cast<size_t> (block)];
        obj->setProperty ("block" + juce::String (block),
                          juce::var (juce::Array<juce::var> { t.pivot.x, t.pivot.y,
                                                              t.offset.x, t.offset.y,
                                                              t.scale, t.rotation }));
    }

    file.getParentDirectory().createDirectory();
    file.replaceWithText (juce::JSON::toString (juce::var (obj.get()), false));
}

juce::File HudView::layoutPresetDirectory() const
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
        .getChildFile ("OnlyObsessionRemained")
        .getChildFile ("layout-presets");
}

juce::StringArray HudView::layoutPresetNames() const
{
    juce::StringArray names;
    const auto dir = layoutPresetDirectory();

    if (! dir.isDirectory())
        return names;

    for (const auto& entry : juce::RangedDirectoryIterator (dir, false, "*.json"))
        names.add (entry.getFile().getFileNameWithoutExtension());

    names.sort (true);
    return names;
}

juce::String HudView::nextLayoutPresetName() const
{
    const auto existing = layoutPresetNames();

    for (int i = 1; i < 100; ++i)
    {
        const auto candidate = "layout-" + juce::String (i).paddedLeft ('0', 2);

        if (! existing.contains (candidate))
            return candidate;
    }

    return "layout";
}

int HudView::layoutFooterButtonAt (juce::Point<float> p) const
{
    for (auto button : { Button::layoutSave, Button::layoutLoad, Button::layoutReset })
        if (buttonRect (button).contains (p))
            return static_cast<int> (button);

    return -1;
}

void HudView::beginLayoutSave()
{
    if (nameEditor.isVisible())
    {
        commitSave();
        return;
    }

    layoutSaveMode = true;
    saveMode = false;

    const auto designRect = rect (330.0f, 236.0f, 300.0f, 26.0f);
    const auto scaleX = static_cast<float> (getWidth()) / designWidth;
    const auto scaleY = static_cast<float> (getHeight()) / designHeight;

    nameEditor.setBounds (juce::Rectangle<int> (juce::roundToInt (designRect.getX() * scaleX),
                                                juce::roundToInt (designRect.getY() * scaleY),
                                                juce::roundToInt (designRect.getWidth() * scaleX),
                                                juce::roundToInt (designRect.getHeight() * scaleY)));
    nameEditor.setText (nextLayoutPresetName(), false);
    nameEditor.selectAll();
    nameEditor.setVisible (true);
    nameEditor.grabKeyboardFocus();

    setTransient ("SYS :: SAVE LAYOUT", 120);
    repaint();
}

void HudView::commitLayoutSave()
{
    auto name = juce::File::createLegalFileName (nameEditor.getText().trim());
    nameEditor.setVisible (false);
    layoutSaveMode = false;

    if (name.isEmpty())
    {
        setEvent ("layout.save :: name empty  >  err");
        setTransient ("SYS :: NAME?", 60);
        repaint();
        return;
    }

    writeLayout (layoutPresetDirectory().getChildFile (name + ".json"));
    setEvent ("layout.save :: " + name + "  >  ok");
    setTransient ("SYS :: LAYOUT SAVED", 60);

    grabKeyboardFocus();
    repaint();
}

void HudView::showLayoutPresetMenu()
{
    const auto names = layoutPresetNames();

    juce::PopupMenu menu;
    menu.addSectionHeader ("LAYOUT PRESETS");

    if (names.isEmpty())
        menu.addItem (1, "(none saved)", false, false);
    else
        for (int i = 0; i < names.size(); ++i)
            menu.addItem (i + 1, names[i]);

    menu.addSeparator();
    menu.addItem (1000, "DEFAULT (reset)");

    auto area = localAreaToGlobal (buttonRect (Button::layoutLoad).toNearestInt());
    juce::Component::SafePointer<HudView> safeThis (this);

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (area)
                            .withMinimumWidth (200),
                        [safeThis, names] (int result)
                        {
                            auto* view = safeThis.getComponent();

                            if (view == nullptr || result <= 0)
                                return;

                            if (result == 1000)
                            {
                                view->resetLayout();
                                view->saveLayout();
                                view->setEvent ("layout.reset :: default  >  ok");
                                view->setTransient ("SYS :: RESET", 60);
                            }
                            else if (result <= names.size())
                            {
                                view->applyLayoutPreset (names[result - 1]);
                            }

                            view->lastFrame = -1;
                            view->repaint();
                        });
}

void HudView::applyLayoutPreset (const juce::String& name)
{
    readLayout (layoutPresetDirectory().getChildFile (name + ".json"));
    saveLayout();                     // 同时成为下次启动的布局

    setEvent ("layout.load :: " + name + "  >  ok");
    setTransient ("SYS :: LAYOUT", 60);
    lastFrame = -1;
    repaint();
}

// =======================================================================
// 构造 / 销毁
// =======================================================================

HudView::HudView (OnlyObsessionRemainedProcessor& p)
    : processor (p)
{
    setOpaque (true);
    setWantsKeyboardFocus (true);
    setMouseCursor (juce::MouseCursor::NormalCursor);

    resetLayout();
    loadLayout();

    // 保存预设用的内联输入：只借 JUCE 的文本输入能力，外观完全交给 HUD
    nameEditor.setMultiLine (false);
    nameEditor.setReturnKeyStartsNewLine (false);
    nameEditor.setJustification (juce::Justification::centredLeft);
    nameEditor.setFont (hudFont (13.0f, 0.03f));
    nameEditor.setColour (juce::TextEditor::backgroundColourId, juce::Colours::transparentBlack);
    nameEditor.setColour (juce::TextEditor::outlineColourId, juce::Colours::transparentBlack);
    nameEditor.setColour (juce::TextEditor::focusedOutlineColourId, juce::Colours::transparentBlack);
    nameEditor.setColour (juce::TextEditor::shadowColourId, juce::Colours::transparentBlack);
    nameEditor.setColour (juce::TextEditor::textColourId, kText);
    nameEditor.setColour (juce::TextEditor::highlightColourId, kText.withAlpha (0.25f));
    nameEditor.setColour (juce::TextEditor::highlightedTextColourId, kBackground);
    nameEditor.setColour (juce::CaretComponent::caretColourId, kText);
    nameEditor.setBorder (juce::BorderSize<int> (1, 2, 1, 2));
    nameEditor.setIndents (0, 0);
    nameEditor.onReturnKey = [this] { if (layoutSaveMode) commitLayoutSave(); else commitSave(); };
    nameEditor.onEscapeKey = [this] { cancelSave(); };
    nameEditor.onFocusLost = [this] { cancelSave(); };
    addChildComponent (nameEditor);

    buildControls();
    buildAttachments();

    // 背景着色器：源码内嵌，不走皮肤清单。先注入再挂 GL 上下文，
    // 探测在挂载后异步进行（见 ShaderBackground / ADR-0002）；失败就保持纯黑底。
    backdrop.loadDirectSource (hudSceneShaderSource, 30);
    backdrop.attachTo (*this);

    processor.presetManager.addChangeListener (this);

    // 浮现入场从"第一次真正上屏绘制"开始计时（见 paint），避免窗口还没显示就把动画跑完；
    // 关闭浮现时沿用原来的行为：构造即开始。
    startTime = kPopInEnabled ? 0.0 : juce::Time::getMillisecondCounterHiRes() * 0.001;
    lastTickTime = startTime;
    lastEventTime = juce::Time::getCurrentTime().formatted ("%H:%M:%S");
    lastEvent = "sys.boot :: hud-30  >  ok";

    startTimerHz (60);
}

HudView::~HudView()
{
    stopTimer();
    processor.presetManager.removeChangeListener (this);

    // 销毁期间 TextEditor 可能收到 focusLost：先断掉回调，避免回调进半销毁的 HudView。
    nameEditor.onReturnKey = nullptr;
    nameEditor.onEscapeKey = nullptr;
    nameEditor.onFocusLost = nullptr;
}

void HudView::buildControls()
{
    knobs[0] = KnobState {};
    knobs[0].paramId = params::mix;
    knobs[0].centre = { 214.0f, 212.0f };
    knobs[0].radius = 104.0f;
    knobs[0].needleInner = 23.0f;
    knobs[0].needleOuter = 98.0f;
    knobs[0].valueCx = 241.04f;
    knobs[0].valueBaseline = 180.80f;
    knobs[0].bracketSize = 18.0f;
    knobs[0].valueSize = 27.0f;
    knobs[0].unitSize = 8.0f;
    knobs[0].label = params::hudNameFor (params::mix);
    knobs[0].labelBox = rect (128.56f, 222.40f, 44.0f, 15.0f);
    knobs[0].labelSize = 8.0f;
    knobs[0].appearFrame = 6;
    knobs[0].scanPhase = 0;

    knobs[1] = KnobState {};
    knobs[1].paramId = params::shimmer;
    knobs[1].centre = { 300.0f, 288.0f };
    knobs[1].radius = 47.0f;
    knobs[1].needleInner = 10.34f;
    knobs[1].needleOuter = 41.0f;
    knobs[1].valueCx = 312.22f;
    knobs[1].valueBaseline = 273.90f;
    knobs[1].bracketSize = 13.0f;
    knobs[1].valueSize = 16.0f;
    knobs[1].unitSize = 8.0f;
    knobs[1].label = params::hudNameFor (params::shimmer);
    knobs[1].labelX = 300.0f;
    knobs[1].labelBaseline = 319.02f;
    knobs[1].labelSize = 6.5f;
    knobs[1].appearFrame = 8;
    knobs[1].scanPhase = 60;

    faders[0] = FaderState {};
    faders[0].paramId = params::decay;
    faders[0].label = params::hudNameFor (params::decay);
    faders[0].root = { 509.85f, 252.11f };
    faders[0].end = { 285.65f, 353.38f };
    faders[0].handleRadius = 4.0f;
    faders[0].decimals = 1;
    faders[0].appearFrame = 10;

    faders[1] = FaderState {};
    faders[1].paramId = params::preDelay;
    faders[1].label = params::hudNameFor (params::preDelay);
    faders[1].root = { 523.05f, 271.42f };
    faders[1].end = { 395.46f, 420.22f };
    faders[1].handleRadius = 4.0f;
    faders[1].decimals = 0;
    faders[1].appearFrame = 12;

    faders[2] = FaderState {};
    faders[2].paramId = params::mod;
    faders[2].label = params::hudNameFor (params::mod);
    faders[2].root = { 543.17f, 283.35f };
    faders[2].end = { 500.89f, 435.59f };
    faders[2].handleRadius = 4.0f;
    faders[2].decimals = 1;
    faders[2].appearFrame = 14;

    faders[3] = FaderState {};
    faders[3].paramId = params::lowDamp;
    faders[3].label = params::hudNameFor (params::lowDamp);
    faders[3].root = { 566.44f, 285.65f };
    faders[3].end = { 596.85f, 469.15f };
    faders[3].outlinedHandle = true;
    faders[3].handleRadius = 6.0f;
    faders[3].decimals = 1;
    faders[3].appearFrame = 16;

    faders[4] = FaderState {};
    faders[4].paramId = params::highDamp;
    faders[4].label = params::hudNameFor (params::highDamp);
    faders[4].root = { 588.52f, 277.90f };
    faders[4].end = { 710.40f, 456.23f };
    faders[4].outlinedHandle = true;
    faders[4].handleRadius = 6.0f;
    faders[4].decimals = 1;
    faders[4].appearFrame = 18;

    // 标签与读数框统一放在刀带末端的外侧延长线上，避免刀带穿过文字。
    for (auto& fader : faders)
    {
        const auto direction = fader.end - fader.root;
        const auto length = direction.getDistanceFromOrigin();

        if (length <= 1.0f)
            continue;

        const auto unit = direction / length;
        const auto anchor = fader.end + unit * 24.0f;

        fader.labelX = anchor.x;
        fader.labelBaseline = anchor.y;
        fader.valueBox = rect (anchor.x, anchor.y + 6.0f, 46.0f, 17.0f);
    }
}

void HudView::buildAttachments()
{
    for (int i = 0; i < static_cast<int> (knobs.size()); ++i)
    {
        auto& knob = knobs[static_cast<size_t> (i)];
        knob.param = processor.apvts.getParameter (knob.paramId);

        if (knob.param == nullptr)
            continue;

        knob.attachment = std::make_unique<juce::ParameterAttachment> (
            *knob.param,
            [this, &knob] (float value)
            {
                knob.targetNorm = knob.param->convertTo0to1 (value);
                knob.denormValue = value;

                if (! initialised)
                    knob.displayNorm = knob.targetNorm;

                repaint();
            });

        knob.attachment->sendInitialUpdate();
    }

    for (int i = 0; i < static_cast<int> (faders.size()); ++i)
    {
        auto& fader = faders[static_cast<size_t> (i)];
        fader.param = processor.apvts.getParameter (fader.paramId);

        if (fader.param == nullptr)
            continue;

        fader.attachment = std::make_unique<juce::ParameterAttachment> (
            *fader.param,
            [this, &fader] (float value)
            {
                fader.targetNorm = fader.param->convertTo0to1 (value);
                fader.denormValue = value;

                if (! initialised)
                    fader.displayNorm = fader.targetNorm;

                repaint();
            });

        fader.attachment->sendInitialUpdate();
    }

    typeParam = processor.apvts.getParameter (params::type);

    if (typeParam != nullptr)
    {
        typeAttachment = std::make_unique<juce::ParameterAttachment> (
            *typeParam,
            [this] (float value)
            {
                typeIndex = juce::jlimit (0, 2, juce::roundToInt (value));
                repaint();
            });

        typeAttachment->sendInitialUpdate();
    }

    initialised = true;
    refreshFromParams (true);
}

void HudView::refreshFromParams (bool snap)
{
    for (auto& knob : knobs)
    {
        if (knob.param == nullptr)
            continue;

        const auto normalised = juce::jlimit (0.0f, 1.0f, knob.param->getValue());
        knob.targetNorm = normalised;
        knob.denormValue = knob.param->convertFrom0to1 (normalised);

        if (snap)
            knob.displayNorm = normalised;
    }

    for (auto& fader : faders)
    {
        if (fader.param == nullptr)
            continue;

        const auto normalised = juce::jlimit (0.0f, 1.0f, fader.param->getValue());
        fader.targetNorm = normalised;
        fader.denormValue = fader.param->convertFrom0to1 (normalised);

        if (snap)
        {
            fader.displayNorm = normalised;
            fader.glowNorm = normalised;
        }
    }

    if (typeParam != nullptr)
        typeIndex = juce::jlimit (0, 2, juce::roundToInt (typeParam->convertFrom0to1 (typeParam->getValue())));
}

// =======================================================================
// 时钟 / 帧
// =======================================================================

int HudView::frameNow() const
{
    if (startTime <= 0.0)
        return 0;

    const auto elapsed = juce::Time::getMillisecondCounterHiRes() * 0.001 - startTime;
    return juce::jmax (0, static_cast<int> (elapsed * 30.0));
}

float HudView::alphaFor (int appearFrame) const
{
    if constexpr (kPopInEnabled)
    {
        // 控件按各自的浮现起始帧依次出现（见 paint 里的 frame >= delay 判断），
        // 出现时就是满亮度，不再统一淡入。
        juce::ignoreUnused (appearFrame);
        return 1.0f;
    }

    const auto frame = frameNow();

    if (frame < appearFrame)
        return 0.0f;

    if (frame < appearFrame + 2)
        return 0.55f;   // power-on：两档硬切

    return 1.0f;
}

int HudView::introFrame() const
{
    return juce::jmax (0, frameNow() - kPopFrames);
}

float HudView::structureAlpha (int appearFrame) const
{
    const auto frame = introFrame();

    if (frame < appearFrame)
        return 0.0f;

    if (frame < appearFrame + 2)
        return 0.55f;   // power-on：两档硬切（原入场效果，时间轴整体后移）

    return 1.0f;
}

juce::AffineTransform HudView::popMatrixAt (juce::Point<float> pivot, float delayFrames) const
{
    // 单个控件的行程只有 4 帧（约 0.13 s）：出现后立刻线性抡到位，
    // 最后 1 帧从 1.06 棘爪回弹，像钟表指针"咔"一下停住。
    constexpr float motion    = 4.0f;
    constexpr float driveEnd  = 0.75f;
    constexpr float overshoot = 1.06f;

    const auto frame = static_cast<float> (frameNow());

    if (frame >= delayFrames + motion)
        return {};

    const auto t = juce::jlimit (0.0f, 1.0f, (frame - delayFrames) / motion);

    const auto e = t < driveEnd
                       ? overshoot * (t / driveEnd)                  // 匀速度盘：一出现就在走
                       : overshoot - (overshoot - 1.0f) * ((t - driveEnd) / (1.0f - driveEnd));

    // 从"深处"弹出来：一开始更小、更靠近画面中心
    const auto scale = 0.50f + 0.50f * e;
    const juce::Point<float> centre (designWidth * 0.5f, designHeight * 0.5f);
    const auto inward = (centre - pivot) * (0.20f * (1.0f - e));
    const auto angle  = (pivot.x < centre.x ? 1.0f : -1.0f) * 0.055f * (1.0f - e);

    const auto c  = std::cos (angle) * scale;
    const auto s  = std::sin (angle) * scale;
    const auto ox = pivot.x + inward.x;
    const auto oy = pivot.y + inward.y;

    return juce::AffineTransform (c,  -s, ox - c * pivot.x + s * pivot.y,
                                  s,   c, oy - s * pivot.x - c * pivot.y);
}

bool HudView::needsRepaint (int frame) const
{
    if (layoutMode)
        return true;

    if (frame < (kPopInEnabled ? kPopFrames : 0) + 40)
        return true;

    if (frame - buttonFlashFrame >= 0 && frame - buttonFlashFrame <= 1)
        return true;

    // hover 二级展开（刀片扇 4 帧 + 装饰线）：进入后的若干帧要持续重绘
    if (hoveredButton >= 0 && frame - buttonHoverFrame <= 6)
        return true;

    if (transientUntilFrame == frame || transientUntilFrame == frame - 1)
        return true;

    if (deleteArmedUntil > juce::Time::getMillisecondCounterHiRes() * 0.001)
        return true;

    for (const auto& knob : knobs)
    {
        if (knob.dragging)
            return true;

        if (knob.hovered && frame - knob.hoverFrame <= 6)
            return true;

        const auto period = (knob.hovered ? 40 : 120);
        auto c = (frame - 34 - knob.scanPhase) % period;

        if (c < 0)
            c += period;

        if (c < 3)
            return true;

        const auto rel = frame - knob.releaseFrame;

        if (rel >= 0 && rel <= 6)
            return true;
    }

    for (const auto& fader : faders)
    {
        if (fader.dragging)
            return true;

        if (fader.hovered && frame - fader.hoverFrame <= 13)
            return true;

        // 退出动效（刀片收回）也要继续重绘
        if (! fader.hovered && fader.hoverEndFrame >= fader.hoverFrame
            && frame - fader.hoverEndFrame <= 8)
            return true;

        // 跟随光条还在追手柄时继续重绘
        if (std::abs (fader.glowNorm - fader.displayNorm) > 0.0005f)
            return true;

        const auto rel = frame - fader.releaseFrame;

        if (rel >= 0 && rel <= 4)
            return true;
    }

    return false;
}

void HudView::timerCallback()
{
    if (! isShowing())
    {
        lastTickTime = 0.0;
        return;
    }

    const auto now = juce::Time::getMillisecondCounterHiRes() * 0.001;

    // 浮现入场时，时钟由第一帧绘制启动（见 paint）；在那之前不推进任何动画
    if (startTime <= 0.0)
    {
        if constexpr (kPopInEnabled)
            return;

        startTime = now;
    }

    auto dt = lastTickTime > 0.0 ? now - lastTickTime : 1.0 / 60.0;
    lastTickTime = now;
    dt = juce::jlimit (0.002, 0.050, dt);

    bool moving = false;

    for (auto& knob : knobs)
    {
        if (std::abs (knob.targetNorm - knob.displayNorm) > 0.0002f)
        {
            knob.displayNorm = springStep (knob.displayNorm, knob.targetNorm, dt);
            moving = true;
        }
        else if (knob.displayNorm != knob.targetNorm)
        {
            knob.displayNorm = knob.targetNorm;
            moving = true;
        }
    }

    for (auto& fader : faders)
    {
        if (std::abs (fader.targetNorm - fader.displayNorm) > 0.0002f)
        {
            fader.displayNorm = springStep (fader.displayNorm, fader.targetNorm, dt);
            moving = true;
        }
        else if (fader.displayNorm != fader.targetNorm)
        {
            fader.displayNorm = fader.targetNorm;
            moving = true;
        }

        // 跟随光条：用同一个弹簧、但把时间压慢，形成"懒跟随"
        if (std::abs (fader.displayNorm - fader.glowNorm) > 0.0004f)
        {
            fader.glowNorm = springStep (fader.glowNorm, fader.displayNorm, dt * 0.28f);
            moving = true;
        }
        else
        {
            fader.glowNorm = fader.displayNorm;
        }
    }

    const auto frame = frameNow();
    const bool frameChanged = frame != lastFrame;
    lastFrame = frame;

    // 背景着色器：时间连续推进；GPU 就绪后按约 30 Hz 重绘（未就绪时保持纯黑底）。
    bool backdropFrame = false;

    if (backdrop.isEnabled())
    {
        backdropTime += dt;

        const auto params = processor.getNormalizedParameters();

        ShaderBackground::FrameState state;
        state.time       = static_cast<float> (backdropTime);
        state.mix        = params.mix;
        state.decay      = params.decay;
        state.preDelayMs = params.preDelayMs;
        state.mod        = params.mod;
        state.highDamp   = params.highDamp;
        state.lowDamp    = params.lowDamp;
        state.shimmer    = params.shimmer;
        state.type       = static_cast<float> (params.type);
        state.level      = processor.getWetOutputLevel();
        state.dark       = juce::Desktop::getInstance().isDarkModeActive() ? 1.0f : 0.0f;

        backdrop.setFrameState (state);

        // 编译失败只报一次：页脚事件行是 HUD 里唯一能说明"背景为什么没了"的地方
        if (const auto error = backdrop.getLastError();
            error.isNotEmpty() && error != lastBackdropError)
        {
            lastBackdropError = error;
            setEvent ("bg.shader :: " + error.substring (0, 46));
        }

        if (backdrop.isGpuActive() && now - lastBackdropFrame >= 1.0 / 30.0)
        {
            lastBackdropFrame = now;
            backdropFrame = true;
        }
    }

    if (moving || backdropFrame || (frameChanged && needsRepaint (frame)))
        repaint();
}

void HudView::setBackdropEnabled (bool shouldBeEnabled)
{
    backdrop.setUserEnabled (shouldBeEnabled);
    repaint();
}

void HudView::visibilityChanged()
{
    if (isVisible())
    {
        // 浮现入场：等第一帧真正绘制时再启动时钟；关闭时沿用原来的逻辑
        startTime = kPopInEnabled ? 0.0 : juce::Time::getMillisecondCounterHiRes() * 0.001;
        lastTickTime = startTime;
        lastFrame = -1;
        repaint();
    }
    else
    {
        cancelSave();
    }
}

// =======================================================================
// 板块布局编辑层
// =======================================================================

void HudView::toggleLayoutEditor()
{
    layoutMode = ! layoutMode;
    layoutDragging = false;
    layoutSelected = blockFaders;
    updateHover ({});

    if (layoutMode)
        setTransient ("SYS :: LAYOUT", 120);
    else
        saveLayout();

    lastFrame = -1;
    repaint();
}

int HudView::blockAt (juce::Point<float> p) const
{
    // 小旋钮压在右侧：倒序命中，保证小件优先。
    for (int block = blockCount - 1; block >= 0; --block)
        if (blockDesignBounds (block).contains (p))
            return block;

    return -1;
}

bool HudView::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();

    if (mods.isCtrlDown() && mods.isShiftDown() && key.getKeyCode() == 'L')
    {
        toggleLayoutEditor();
        return true;
    }

    if (! layoutMode)
        return false;

    if (key.getKeyCode() == juce::KeyPress::escapeKey)
    {
        toggleLayoutEditor();
        return true;
    }

    if (key.getKeyCode() == 'R' && ! mods.isCtrlDown())
    {
        resetLayout();
        saveLayout();
        lastFrame = -1;
        repaint();
        return true;
    }

    const auto step = mods.isShiftDown() ? 10.0f : 1.0f;
    const auto keyCode = key.getKeyCode();
    auto& target = blockTransform[static_cast<size_t> (layoutSelected)];

    if (keyCode == juce::KeyPress::leftKey)
        target.offset.x -= step;
    else if (keyCode == juce::KeyPress::rightKey)
        target.offset.x += step;
    else if (keyCode == juce::KeyPress::upKey)
        target.offset.y -= step;
    else if (keyCode == juce::KeyPress::downKey)
        target.offset.y += step;
    else
        return false;

    saveLayout();
    repaint();
    return true;
}

// =======================================================================
// 命中 / 交互
// =======================================================================

juce::Rectangle<float> HudView::buttonRect (Button button) const
{
    switch (button)
    {
        case Button::prev:        return rect (24.0f, 12.0f, 26.0f, 32.0f);
        case Button::next:        return rect (262.0f, 12.0f, 26.0f, 32.0f);
        case Button::presetField: return rect (56.0f, 12.0f, 200.0f, 32.0f);
        case Button::save:        return rect (296.0f, 12.0f, 48.0f, 32.0f);
        case Button::del:         return rect (350.0f, 12.0f, 40.0f, 32.0f);
        case Button::type:        return rect (806.0f, 12.0f, 82.0f, 32.0f);
        case Button::typeUp:      return rect (892.0f, 15.0f, 20.0f, 13.0f);
        case Button::typeDown:    return rect (892.0f, 28.0f, 20.0f, 13.0f);
        case Button::layout:      return rect (870.0f, 501.0f, 66.0f, 18.0f);
        case Button::layoutSave:  return rect (676.0f, 501.0f, 46.0f, 18.0f);
        case Button::layoutLoad:  return rect (730.0f, 501.0f, 46.0f, 18.0f);
        case Button::layoutReset: return rect (784.0f, 501.0f, 52.0f, 18.0f);
        case Button::count:       break;
    }

    return {};
}

juce::Rectangle<float> HudView::presetTextRect() const
{
    return rect (100.0f, 16.0f, 146.0f, 26.0f);
}

HudView::Hit HudView::hitTest (juce::Point<float> p) const
{
    for (auto button : { Button::typeUp, Button::typeDown, Button::prev, Button::next,
                         Button::presetField, Button::save, Button::del, Button::type,
                         Button::layout })
    {
        if (buttonRect (button).contains (p))
            return { Hit::Kind::button, static_cast<int> (button) };
    }

    // 推子优先于旋钮：大旋钮的外圈会和推子轨道 / 读数框重叠，
    // 此时鼠标落在重叠区应控制推子。
    const auto local = transformFor (blockFaders).toLocal (p);
    int bestFader = -1;
    float bestDistance = 1.0e9f;

    for (int i = 0; i < static_cast<int> (faders.size()); ++i)
    {
        const auto& fader = faders[static_cast<size_t> (i)];
        const auto v = fader.end - fader.root;
        const auto lengthSquared = v.x * v.x + v.y * v.y;

        if (lengthSquared <= 1.0f)
            continue;

        if (fader.valueBox.contains (local))
            return { Hit::Kind::fader, i };

        const auto t = (local - fader.root).x * v.x + (local - fader.root).y * v.y;
        const auto clampedT = juce::jlimit (0.0f, 1.0f, t / lengthSquared);
        const auto nearest = fader.root + v * clampedT;
        const auto distance = local.getDistanceFrom (nearest);

        if (distance <= 16.0f && distance < bestDistance)
        {
            bestDistance = distance;
            bestFader = i;
        }
    }

    if (bestFader >= 0)
        return { Hit::Kind::fader, bestFader };

    // 小旋钮压在 mix 盘下方：倒序命中，小件优先
    for (int i = static_cast<int> (knobs.size()) - 1; i >= 0; --i)
    {
        const auto& knob = knobs[static_cast<size_t> (i)];
        const auto knobLocal = transformFor (blockForKnob (i)).toLocal (p);

        if (knobLocal.getDistanceFrom (knob.centre) <= knob.radius + 5.0f)
            return { Hit::Kind::knob, i };
    }

    return {};
}

void HudView::updateHover (const Hit& hit)
{
    const int newButton = hit.kind == Hit::Kind::button ? hit.index : -1;
    const int newKnob   = hit.kind == Hit::Kind::knob   ? hit.index : -1;
    const int newFader  = hit.kind == Hit::Kind::fader  ? hit.index : -1;

    if (newButton == hoveredButton && newKnob == hoveredKnob && newFader == hoveredFader)
        return;

    const auto frame = frameNow();
    const auto oldButton = hoveredButton;

    hoveredButton = newButton;
    hoveredKnob   = newKnob;
    hoveredFader  = newFader;

    if (newButton >= 0 && newButton != oldButton)
        buttonHoverFrame = frame;

    for (int i = 0; i < static_cast<int> (knobs.size()); ++i)
    {
        auto& knob = knobs[static_cast<size_t> (i)];
        const auto wasHovered = knob.hovered;

        knob.hovered = (i == hoveredKnob);

        if (knob.hovered && ! wasHovered)
            knob.hoverFrame = frame;
    }

    for (int i = 0; i < static_cast<int> (faders.size()); ++i)
    {
        auto& fader = faders[static_cast<size_t> (i)];
        const auto wasHovered = fader.hovered;

        fader.hovered = (i == hoveredFader);

        if (fader.hovered && ! wasHovered)
        {
            fader.hoverFrame = frame;
            fader.hoverEndFrame = -1000;
            fader.glowNorm = fader.displayNorm;   // 光条从手柄当前位置起步，不从旧值飞过来
        }
        else if (! fader.hovered && wasHovered)
        {
            // 退出动效：刀片从当前展开量收回
            fader.hoverEndFrame = frame;
        }
    }

    setMouseCursor (layoutMode ? (hoveredButton >= 0 ? juce::MouseCursor::PointingHandCursor
                                                     : juce::MouseCursor::DraggingHandCursor)
                               : (hoveredKnob >= 0 || hoveredFader >= 0
                                      ? juce::MouseCursor::DraggingHandCursor
                                      : (hoveredButton >= 0 ? juce::MouseCursor::PointingHandCursor
                                                            : juce::MouseCursor::NormalCursor)));

    repaint();
}

void HudView::mouseMove (const juce::MouseEvent& event)
{
    if (nameEditor.isVisible())
        return;

    const auto scaleX = designWidth / static_cast<float> (juce::jmax (1, getWidth()));
    const auto scaleY = designHeight / static_cast<float> (juce::jmax (1, getHeight()));
    const auto p = juce::Point<float> (event.position.x * scaleX, event.position.y * scaleY);

    if (layoutMode)
    {
        const auto footerButton = layoutFooterButtonAt (p);
        updateHover (footerButton >= 0 ? Hit { Hit::Kind::button, footerButton } : Hit {});
        return;
    }

    updateHover (hitTest (p));
}

void HudView::mouseExit (const juce::MouseEvent&)
{
    if (layoutMode)
        return;

    updateHover ({});
}

void HudView::mouseDown (const juce::MouseEvent& event)
{
    grabKeyboardFocus();

    const auto scaleX = designWidth / static_cast<float> (juce::jmax (1, getWidth()));
    const auto scaleY = designHeight / static_cast<float> (juce::jmax (1, getHeight()));
    const auto p = juce::Point<float> (event.position.x * scaleX, event.position.y * scaleY);

    if (layoutMode)
    {
        if (const auto footerButton = layoutFooterButtonAt (p); footerButton >= 0)
        {
            handleButton (static_cast<Button> (footerButton), p);
            return;
        }

        const auto block = blockAt (p);

        if (block >= 0)
        {
            layoutSelected = block;
            layoutDragging = true;
            layoutDragPoint = p;
            layoutDragOffset = blockTransform[static_cast<size_t> (block)].offset;
        }

        repaint();
        return;
    }

    const auto hit = hitTest (p);

    updateHover (hit);

    if (hit.kind == Hit::Kind::button)
    {
        // 预设法在鼠标松开时再弹菜单：按下时弹会被随后的 mouseUp 立刻关掉。
        if (static_cast<Button> (hit.index) == Button::presetField)
        {
            presetFieldPressed = true;
            return;
        }

        handleButton (static_cast<Button> (hit.index), p);
        return;
    }

    if (hit.kind == Hit::Kind::knob)
    {
        beginKnobDrag (hit.index, p);
        return;
    }

    if (hit.kind == Hit::Kind::fader)
        beginFaderDrag (hit.index, p);
}

void HudView::mouseDrag (const juce::MouseEvent& event)
{
    const auto scaleX = designWidth / static_cast<float> (juce::jmax (1, getWidth()));
    const auto scaleY = designHeight / static_cast<float> (juce::jmax (1, getHeight()));
    const auto p = juce::Point<float> (event.position.x * scaleX, event.position.y * scaleY);

    if (layoutMode)
    {
        if (layoutDragging)
        {
            blockTransform[static_cast<size_t> (layoutSelected)].offset
                = layoutDragOffset + (p - layoutDragPoint);
            repaint();
        }

        return;
    }

    for (int i = 0; i < static_cast<int> (knobs.size()); ++i)
    {
        auto& knob = knobs[static_cast<size_t> (i)];

        if (! knob.dragging)
            continue;

        const auto norm = juce::jlimit (0.0f, 1.0f,
                                       knob.dragStartNorm + (knob.dragStartY - p.y) / 180.0f);
        setKnobNorm (i, norm);
    }

    const auto local = transformFor (blockFaders).toLocal (p);

    for (int i = 0; i < static_cast<int> (faders.size()); ++i)
    {
        auto& fader = faders[static_cast<size_t> (i)];

        if (! fader.dragging)
            continue;

        const auto norm = juce::jlimit (0.0f, 1.0f,
                                       fader.dragStartNorm + faderProjection (fader, local)
                                                                - fader.dragStartProj);
        setFaderNorm (i, norm);
    }
}

void HudView::mouseUp (const juce::MouseEvent& event)
{
    if (layoutMode)
    {
        if (layoutDragging)
        {
            layoutDragging = false;
            saveLayout();
        }

        return;
    }

    if (presetFieldPressed)
    {
        presetFieldPressed = false;

        const auto scaleX = designWidth / static_cast<float> (juce::jmax (1, getWidth()));
        const auto scaleY = designHeight / static_cast<float> (juce::jmax (1, getHeight()));
        const auto p = juce::Point<float> (event.position.x * scaleX, event.position.y * scaleY);

        if (buttonRect (Button::presetField).contains (p))
        {
            buttonFlashFrame = frameNow();
            buttonFlashIndex = static_cast<int> (Button::presetField);
            showPresetMenu();
            repaint();
        }
    }

    bool released = false;

    for (auto& knob : knobs)
    {
        if (! knob.dragging)
            continue;

        knob.dragging = false;
        knob.releaseFrame = frameNow();

        if (knob.attachment != nullptr)
            knob.attachment->endGesture();

        released = true;
    }

    for (auto& fader : faders)
    {
        if (! fader.dragging)
            continue;

        fader.dragging = false;
        fader.releaseFrame = frameNow();

        if (fader.attachment != nullptr)
            fader.attachment->endGesture();

        released = true;
    }

    if (released)
        repaint();
}

void HudView::mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails& wheel)
{
    if (! layoutMode)
        return;

    if (std::abs (wheel.deltaY) < 0.001f)
        return;

    auto& target = blockTransform[static_cast<size_t> (layoutSelected)];

    if (juce::ModifierKeys::getCurrentModifiers().isShiftDown())
    {
        target.rotation = std::fmod (target.rotation + (wheel.deltaY > 0.0f ? 5.0f : -5.0f) + 360.0f,
                                     360.0f);
    }
    else
    {
        const auto factor = wheel.deltaY > 0.0f ? 1.05f : 1.0f / 1.05f;
        target.scale = juce::jlimit (0.3f, 3.0f, target.scale * factor);
    }

    saveLayout();
    repaint();
}

// =======================================================================
// 拖动 / 参数
// =======================================================================

void HudView::beginKnobDrag (int index, juce::Point<float> p)
{
    auto& knob = knobs[static_cast<size_t> (index)];
    knob.dragging = true;
    knob.grabFrame = frameNow();
    knob.releaseFrame = -1000;
    knob.dragStartY = p.y;
    knob.dragStartNorm = knob.targetNorm;

    if (knob.attachment != nullptr)
        knob.attachment->beginGesture();

    repaint();
}

void HudView::beginFaderDrag (int index, juce::Point<float> p)
{
    auto& fader = faders[static_cast<size_t> (index)];
    fader.dragging = true;
    fader.grabFrame = frameNow();
    fader.releaseFrame = -1000;
    fader.dragStartProj = faderProjection (fader, transformFor (blockFaders).toLocal (p));
    fader.dragStartNorm = fader.targetNorm;

    if (fader.attachment != nullptr)
        fader.attachment->beginGesture();

    repaint();
}

float HudView::faderProjection (const FaderState& fader, juce::Point<float> p) const
{
    const auto v = fader.end - fader.root;
    const auto lengthSquared = v.x * v.x + v.y * v.y;

    if (lengthSquared <= 1.0f)
        return 0.0f;

    return juce::jlimit (0.0f, 1.0f,
                         ((p - fader.root).x * v.x + (p - fader.root).y * v.y) / lengthSquared);
}

void HudView::setKnobNorm (int index, float norm)
{
    auto& knob = knobs[static_cast<size_t> (index)];
    norm = juce::jlimit (0.0f, 1.0f, norm);
    knob.targetNorm = norm;

    if (knob.param != nullptr)
    {
        knob.denormValue = knob.param->convertFrom0to1 (norm);

        if (knob.attachment != nullptr)
            knob.attachment->setValueAsPartOfGesture (knob.denormValue);
    }

    setEvent (juce::String (kKnobShortNames[index]).toLowerCase() + ".set :: "
              + knobValueText (knob.denormValue) + "  >  ok");
    lastFrame = -1;
    repaint();
}

void HudView::setFaderNorm (int index, float norm)
{
    auto& fader = faders[static_cast<size_t> (index)];
    norm = juce::jlimit (0.0f, 1.0f, norm);
    fader.targetNorm = norm;

    if (fader.param != nullptr)
    {
        fader.denormValue = fader.param->convertFrom0to1 (norm);

        if (fader.attachment != nullptr)
            fader.attachment->setValueAsPartOfGesture (fader.denormValue);
    }

    setEvent (juce::String (kFaderShortNames[index]).toLowerCase() + ".set :: "
              + faderValueText (fader.denormValue, fader.decimals) + "  >  ok");
    lastFrame = -1;
    repaint();
}

// =======================================================================
// 顶栏动作
// =======================================================================

void HudView::handleButton (Button button, juce::Point<float>)
{
    buttonFlashFrame = frameNow();
    buttonFlashIndex = static_cast<int> (button);

    switch (button)
    {
        case Button::prev:
        case Button::next:
        {
            const auto total = processor.presetManager.getNumPresets();

            if (total <= 0)
            {
                setTransient ("SYS :: NO PRESET", 45);
                break;
            }

            const auto current = processor.presetManager.getCurrentIndex();
            const auto delta = button == Button::next ? 1 : -1;
            const auto target = ((current + delta) % total + total) % total;
            processor.presetManager.loadPreset (target);
            setEvent ("preset.load :: " + processor.presetManager.getPresetName (target) + "  >  ok");
            setTransient ("SYS :: PRESET", 45);
            break;
        }

        case Button::presetField:
            showPresetMenu();
            break;

        case Button::save:
            beginSave();
            break;

        case Button::del:
            deleteCurrentPreset();
            break;

        case Button::typeUp:
            cycleType (1);
            break;

        case Button::typeDown:
            cycleType (-1);
            break;

        case Button::type:
            cycleType (1);
            break;

        case Button::layout:
            toggleLayoutEditor();
            break;

        case Button::layoutSave:
            beginLayoutSave();
            break;

        case Button::layoutLoad:
            showLayoutPresetMenu();
            break;

        case Button::layoutReset:
            resetLayout();
            saveLayout();
            lastFrame = -1;
            setEvent ("layout.reset :: default  >  ok");
            setTransient ("SYS :: RESET", 60);
            break;

        case Button::count:
            break;
    }

    repaint();
}

void HudView::showPresetMenu()
{
    juce::PopupMenu menu;

    const auto total = processor.presetManager.getNumPresets();
    const auto factory = PresetManager::getNumFactoryPresets();
    const auto current = processor.presetManager.getCurrentIndex();

    menu.addSectionHeader ("FACTORY PRESETS");

    for (int i = 0; i < juce::jmin (factory, total); ++i)
        menu.addItem (i + 1, processor.presetManager.getPresetName (i), true, i == current);

    if (total > factory)
    {
        menu.addSeparator();
        menu.addSectionHeader ("USER PRESETS");

        for (int i = factory; i < total; ++i)
            menu.addItem (i + 1, processor.presetManager.getPresetName (i), true, i == current);
    }

    auto area = localAreaToGlobal (buttonRect (Button::presetField).toNearestInt());
    juce::Component::SafePointer<HudView> safeThis (this);

    menu.showMenuAsync (juce::PopupMenu::Options()
                            .withTargetScreenArea (area)
                            .withMinimumWidth (220),
                        [safeThis] (int result)
                        {
                            if (result <= 0)
                                return;

                            if (auto* view = safeThis.getComponent())
                            {
                                view->processor.presetManager.loadPreset (result - 1);
                                view->setEvent ("preset.load :: "
                                                + view->processor.presetManager.getPresetName (result - 1)
                                                + "  >  ok");
                                view->setTransient ("SYS :: PRESET", 45);
                                view->repaint();
                            }
                        });
}

void HudView::beginSave()
{
    if (nameEditor.isVisible())
    {
        commitSave();
        return;
    }

    saveMode = true;

    const auto current = processor.presetManager.getCurrentIndex();
    auto defaultName = processor.presetManager.getNumPresets() > current
                           ? processor.presetManager.getPresetName (current)
                           : juce::String ("Preset");

    if (defaultName.isEmpty())
        defaultName = "Preset";

    nameEditor.setBounds (presetTextRect().toNearestInt());
    nameEditor.setText (defaultName, false);
    nameEditor.selectAll();
    nameEditor.setVisible (true);
    nameEditor.grabKeyboardFocus();

    setTransient ("SYS :: INPUT", 90);
    repaint();
}

void HudView::commitSave()
{
    if (! nameEditor.isVisible())
        return;

    if (layoutSaveMode)
    {
        commitLayoutSave();
        return;
    }

    const auto name = nameEditor.getText().trim();
    nameEditor.setVisible (false);
    saveMode = false;

    if (name.isEmpty())
    {
        setEvent ("preset.save :: name empty  >  err");
        setTransient ("SYS :: NAME?", 60);
        repaint();
        return;
    }

    juce::String error;

    if (processor.presetManager.saveUserPreset (name, error))
    {
        const auto legalName = juce::File::createLegalFileName (name);
        int found = -1;

        for (int i = PresetManager::getNumFactoryPresets();
             i < processor.presetManager.getNumPresets(); ++i)
        {
            if (processor.presetManager.getPresetName (i).equalsIgnoreCase (legalName))
            {
                found = i;
                break;
            }
        }

        if (found >= 0)
            processor.presetManager.setCurrentIndex (found);

        setEvent ("preset.save :: " + name + "  >  ok");
        setTransient ("SYS :: SAVED", 60);
    }
    else
    {
        setEvent ("preset.save :: " + error);
        setTransient ("SYS :: SAVE ERR", 90);
    }

    grabKeyboardFocus();
    repaint();
}

void HudView::cancelSave()
{
    if (! nameEditor.isVisible())
        return;

    nameEditor.setVisible (false);
    saveMode = false;
    layoutSaveMode = false;
    setTransient ("SYS :: CANCEL", 30);
    repaint();
}

void HudView::deleteCurrentPreset()
{
    const auto index = processor.presetManager.getCurrentIndex();

    if (! processor.presetManager.isUserPreset (index))
    {
        setEvent ("preset.del :: factory  >  denied");
        setTransient ("SYS :: FACTORY", 45);
        repaint();
        return;
    }

    const auto now = juce::Time::getMillisecondCounterHiRes() * 0.001;

    if (now > deleteArmedUntil)
    {
        deleteArmedUntil = now + 3.0;
        setTransient ("SYS :: DEL? AGAIN", 90);
        repaint();
        return;
    }

    deleteArmedUntil = 0.0;

    const auto name = processor.presetManager.getPresetName (index);
    juce::String error;

    if (processor.presetManager.deleteUserPreset (index, error))
    {
        setEvent ("preset.del :: " + name + "  >  ok");
        setTransient ("SYS :: DELETED", 60);

        // 删除后 PresetManager 会把当前索引挪到相邻项；这里同时载入它，
        // 否则界面上的预设名与参数值会对不上。
        if (processor.presetManager.getNumPresets() > 0)
            processor.presetManager.loadPreset (
                juce::jmin (index, processor.presetManager.getNumPresets() - 1));
    }
    else
    {
        setEvent ("preset.del :: " + error);
        setTransient ("SYS :: DEL ERR", 60);
    }

    repaint();
}

void HudView::cycleType (int delta)
{
    const auto next = (typeIndex + delta + 3) % 3;
    typeIndex = next;

    if (typeAttachment != nullptr)
        typeAttachment->setValueAsCompleteGesture (static_cast<float> (next));

    setTransient ("SYS :: TYPE", 45);
    repaint();
}

// =======================================================================
// 状态 / 日志
// =======================================================================

juce::String HudView::currentStatus() const
{
    if (layoutMode)
        return "SYS :: LAYOUT";

    if (saveMode)
        return "SYS :: INPUT";

    for (int i = 0; i < static_cast<int> (faders.size()); ++i)
        if (faders[static_cast<size_t> (i)].dragging)
            return juce::String ("SYS :: ADJ ") + kFaderShortNames[i];

    for (int i = 0; i < static_cast<int> (knobs.size()); ++i)
        if (knobs[static_cast<size_t> (i)].dragging)
            return juce::String ("SYS :: ADJ ") + kKnobShortNames[i];

    if (deleteArmedUntil > juce::Time::getMillisecondCounterHiRes() * 0.001)
        return "SYS :: DEL? AGAIN";

    if (transientUntilFrame > frameNow())
        return transientStatus;

    return "SYS :: STANDBY";
}

void HudView::setEvent (const juce::String& eventText)
{
    lastEvent = eventText;
    lastEventTime = juce::Time::getCurrentTime().formatted ("%H:%M:%S");
}

void HudView::setTransient (const juce::String& status, int frames)
{
    transientStatus = status;
    transientUntilFrame = frameNow() + frames;
}

void HudView::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source != &processor.presetManager)
        return;

    refreshFromParams (true);
    repaint();
}

// =======================================================================
// 绘制
// =======================================================================

void HudView::paint (juce::Graphics& g)
{
    if constexpr (kPopInEnabled)
    {
        // 第一帧真正上屏时才启动入场时钟：从创建到显示之间的时间不算进动画
        if (startTime <= 0.0 && isShowing())
        {
            startTime = juce::Time::getMillisecondCounterHiRes() * 0.001;
            lastTickTime = startTime;
        }
    }

    // 背景：GPU 通路可用时铺场景着色器；不可用或被关掉时保持原来的纯黑底。
    // 着色器按设备像素自己铺满组件，所以画在设计坐标的缩放变换之前。
    if (backdrop.isGpuActive() && backdrop.isEnabled())
        backdrop.paintBackground (g, getLocalBounds(), kBackground);
    else
        g.fillAll (kBackground);

    const auto scaleX = static_cast<float> (getWidth()) / designWidth;
    const auto scaleY = static_cast<float> (getHeight()) / designHeight;

    juce::Graphics::ScopedSaveState saveState (g);
    g.addTransform (juce::AffineTransform::scale (scaleX, scaleY));
    g.setImageResamplingQuality (juce::Graphics::highResamplingQuality);

    const auto frame = frameNow();

    if constexpr (! kPopInEnabled)
    {
        // 浮现之前的行为：0–1 帧纯黑，第 2 帧结构硬切出现，第 4 帧升到全亮
        if (frame < 2)
            return;
    }

    drawChrome (g);

    if constexpr (kPopInEnabled)
    {
        // 3D 浮现入场（当前关闭）：每个控件按自己的起始帧依次出现
        // （顶栏 → MIX → SHMR → 五个推子 → 数据块 → 页脚），从 0.5 倍尺度机械式弹到位。
        const auto frameF = static_cast<float> (frame);

        {
            juce::Graphics::ScopedSaveState topState (g);
            g.addTransform (popMatrixAt ({ designWidth * 0.5f, 38.0f }, 0.0f));
            drawTopBar (g, frame);
        }

        {
            juce::Graphics::ScopedSaveState finState (g);
            g.addTransform (matrixFor (blockFaders));
            drawFins (g, structureAlpha (4));
        }

        for (int i = 0; i < static_cast<int> (knobs.size()); ++i)
        {
            const auto delay = 1.0f + static_cast<float> (i);

            if (frameF < delay)
                continue;

            juce::Graphics::ScopedSaveState knobState (g);
            g.addTransform (popMatrixAt (blockDesignBounds (blockForKnob (i)).getCentre(), delay));
            drawKnob (g, i, frame);
        }

        const auto faderMatrix = matrixFor (blockFaders);

        for (int i = 0; i < static_cast<int> (faders.size()); ++i)
        {
            const auto delay = 3.0f + static_cast<float> (i);

            if (frameF < delay)
                continue;

            const auto mid = (faders[static_cast<size_t> (i)].root
                              + faders[static_cast<size_t> (i)].end) * 0.5f;
            float px = mid.x;
            float py = mid.y;
            faderMatrix.transformPoint (px, py);

            juce::Graphics::ScopedSaveState faderState (g);
            g.addTransform (popMatrixAt ({ px, py }, delay));
            drawFader (g, i, frame);
        }

        // 读数是信息层：压在控件与 hover 刀片扇之上，避免被装饰盖住
        drawDecorations (g);

        if (frameF >= 10.0f)
        {
            juce::Graphics::ScopedSaveState footState (g);
            g.addTransform (popMatrixAt ({ designWidth * 0.5f, 505.0f }, 10.0f));
            drawFooter (g, frame);
        }
    }
    else
    {
        drawTopBar (g, frame);

        {
            // 鳍形装饰属于推子板块：跟着推子组一起平移 / 缩放 / 旋转
            juce::Graphics::ScopedSaveState finState (g);
            g.addTransform (matrixFor (blockFaders));
            drawFins (g, alphaFor (4));
        }

        for (int i = 0; i < static_cast<int> (knobs.size()); ++i)
            drawKnob (g, i, frame);

        for (int i = 0; i < static_cast<int> (faders.size()); ++i)
            drawFader (g, i, frame);

        // 读数是信息层：压在控件与 hover 刀片扇之上，避免被装饰盖住
        drawDecorations (g);

        drawFooter (g, frame);
    }

    if (layoutMode)
        drawLayoutOverlay (g, frame);

    // 布局预设命名：垫一块面板，让内联输入框在 HUD 里读作"输入格"
    if (layoutSaveMode && nameEditor.isVisible())
    {
        const auto panel = rect (330.0f, 236.0f, 300.0f, 26.0f).expanded (10.0f, 9.0f);

        g.setColour (kBackground.withAlpha (0.97f));
        g.fillRect (panel);
        g.setColour (kText.withAlpha (0.85f));
        g.drawRect (panel, 1.0f);
        drawTextAtBaseline (g, "SAVE LAYOUT PRESET", panel.getX(), panel.getY() - 5.0f,
                            hudFont (7.0f, 0.14f), kDim);
    }
}

void HudView::resized()
{
    if (! nameEditor.isVisible())
        return;

    const auto designRect = layoutSaveMode ? rect (330.0f, 236.0f, 300.0f, 26.0f)
                                           : presetTextRect();
    const auto scaleX = static_cast<float> (getWidth()) / designWidth;
    const auto scaleY = static_cast<float> (getHeight()) / designHeight;

    nameEditor.setBounds (juce::Rectangle<int> (juce::roundToInt (designRect.getX() * scaleX),
                                                juce::roundToInt (designRect.getY() * scaleY),
                                                juce::roundToInt (designRect.getWidth() * scaleX),
                                                juce::roundToInt (designRect.getHeight() * scaleY)));
}

// -----------------------------------------------------------------------
// 静态版面：顶栏分隔线
// -----------------------------------------------------------------------

void HudView::drawChrome (juce::Graphics& g)
{
    // 浮现入场时结构线属于 power-on 层，浮现期间不画；关闭浮现时沿用原来的 alphaFor 节奏
    const auto alpha = kPopInEnabled ? structureAlpha (2) : alphaFor (2);

    g.setColour (kText.withAlpha (0.10f * alpha));
    g.drawLine (24.0f, 54.0f, 936.0f, 54.0f, 1.0f);
}

// -----------------------------------------------------------------------
// 装饰层：只保留真实读数块，去掉无意义的标尺 / 坐标系
// -----------------------------------------------------------------------

void HudView::drawDecorations (juce::Graphics& g)
{
    const auto alpha = alphaFor (4);

    if (alpha <= 0.0f)
        return;

    const auto labelFont = hudFont (6.5f, 0.10f);
    const auto valueFont = hudFont (6.5f, 0.06f);

    auto drawDataRow = [&] (const juce::String& label, const juce::String& value,
                            float labelX, float valueRight, float baseline)
    {
        drawTextAtBaseline (g, label, labelX, baseline, labelFont, kMicro.withAlpha (0.85f * alpha));

        const auto valueWidth = textWidth (valueFont, value);
        drawTextAtBaseline (g, value, valueRight - valueWidth, baseline,
                            valueFont, kDim.withAlpha (0.80f * alpha));
    };

    // ---- 引擎侧：采样率 / 缓冲 / CPU / 延迟（全部来自处理器实时状态）----
    const auto sampleRate = processor.getSampleRate();
    const auto srText = sampleRate > 0.0 ? juce::String (sampleRate / 1000.0, 1) + " kHz"
                                         : juce::String ("---");
    const auto latencyMs = sampleRate > 0.0
                               ? static_cast<double> (processor.getLatencySamples()) * 1000.0
                                     / sampleRate
                               : 0.0;

    if (! kPopInEnabled || frameNow() >= 8)
    {
        juce::Graphics::ScopedSaveState dataState (g);

        if constexpr (kPopInEnabled)
            g.addTransform (popMatrixAt (blockDesignBounds (blockDataA).getCentre(), 8.0f));

        g.addTransform (matrixFor (blockDataA));

        drawDataRow ("[ sr ]", srText, 0.0f, 96.0f, 12.0f);
        drawDataRow ("[ blk ]", juce::String (processor.getBlockSize()), 0.0f, 96.0f, 25.0f);
        drawDataRow ("[ dsp ]", juce::String (processor.getDspLoadPercent(), 1) + " %", 0.0f, 96.0f, 38.0f);
        drawDataRow ("[ lat ]", juce::String (latencyMs, 1) + " ms", 0.0f, 96.0f, 51.0f);
    }

    // ---- 混响侧：算法 / 预设位 / 衰尾 / 湿比 ----
    static const char* const algNames[] = { "PLATE", "ROOM", "SPRING" };
    const auto total = processor.presetManager.getNumPresets();
    const auto index = total > 0 ? processor.presetManager.getCurrentIndex() + 1 : 0;
    const auto programText = juce::String::formatted ("%03d/%03d", index, total);

    // 与三套 tank 共用同一名义映射：RT60 = 0.25 * 120^decay
    const auto decay01 = juce::jlimit (0.0f, 1.0f, faders[0].denormValue / 100.0f);
    const auto tailSeconds = 0.25 * std::pow (120.0, static_cast<double> (decay01));

    if (! kPopInEnabled || frameNow() >= 9)
    {
        juce::Graphics::ScopedSaveState dataState (g);

        if constexpr (kPopInEnabled)
            g.addTransform (popMatrixAt (blockDesignBounds (blockDataB).getCentre(), 9.0f));

        g.addTransform (matrixFor (blockDataB));

        drawDataRow ("[ alg ]", algNames[juce::jlimit (0, 2, typeIndex)], 0.0f, 96.0f, 12.0f);
        drawDataRow ("[ prg ]", programText, 0.0f, 96.0f, 25.0f);
        drawDataRow ("[ tail ]", juce::String (tailSeconds, 2) + " s", 0.0f, 96.0f, 38.0f);
        drawDataRow ("[ wet ]", juce::String (knobs[0].denormValue, 1) + " %", 0.0f, 96.0f, 51.0f);
    }
}

// -----------------------------------------------------------------------
// 顶栏
// -----------------------------------------------------------------------

void HudView::drawTopBar (juce::Graphics& g, int frame)
{
    const auto frameAlpha = alphaFor (2);
    const auto alpha = alphaFor (4);

    if (frameAlpha <= 0.0f)
        return;

    auto frameRect = [&] (juce::Rectangle<float> area, float opacity)
    {
        g.setColour (kText.withAlpha (opacity * frameAlpha));
        g.drawRect (area.reduced (0.5f), 1.0f);
    };

    frameRect (buttonRect (Button::prev), 0.30f);
    frameRect (buttonRect (Button::next), 0.30f);
    frameRect (buttonRect (Button::presetField), 0.18f);
    frameRect (rect (62.0f, 19.0f, 34.0f, 18.0f), 0.32f);
    frameRect (buttonRect (Button::save), 0.32f);
    frameRect (buttonRect (Button::del), 0.32f);
    frameRect (rect (748.0f, 12.0f, 52.0f, 32.0f), 0.32f);

    // 白色 TYPE 主块
    g.setColour (kText.withAlpha (0.94f * frameAlpha));
    g.fillRect (buttonRect (Button::type));

    g.setColour (kDim.withAlpha (0.75f * frameAlpha));
    g.drawLine (40.0f, 24.0f, 33.0f, 28.0f, 1.3f);
    g.drawLine (33.0f, 28.0f, 40.0f, 32.0f, 1.3f);
    g.drawLine (272.0f, 24.0f, 279.0f, 28.0f, 1.3f);
    g.drawLine (279.0f, 28.0f, 272.0f, 32.0f, 1.3f);

    // 上下切换三角
    juce::Path up, down;
    up.addTriangle (898.0f, 24.0f, 906.0f, 24.0f, 902.0f, 19.0f);
    down.addTriangle (898.0f, 30.0f, 906.0f, 30.0f, 902.0f, 35.0f);
    g.setColour (kText.withAlpha (0.55f * frameAlpha));
    g.fillPath (up);
    g.fillPath (down);

    if (alpha <= 0.0f)
        return;

    drawCentredText (g, "PRE", rect (62.0f, 19.0f, 34.0f, 18.0f), hudFont (7.0f, 0.13f),
                     kDim.withAlpha (0.85f * alpha));
    drawCentredText (g, "SAVE", buttonRect (Button::save), hudFont (7.0f, 0.13f),
                     kDim.withAlpha (0.85f * alpha));
    drawCentredText (g, "DEL", buttonRect (Button::del), hudFont (7.0f, 0.13f),
                     kDim.withAlpha (0.85f * alpha));
    drawCentredText (g, "TYPE", rect (748.0f, 12.0f, 52.0f, 32.0f), hudFont (7.0f, 0.13f),
                     kDim.withAlpha (0.85f * alpha));

    const auto presetName = processor.presetManager.getNumPresets() > 0
                                ? processor.presetManager.getPresetName (
                                      processor.presetManager.getCurrentIndex())
                                : juce::String ("---");

    if (! saveMode)
    {
        {
            juce::Graphics::ScopedSaveState clip (g);
            g.reduceClipRegion (juce::Rectangle<int> (100, 12, 114, 32));
            drawTextAtBaseline (g, presetName, 104.0f, 34.0f,
                                hudFont (13.0f, 0.03f), kText.withAlpha (alpha));
        }

        const auto total = processor.presetManager.getNumPresets();
        const auto index = total > 0 ? processor.presetManager.getCurrentIndex() + 1 : 0;
        const auto count = juce::String::formatted ("%03d/%03d", index, total);
        const auto countFont = hudFont (7.0f, 0.086f);
        drawTextAtBaseline (g, count, 250.0f - textWidth (countFont, count), 34.0f,
                            countFont, kMicro.withAlpha (alpha));
    }

    static const char* const typeNames[] = { "PLATE", "ROOM", "SPRING" };
    const auto typeText = juce::String (typeNames[juce::jlimit (0, 2, typeIndex)]);
    const auto typeFont = hudFont (12.0f, 0.10f);
    drawCentredText (g, typeText, buttonRect (Button::type), typeFont,
                     kBackground.withAlpha (alpha));

    const auto status = currentStatus();
    const auto statusFont = hudFont (7.5f, 0.107f);
    drawTextAtBaseline (g, status, 690.0f - textWidth (statusFont, status), 34.0f,
                        statusFont, kMicro.withAlpha (alpha));

    if (frame < 4)
        return;

    // hover：主效果 1 帧提亮框线，二级括号 / 基线在 2 帧内向外展开
    if (hoveredButton >= 0 && static_cast<Button> (hoveredButton) != Button::layout)
    {
        const auto button = static_cast<Button> (hoveredButton);
        const auto phase = juce::jlimit (0.0f, 1.0f,
                                         static_cast<float> (frame - buttonHoverFrame) / 2.0f);

        if (button == Button::type)
        {
            const auto area = buttonRect (button).expanded (2.0f);

            g.setColour (kText.withAlpha (0.55f));
            g.drawRect (area, 1.0f);
            drawHoverBrackets (g, area, phase, 0.85f);
        }
        else if (button == Button::typeUp || button == Button::typeDown)
        {
            juce::Path triangle;
            const auto isUp = (button == Button::typeUp);

            if (isUp)
                triangle.addTriangle (898.0f, 24.0f, 906.0f, 24.0f, 902.0f, 19.0f);
            else
                triangle.addTriangle (898.0f, 30.0f, 906.0f, 30.0f, 902.0f, 35.0f);

            g.setColour (kText);
            g.fillPath (triangle);

            // 二级：三角底边向外让出一条短刻度
            const auto y = isUp ? 24.0f - 2.0f - 2.0f * phase : 30.0f + 2.0f + 2.0f * phase;
            const auto half = 4.0f + 4.0f * phase;

            g.setColour (kText.withAlpha (0.85f));
            g.drawLine (902.0f - half, y, 902.0f + half, y, 1.0f);
        }
        else
        {
            drawButtonHover (g, buttonRect (button), phase);
        }
    }

    if (buttonFlashFrame >= 0 && frame - buttonFlashFrame >= 0 && frame - buttonFlashFrame <= 1)
    {
        g.setColour (kText.withAlpha (0.12f));
        g.fillRect (buttonRect (static_cast<Button> (buttonFlashIndex)));
    }

    if (saveMode)
    {
        g.setColour (kText);
        g.fillRect (rect (100.0f, 40.5f, 146.0f, 1.0f));
    }

    if (deleteArmedUntil > juce::Time::getMillisecondCounterHiRes() * 0.001
        && ((frame / 2) % 2) == 0)
    {
        g.setColour (kAlert);
        g.drawRect (buttonRect (Button::del).reduced (0.5f), 1.0f);
    }
}

void HudView::drawButtonHover (juce::Graphics& g, juce::Rectangle<float> area, float phase) const
{
    g.setColour (kText.withAlpha (0.65f));
    g.drawRect (area.reduced (0.5f), 1.0f);

    drawHoverBrackets (g, area, phase, 0.85f);

    // 二级：底边中点向两侧拉出的基线段
    const auto half = area.getWidth() * 0.5f * (0.2f + 0.8f * phase);

    g.setColour (kText.withAlpha (0.55f));
    g.drawLine (area.getCentreX() - half, area.getBottom() + 2.0f,
                area.getCentreX() + half, area.getBottom() + 2.0f, 1.0f);
}

void HudView::drawHoverBrackets (juce::Graphics& g, juce::Rectangle<float> area,
                                 float phase, float alpha) const
{
    const auto out = 2.0f + 3.0f * phase;
    const auto arm = 3.0f + 3.0f * phase;

    g.setColour (kText.withAlpha (alpha));

    for (int sx = -1; sx <= 1; sx += 2)
    {
        for (int sy = -1; sy <= 1; sy += 2)
        {
            const auto corner = juce::Point<float> (sx < 0 ? area.getX() - out : area.getRight() + out,
                                                    sy < 0 ? area.getY() - out : area.getBottom() + out);
            const auto inwardX = sx < 0 ? 1.0f : -1.0f;
            const auto inwardY = sy < 0 ? 1.0f : -1.0f;

            g.drawLine (corner.x, corner.y, corner.x + arm * inwardX, corner.y, 1.0f);
            g.drawLine (corner.x, corner.y, corner.x, corner.y + arm * inwardY, 1.0f);
        }
    }
}

void HudView::drawFooter (juce::Graphics& g, int frame)
{
    juce::ignoreUnused (frame);

    if constexpr (! kPopInEnabled)
    {
        // 浮现关闭时沿用原来的延迟：前 20 帧不画页脚
        if (frame < 20)
            return;
    }

    g.setColour (kText.withAlpha (0.08f));
    g.drawLine (24.0f, 500.0f, 936.0f, 500.0f, 1.0f);

    const auto log = "[ " + lastEventTime + " ]  " + lastEvent;

    {
        juce::Graphics::ScopedSaveState clip (g);
        g.reduceClipRegion (juce::Rectangle<int> (20, 500, layoutMode ? 600 : 840, 20));
        drawTextAtBaseline (g, log, 24.0f, 514.0f, hudFont (7.0f, 0.086f), kMicro);
    }

    // 布局模式下多出三个动作：保存预设 / 载入预设 / 复位
    if (layoutMode)
    {
        struct LayoutAction { Button id; const char* text; };
        static const LayoutAction actions[] =
        {
            { Button::layoutSave,  "SAVE" },
            { Button::layoutLoad,  "LOAD" },
            { Button::layoutReset, "RESET" },
        };

        for (const auto& action : actions)
        {
            const auto area = buttonRect (action.id);
            const auto hovered = hoveredButton == static_cast<int> (action.id);
            const auto font = hudFont (7.0f, 0.10f);

            drawTextAtBaseline (g, action.text, area.getX(), 514.0f, font,
                                hovered ? kText : kMicro);

            if (hovered)
                g.fillRect (juce::Rectangle<float> (area.getX(), 517.0f, area.getWidth(), 1.0f));
        }
    }

    const auto layoutText = layoutMode ? "LAYOUT ON" : "LAYOUT";
    const auto layoutFont = hudFont (7.0f, 0.10f);
    const auto layoutColour = (hoveredButton == static_cast<int> (Button::layout) || layoutMode)
                                  ? kText
                                  : kMicro;

    drawTextAtBaseline (g, layoutText, 936.0f - textWidth (layoutFont, layoutText), 514.0f,
                        layoutFont, layoutColour);

    if (layoutMode)
    {
        g.setColour (kText.withAlpha (0.55f));
        g.fillRect (rect (870.0f, 516.0f, 66.0f, 1.0f));
    }
    else if (hoveredButton == static_cast<int> (Button::layout))
    {
        // 二级：文字左右两侧的括号 + 底线由中心向两侧展开
        const auto phase = juce::jlimit (0.0f, 1.0f,
                                         static_cast<float> (frame - buttonHoverFrame) / 2.0f);
        const auto area = buttonRect (Button::layout);

        drawHoverBrackets (g, area, phase, 0.85f);

        const auto half = area.getWidth() * 0.5f * (0.2f + 0.8f * phase);
        g.setColour (kText.withAlpha (0.55f));
        g.drawLine (area.getCentreX() - half, 517.0f, area.getCentreX() + half, 517.0f, 1.0f);
    }
}

// -----------------------------------------------------------------------
// 旋钮
// -----------------------------------------------------------------------

void HudView::drawKnobStatic (juce::Graphics& g, const KnobState& knob, float alpha) const
{
    const auto centre = knob.centre;
    const auto radius = knob.radius;

    // 盘底：极淡的白光，提供层次但不抢主件
    g.setColour (kText.withAlpha (0.030f * alpha));
    g.fillEllipse (juce::Rectangle<float> (centre.x - radius, centre.y - radius,
                                           radius * 2.0f, radius * 2.0f));

    if (knob.label != nullptr && ! knob.labelBox.isEmpty())
    {
        g.setColour (kText.withAlpha (alpha));
        g.fillRect (knob.labelBox);
        drawCentredText (g, knob.label, knob.labelBox, hudFont (knob.labelSize, 0.16f),
                         kBackground.withAlpha (alpha));
    }
    else if (knob.label != nullptr)
    {
        drawTextAtBaseline (g, knob.label, knob.labelX, knob.labelBaseline,
                            hudFont (knob.labelSize, 0.20f), kDim.withAlpha (alpha));
    }

    // 外环
    const auto ring = makeArcPath (centre, radius, 140.0f, 400.0f);
    g.setColour (kText.withAlpha (0.16f * alpha));
    g.strokePath (ring, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved,
                                              juce::PathStrokeType::butt));
}

void HudView::drawKnob (juce::Graphics& g, int index, int frame)
{
    auto& knob = knobs[static_cast<size_t> (index)];
    const auto alpha = alphaFor (knob.appearFrame);

    if (alpha <= 0.0f)
        return;

    juce::Graphics::ScopedSaveState blockState (g);
    g.addTransform (matrixFor (blockForKnob (index)));

    const auto norm = juce::jlimit (0.0f, 1.0f, knob.displayNorm);
    const auto angle = 140.0f + norm * 260.0f;
    const auto centre = knob.centre;
    const auto radius = knob.radius;
    const auto hoverPhase = knob.hovered
                                ? juce::jlimit (0.0f, 1.0f, static_cast<float> (frame - knob.hoverFrame) / 2.0f)
                                : 0.0f;
    drawKnobStatic (g, knob, alpha);

    // 大旋钮的 MIN / MAX 端点标记
    if (index == 0)
    {
        const auto minAnchor = pointOnCircle (centre, radius + 8.0f, 140.0f);
        const auto maxAnchor = pointOnCircle (centre, radius + 8.0f, 40.0f);
        drawTextAtBaseline (g, "MIN", minAnchor.x - 7.0f, minAnchor.y + 2.0f,
                            hudFont (5.5f, 0.15f), kMicro.withAlpha (alpha));
        drawTextAtBaseline (g, "MAX", maxAnchor.x - 7.0f, maxAnchor.y + 2.0f,
                            hudFont (5.5f, 0.15f), kMicro.withAlpha (alpha));
    }

    // idle 雷达扫掠：3 帧穿过 260°，hover 时周期从 4s 缩到 1.33s
    if (frame >= 34 && ! knob.dragging)
    {
        const auto period = knob.hovered ? 40 : 120;
        auto cycle = (frame - 34 - knob.scanPhase) % period;

        if (cycle < 0)
            cycle += period;

        if (cycle < 3)
        {
            const auto progress = static_cast<float> (cycle) / 2.0f;
            const auto sweepCentre = 140.0f + progress * 260.0f;
            const auto innerRadius = juce::jmax (2.0f, radius - 3.0f);

            juce::Path sector;
            sector.startNewSubPath (centre.x, centre.y);

            const auto from = sweepCentre - 15.0f;
            const auto to = sweepCentre + 15.0f;
            constexpr int steps = 8;

            for (int i = 0; i <= steps; ++i)
            {
                const auto p = pointOnCircle (centre, innerRadius,
                                              from + (to - from) * static_cast<float> (i)
                                                         / static_cast<float> (steps));
                sector.lineTo (p.x, p.y);
            }

            sector.closeSubPath();

            g.setColour (kText.withAlpha (knob.hovered ? 0.10f : 0.06f));
            g.fillPath (sector);
        }
    }

    // 值弧（白色）
    if (norm > 0.001f)
    {
        const auto path = makeArcPath (centre, radius, 140.0f, angle);
        g.setColour (kText.withAlpha (0.55f * alpha));
        g.strokePath (path, juce::PathStrokeType (4.0f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
        g.setColour (kText.withAlpha (0.95f * alpha));
        g.strokePath (path, juce::PathStrokeType (1.2f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::rounded));
    }

    // hover / grab：外环与指针附近的环段提亮
    if (knob.hovered || knob.dragging)
    {
        const auto ring = makeArcPath (centre, radius, 140.0f, 400.0f);
        g.setColour (kText.withAlpha (0.30f));
        g.strokePath (ring, juce::PathStrokeType (1.4f, juce::PathStrokeType::curved,
                                                  juce::PathStrokeType::butt));

        const auto spotlight = makeArcPath (centre, radius, angle - 15.0f, angle + 15.0f);
        g.setColour (kText.withAlpha (0.90f));
        g.strokePath (spotlight, juce::PathStrokeType (1.6f, juce::PathStrokeType::curved,
                                                       juce::PathStrokeType::rounded));
    }

    // 指针 + 针尖点
    const auto direction = juce::Point<float> (std::cos (angle * kPi / 180.0f),
                                               std::sin (angle * kPi / 180.0f));
    const auto needleStart = centre + direction * knob.needleInner;
    const auto needleEnd = centre + direction * knob.needleOuter;

    g.setColour (kText.withAlpha (0.90f * alpha));
    g.drawLine (needleStart.x, needleStart.y, needleEnd.x, needleEnd.y, 1.3f);

    const auto tipAlpha = knob.hovered ? 1.0f : 0.95f;
    g.setColour (kText.withAlpha (0.16f * alpha));
    g.fillEllipse (juce::Rectangle<float> (needleEnd.x - 5.0f, needleEnd.y - 5.0f, 10.0f, 10.0f));
    g.setColour (kText.withAlpha (tipAlpha * alpha));
    g.fillEllipse (juce::Rectangle<float> (needleEnd.x - 2.2f, needleEnd.y - 2.2f, 4.4f, 4.4f));

    // hover 二级：针尖外扩的光环 + 环外让出一条径向短刻度
    if (knob.hovered)
    {
        const auto halo = 6.0f + 4.0f * hoverPhase;

        g.setColour (kText.withAlpha (0.30f * hoverPhase * alpha));
        g.drawEllipse (juce::Rectangle<float> (needleEnd.x - halo, needleEnd.y - halo,
                                               halo * 2.0f, halo * 2.0f), 1.0f);

        const auto tickFrom = pointOnCircle (centre, radius + 4.0f, angle);
        const auto tickTo = pointOnCircle (centre, radius + 4.0f + 7.0f * hoverPhase, angle);
        g.setColour (kText.withAlpha (0.80f * hoverPhase * alpha));
        g.drawLine (tickFrom.x, tickFrom.y, tickTo.x, tickTo.y, 1.0f);
    }

    // 针根点（抓取时放大 1.5×）
    const auto centreDot = knob.dragging ? 3.0f : 2.0f;
    g.setColour (kText.withAlpha (0.85f * alpha));
    g.fillEllipse (juce::Rectangle<float> (centre.x - centreDot, centre.y - centreDot,
                                           centreDot * 2.0f, centreDot * 2.0f));

    drawKnobValueText (g, knob, alpha, 2.0f * hoverPhase);

    // ADJ 标签（hover / grab 时出现，release 时错帧闪一次）——全界面唯一的蓝色
    const auto rel = frame - knob.releaseFrame;
    const auto tagVisible = knob.hovered || knob.dragging
                            || (rel >= 0 && rel <= 4 && (rel == 0 || rel >= 3));

    if (tagVisible)
    {
        const auto anchor = juce::Point<float> (centre.x, centre.y - radius - 6.0f);
        g.setColour (kText.withAlpha (0.55f));
        g.drawLine (anchor.x, anchor.y, centre.x, centre.y - radius + 7.0f, 1.0f);
        drawAdjustedTag (g, anchor, true);
    }
}

void HudView::drawKnobValueText (juce::Graphics& g, const KnobState& knob,
                                 float alpha, float bracketExpand) const
{
    const auto value = knobValueText (knob.denormValue);
    const auto bracketFont = hudFont (knob.bracketSize, 0.022f);
    const auto valueFont = hudFont (knob.valueSize, 0.011f);
    const auto unitFont = hudFont (knob.unitSize, 0.075f);

    const juce::String left = "[ ";
    const juce::String right = " ]";
    const juce::String unit = " %";

    const auto width = textWidth (bracketFont, left) + textWidth (valueFont, value)
                       + textWidth (bracketFont, right) + textWidth (unitFont, unit)
                       + bracketExpand * 2.0f;
    auto x = knob.valueCx - width * 0.5f;

    drawTextAtBaseline (g, left, x, knob.valueBaseline, bracketFont, kDim.withAlpha (alpha));
    x += textWidth (bracketFont, left) + bracketExpand;

    drawTextAtBaseline (g, value, x, knob.valueBaseline, valueFont, kText.withAlpha (alpha));
    x += textWidth (valueFont, value) + bracketExpand;

    drawTextAtBaseline (g, right, x, knob.valueBaseline, bracketFont, kDim.withAlpha (alpha));
    x += textWidth (bracketFont, right);

    drawTextAtBaseline (g, unit, x, knob.valueBaseline, unitFont, kMicro.withAlpha (alpha));
}

void HudView::drawAdjustedTag (juce::Graphics& g, juce::Point<float> anchor, bool visible) const
{
    if (! visible)
        return;

    const auto area = rect (anchor.x - 17.0f, anchor.y - 13.0f, 34.0f, 13.0f);
    g.setColour (kGem);
    g.fillRect (area);
    g.setColour (kText);
    g.setFont (hudFont (7.0f, 0.11f));
    g.drawText ("ADJ", area, juce::Justification::centred, false);
}

// -----------------------------------------------------------------------
// 推子
// -----------------------------------------------------------------------

void HudView::drawFaderStatic (juce::Graphics& g, const FaderState& fader, float alpha) const
{
    const auto v = fader.end - fader.root;
    const auto length = v.getDistanceFromOrigin();

    if (length <= 1.0f)
        return;

    const auto u = v / length;
    const auto perp = juce::Point<float> (-u.y, u.x);

    // 刀带：锥形带体（根部宽、末端收尖）+ 两侧亮边，复刻旧版刃式轨道
    const auto edgeA0 = fader.root + perp * kBladeHalfRoot;
    const auto edgeB0 = fader.root - perp * kBladeHalfRoot;
    const auto edgeA1 = fader.root + u * (length * 0.72f) + perp * kBladeHalfMid;
    const auto edgeB1 = fader.root - u * (length * 0.72f) - perp * kBladeHalfMid;

    juce::Path blade;
    blade.startNewSubPath (edgeA0.x, edgeA0.y);
    blade.quadraticTo (edgeA1.x, edgeA1.y, fader.end.x, fader.end.y);
    blade.quadraticTo (edgeB1.x, edgeB1.y, edgeB0.x, edgeB0.y);
    blade.closeSubPath();

    g.setColour (kText.withAlpha (0.09f * alpha));
    g.fillPath (blade);

    g.setColour (kText.withAlpha (0.45f * alpha));
    g.drawLine (edgeA0.x, edgeA0.y, fader.end.x, fader.end.y, 1.0f);
    g.drawLine (edgeB0.x, edgeB0.y, fader.end.x, fader.end.y, 1.0f);

    // 刃芯
    g.setColour (kText.withAlpha (0.35f * alpha));
    g.drawLine (fader.root.x, fader.root.y, fader.end.x, fader.end.y, 1.0f);

    // 根部圆点
    g.setColour (kText.withAlpha (0.45f * alpha));
    g.fillEllipse (juce::Rectangle<float> (fader.root.x - 1.5f, fader.root.y - 1.5f, 3.0f, 3.0f));

    // 静态刻度：沿刀带外侧伸出的 4 根短刻度（hover 时手柄 40px 内聚光）
    const auto handlePos = fader.root + v * fader.displayNorm;

    for (int i = 1; i <= 4; ++i)
    {
        const auto p = fader.root + v * (static_cast<float> (i) / 5.0f);
        const auto near = fader.hovered && p.getDistanceFrom (handlePos) <= 40.0f;

        g.setColour (kText.withAlpha ((near ? 0.45f : 0.16f) * alpha));
        g.drawLine (p.x + perp.x * (kBladeHalfRoot - 0.5f), p.y + perp.y * (kBladeHalfRoot - 0.5f),
                    p.x + perp.x * (kBladeHalfRoot + 3.5f), p.y + perp.y * (kBladeHalfRoot + 3.5f),
                    1.0f);
    }

    // 控件名与读数框
    drawTextAtBaseline (g, fader.label, fader.labelX, fader.labelBaseline,
                        hudFont (7.0f, 0.20f), kDim.withAlpha (alpha));

    g.setColour (kText.withAlpha (alpha));
    g.fillRect (fader.valueBox);
}

void HudView::drawFins (juce::Graphics& g, float alpha) const
{
    // 旧版鳍形装饰：四片贴线生长的鳍片，内端落在根部圆弧、尖端向外收成锐角。
    struct Fin
    {
        juce::Point<float> start, c1, c2, tip, c3, c4, end;
    };

    static const Fin fins[] =
    {
        { { 510.67f, 253.96f }, { 456.75f, 275.40f }, { 468.99f, 308.36f }, { 400.84f, 349.77f },
          { 460.37f, 296.70f }, { 488.31f, 318.05f }, { 524.57f, 272.75f } },
        { { 524.57f, 272.75f }, { 484.77f, 314.97f }, { 509.79f, 339.67f }, { 465.52f, 406.01f },
          { 497.05f, 332.75f }, { 531.39f, 340.28f }, { 545.12f, 283.91f } },
        { { 545.12f, 283.91f }, { 526.88f, 338.99f }, { 560.00f, 350.80f }, { 547.91f, 429.63f },
          { 545.52f, 349.91f }, { 579.83f, 342.23f }, { 568.45f, 285.34f } },
        { { 568.45f, 285.34f }, { 575.20f, 342.96f }, { 610.21f, 339.67f }, { 632.57f, 416.22f },
          { 596.71f, 344.99f }, { 624.56f, 323.52f }, { 590.20f, 276.77f } },
    };

    for (const auto& fin : fins)
    {
        juce::Path path;
        path.startNewSubPath (fin.start.x, fin.start.y);
        path.cubicTo (fin.c1.x, fin.c1.y, fin.c2.x, fin.c2.y, fin.tip.x, fin.tip.y);
        path.cubicTo (fin.c3.x, fin.c3.y, fin.c4.x, fin.c4.y, fin.end.x, fin.end.y);
        path.closeSubPath();

        g.setColour (kText.withAlpha (0.05f * alpha));
        g.fillPath (path);
        g.setColour (kText.withAlpha (0.12f * alpha));
        g.strokePath (path, juce::PathStrokeType (1.0f));
    }
}

void HudView::drawFader (juce::Graphics& g, int index, int frame)
{
    auto& fader = faders[static_cast<size_t> (index)];
    const auto alpha = alphaFor (fader.appearFrame);

    if (alpha <= 0.0f)
        return;

    juce::Graphics::ScopedSaveState blockState (g);
    g.addTransform (matrixFor (blockFaders));

    const auto norm = juce::jlimit (0.0f, 1.0f, fader.displayNorm);
    const auto v = fader.end - fader.root;
    const auto length = v.getDistanceFromOrigin();

    if (length <= 1.0f)
        return;

    const auto u = v / length;
    const auto perp = juce::Point<float> (-u.y, u.x);
    const auto handle = fader.root + v * norm;
    const auto hoverPhase = fader.hovered
                                ? juce::jlimit (0.0f, 1.0f, static_cast<float> (frame - fader.hoverFrame) / 2.0f)
                                : 0.0f;

    // 刀片展开量：进入 11 帧展开，离开 7 帧收回（同一套几何反向播放）
    const auto enterAmount = juce::jlimit (0.0f, 1.0f,
                                           static_cast<float> (frame - fader.hoverFrame) / 11.0f);
    const auto exitAmount = 1.0f - juce::jlimit (0.0f, 1.0f,
                                                 static_cast<float> (frame - fader.hoverEndFrame) / 7.0f);
    const auto bladeAmount = fader.hovered ? enterAmount
                                           : (fader.hoverEndFrame >= fader.hoverFrame ? exitAmount : 0.0f);

    // 跟随光条的淡入淡出：hover 3 帧亮起，离开 6 帧熄灭
    const auto glowIn = juce::jlimit (0.0f, 1.0f, static_cast<float> (frame - fader.hoverFrame) / 3.0f);
    const auto glowOut = 1.0f - juce::jlimit (0.0f, 1.0f,
                                              static_cast<float> (frame - fader.hoverEndFrame) / 6.0f);
    const auto glowFade = (fader.hovered || fader.dragging)
                              ? glowIn
                              : (fader.hoverEndFrame >= fader.hoverFrame ? glowOut : 0.0f);

    drawFaderStatic (g, fader, alpha);

    // 光标→末端之间的 4 根动态刻度（hover 时手柄 40px 内的刻度聚光）
    for (int i = 0; i < 4; ++i)
    {
        const auto t = norm + (1.0f - norm) * static_cast<float> (i) / 3.0f;
        const auto p = fader.root + v * t;
        const auto near = fader.hovered && p.getDistanceFrom (handle) <= 40.0f;
        const auto a = (near ? 0.60f : 0.25f) * alpha;

        g.setColour (kText.withAlpha (a));
        g.drawLine (p.x + perp.x * (kBladeHalfRoot - 0.5f), p.y + perp.y * (kBladeHalfRoot - 0.5f),
                    p.x + perp.x * (kBladeHalfRoot + 5.0f), p.y + perp.y * (kBladeHalfRoot + 5.0f),
                    1.0f);
    }

    // hover / grab：跟随光条。位置用更慢的弹簧追手柄（glowNorm），
    // 所以推子移动时能看见它懒懒地跟上来；不 hover 就完全不出现。
    if (glowFade > 0.001f)
    {
        const auto gp = fader.root + v * juce::jlimit (0.0f, 1.0f, fader.glowNorm);

        juce::Path bar;
        bar.startNewSubPath (gp.x - perp.x * 19.0f, gp.y - perp.y * 19.0f);
        bar.lineTo (gp.x + perp.x * 19.0f, gp.y + perp.y * 19.0f);

        const auto barStroke = [&] (float width, float a)
        {
            g.setColour (kText.withAlpha (a * glowFade * alpha));
            g.strokePath (bar, juce::PathStrokeType (width, juce::PathStrokeType::curved,
                                                     juce::PathStrokeType::rounded));
        };

        barStroke (13.0f, 0.055f);
        barStroke (6.0f, 0.13f);
        barStroke (1.9f, 0.72f);
    }

    // hover / grab：刀带两侧亮边与刃芯提亮
    if (fader.hovered || fader.dragging)
    {
        const auto hoverA = fader.root + perp * kBladeHalfRoot;
        const auto hoverB = fader.root - perp * kBladeHalfRoot;

        g.setColour (kText.withAlpha (0.90f));
        g.drawLine (hoverA.x, hoverA.y, fader.end.x, fader.end.y, 1.0f);
        g.drawLine (hoverB.x, hoverB.y, fader.end.x, fader.end.y, 1.0f);

        g.setColour (kText.withAlpha (0.55f));
        g.drawLine (fader.root.x, fader.root.y, fader.end.x, fader.end.y, 1.0f);
    }

    // grab：手柄下方一条加粗白块
    if (fader.dragging)
    {
        const auto from = handle - u * 9.0f;
        const auto to = handle + u * 9.0f;
        g.setColour (kText);
        g.drawLine (from.x, from.y, to.x, to.y, 5.0f);
    }

    // 刀带（横跨轨道的手柄叶片）
    g.setColour (kText.withAlpha (0.90f * alpha));
    g.drawLine (handle.x - perp.x * 16.0f, handle.y - perp.y * 16.0f,
                handle.x + perp.x * 16.0f, handle.y + perp.y * 16.0f, 1.1f);

    // 手柄菱形：一条对角线沿轨道
    const auto r = fader.handleRadius;
    const auto pTop = handle - u * r;
    const auto pBottom = handle + u * r;
    const auto pLeft = handle - perp * r;
    const auto pRight = handle + perp * r;

    juce::Path diamond;
    diamond.startNewSubPath (pTop.x, pTop.y);
    diamond.lineTo (pRight.x, pRight.y);
    diamond.lineTo (pBottom.x, pBottom.y);
    diamond.lineTo (pLeft.x, pLeft.y);
    diamond.closeSubPath();

    if (fader.outlinedHandle)
    {
        g.setColour (kBackground);
        g.fillPath (diamond);
        g.setColour (kText.withAlpha (0.95f * alpha));
        g.strokePath (diamond, juce::PathStrokeType (1.4f));
        g.fillEllipse (juce::Rectangle<float> (handle.x - 1.6f, handle.y - 1.6f, 3.2f, 3.2f));
    }
    else
    {
        g.setColour (kText.withAlpha (alpha));
        g.fillPath (diamond);
    }

    // hover 二级：手柄向外侧展开的刀片扇
    // 扇向取背离扇心的一侧：扇心用五个根部的均值（固定），中点用轨道中点，
    // 与数值无关，所以数值拉到底也不会突然翻面。
    auto fanPerp = perp;
    {
        auto hub = juce::Point<float> (0.0f, 0.0f);

        for (const auto& f : faders)
            hub += f.root;

        hub /= static_cast<float> (faders.size());

        const auto outwardVector = (fader.root + v * 0.5f) - hub;

        if (fanPerp.x * outwardVector.x + fanPerp.y * outwardVector.y < 0.0f)
            fanPerp = -fanPerp;
    }

    if (bladeAmount > 0.0f)
    {
        // 只在推子的一侧展开：方向取背离扇心的一侧；美术层跟着手柄走
        const auto outward = std::atan2 (fanPerp.y, fanPerp.x);

        drawHoverBlades (g, handle, outward, bladeAmount, alpha, 1.0f);
    }

    // hover 角标 / grab 角标（hover 时在 2 帧内向外展开）
    if (fader.hovered || fader.dragging)
    {
        const auto distance = fader.dragging ? 16.0f : 9.0f + 5.0f * hoverPhase;
        const auto arm = fader.dragging ? 4.0f : 3.0f + 2.0f * hoverPhase;

        g.setColour (kText.withAlpha (0.9f));

        for (int su = -1; su <= 1; su += 2)
        {
            for (int sv = -1; sv <= 1; sv += 2)
            {
                const auto corner = handle + u * (distance * static_cast<float> (su))
                                           + perp * (distance * static_cast<float> (sv));
                const auto armU = corner - u * (arm * static_cast<float> (su));
                const auto armV = corner - perp * (arm * static_cast<float> (sv));

                g.drawLine (corner.x, corner.y, armU.x, armU.y, 1.0f);
                g.drawLine (corner.x, corner.y, armV.x, armV.y, 1.0f);
            }
        }
    }

    // 手柄 → 读数框右上角的引出线（hover 3 帧画完）
    // 所有推子统一连到右上角，不再按最近边取点，避免每个推子的线走向都不一样。
    const auto boxCorner = fader.valueBox.getTopRight();

    if (fader.hovered && ! fader.dragging)
    {
        const auto progress = juce::jlimit (0.0f, 1.0f,
                                            static_cast<float> (frame - fader.hoverFrame - 1) / 3.0f);
        const auto tip = handle + (boxCorner - handle) * progress;

        if (progress > 0.0f)
        {
            g.setColour (kText.withAlpha (0.55f));
            g.drawLine (handle.x, handle.y, tip.x, tip.y, 1.0f);

            if (progress >= 0.99f)
            {
                g.setColour (kText.withAlpha (0.85f));
                g.fillEllipse (juce::Rectangle<float> (tip.x - 1.6f, tip.y - 1.6f, 3.2f, 3.2f));
            }
        }
    }

    // grab：同一条线（2 帧画完）
    if (fader.dragging)
    {
        const auto progress = juce::jlimit (0.0f, 1.0f,
                                            static_cast<float> (frame - fader.grabFrame) / 2.0f);
        const auto tip = handle + (boxCorner - handle) * progress;

        g.setColour (kText.withAlpha (0.70f));
        g.drawLine (handle.x, handle.y, tip.x, tip.y, 1.0f);
        g.setColour (kText.withAlpha (0.85f));
        g.fillEllipse (juce::Rectangle<float> (tip.x - 1.6f, tip.y - 1.6f, 3.2f, 3.2f));
    }

    // ADJ 标签：hover / grab 时出现，release 时亮-灭-亮一次后消失。
    // 挂在读数框右上角（引出线的落点），不压刀片。
    const auto rel = frame - fader.releaseFrame;
    const auto tagVisible = fader.hovered || fader.dragging
                            || (rel >= 0 && rel <= 4 && (rel == 0 || rel >= 3));

    if (tagVisible)
    {
        // 标签挂在读数框右侧、与框垂直居中：避开刀片，也不压控件名
        auto anchor = juce::Point<float> (fader.valueBox.getRight() + 23.0f,
                                          fader.valueBox.getCentreY() + 6.5f);

        anchor.x = juce::jlimit (40.0f, 920.0f, anchor.x);
        anchor.y = juce::jlimit (74.0f, 505.0f, anchor.y);

        drawAdjustedTag (g, anchor, true);
    }

    // 读数
    if (fader.hovered || fader.dragging)
    {
        g.setColour (kText.withAlpha (0.80f));
        g.drawRect (fader.valueBox.expanded (1.0f), 1.0f);
    }

    g.setColour (kBackground.withAlpha (alpha));
    g.setFont (hudFont (10.5f, 0.06f));
    g.drawText (faderValueText (fader.denormValue, fader.decimals), fader.valueBox,
                juce::Justification::centred, false);
}

// -----------------------------------------------------------------------
// 布局编辑层
// -----------------------------------------------------------------------

void HudView::drawLayoutOverlay (juce::Graphics& g, int frame)
{
    for (int block = 0; block < blockCount; ++block)
    {
        const auto& t = blockTransform[static_cast<size_t> (block)];
        const auto bounds = blockDesignBounds (block);
        const bool selected = (block == layoutSelected);
        const auto pulse = selected ? 0.72f + 0.18f * std::sin (static_cast<float> (frame) * 0.18f)
                                    : 0.35f;

        g.setColour (kText.withAlpha (pulse));
        g.drawRect (bounds, selected ? 1.6f : 1.0f);

        if (selected)
        {
            const auto corners = { bounds.getTopLeft(), bounds.getTopRight(),
                                   bounds.getBottomRight(), bounds.getBottomLeft() };

            for (const auto& corner : corners)
                g.fillRect (juce::Rectangle<float> (corner.x - 2.0f, corner.y - 2.0f, 4.0f, 4.0f));
        }

        const auto title = juce::String (kBlockNames[block])
                           + "   x" + juce::String (t.scale, 2)
                           + "   " + juce::String (juce::roundToInt (t.rotation)) + " deg";

        drawTextAtBaseline (g, title, bounds.getX() + 2.0f, bounds.getY() - 5.0f,
                            hudFont (7.0f, 0.10f), kText.withAlpha (selected ? 0.95f : 0.55f));
    }

    const auto hint = "LAYOUT :: DRAG MOVE   WHEEL SCALE   SHIFT+WHEEL ROTATE   ARROWS NUDGE   R RESET   ESC EXIT";
    drawTextAtBaseline (g, hint, 24.0f, 492.0f, hudFont (7.0f, 0.08f), kDim.withAlpha (0.9f));

    // 选中块的百分比提示（版式目标 6:3:1）
    static const char* const shares[] = { "60%", "30%", "10%", "DATA", "DATA" };
    const auto share = juce::String (kBlockNames[layoutSelected]) + "  " + shares[layoutSelected];
    const auto shareFont = hudFont (8.0f, 0.12f);
    drawTextAtBaseline (g, share, 936.0f - textWidth (shareFont, share), 492.0f,
                        shareFont, kText.withAlpha (0.95f));
}
