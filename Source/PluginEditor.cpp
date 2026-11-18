#include "PluginEditor.h"

#include "params/ParameterIDs.h"
#include "params/ParameterSchema.h"
#include "ui/UiText.h"
#include "ui/WindowShaper.h"

namespace
{
    constexpr int kPresetRowHeight = 26;
    constexpr int kHudWidth  = 960;
    constexpr int kHudHeight = 520;
    const juce::Colour kHudBackground (0xff0b0b0d);

    /** 旋钮的顺序与 sliderControls 一致；参数元数据统一来自 params/ParameterSchema.h。 */
    static_assert (params::kNumFloatSpecs == 7,
                   "ParameterSchema 的连续参数条目数必须与 OnlyObsessionRemainedEditor::kNumKnobs 一致");

    juce::String knobLabelId (int index)
    {
        return juce::String (params::floatSpec (index).skinId) + "Label";
    }

    /** 皮肤元素 id 打在控件属性上，LookAndFeel 靠它去取贴图。 */
    void setSkinId (juce::Component& component, const juce::String& id)
    {
        component.getProperties().set ("skinId", id);
    }

    juce::Justification parseJustification (const juce::String& text, juce::Justification fallback)
    {
        if (text.startsWithIgnoreCase ("l"))  return juce::Justification::centredLeft;
        if (text.startsWithIgnoreCase ("r"))  return juce::Justification::centredRight;
        if (text.startsWithIgnoreCase ("c"))  return juce::Justification::centred;

        return fallback;
    }
}

OnlyObsessionRemainedEditor::OnlyObsessionRemainedEditor (OnlyObsessionRemainedProcessor& p)
    : juce::AudioProcessorEditor (&p), processor (p)
{
    // 皮肤 LookAndFeel 挂在编辑器上（子控件全部继承），换皮肤时刷新颜色/字体
    // 旧皮肤路径已停用：不再挂 LookAndFeel，也不再创建隐藏的参数控件与附件
    if constexpr (kLegacySkinUiEnabled)
        setLookAndFeel (&lookAndFeel);

    // 让编辑器本身能拿到键盘焦点：Ctrl+Shift+* 系列快捷键（皮肤/设计器）才随时可用
    setWantsKeyboardFocus (true);

    addAndMakeVisible (titleLabel);

    addAndMakeVisible (typeLabel);

    typeBox.addItemList ({ "Plate", "Room", "Spring" }, 1);
    addAndMakeVisible (typeBox);

    if constexpr (kLegacySkinUiEnabled)
        typeAttachment = std::make_unique<juce::AudioProcessorValueTreeState::ComboBoxAttachment> (
            processor.apvts, params::type, typeBox);

    // ---- 预设条 ----
    presetBox.setTextWhenNothingSelected (U8 ("选择预设"));
    presetBox.onChange = [this]
    {
        const int index = presetBox.getSelectedId() - 1;

        if (index >= 0 && index != processor.presetManager.getCurrentIndex())
            processor.presetManager.loadPreset (index);
    };
    addAndMakeVisible (presetBox);

    addAndMakeVisible (prevButton);
    addAndMakeVisible (nextButton);
    addAndMakeVisible (saveButton);
    addAndMakeVisible (deleteButton);

    presetNameEditor.onReturnKey = [this] { saveButton.triggerClick(); };
    addAndMakeVisible (presetNameEditor);

    prevButton.onClick = [this]
    {
        selectPreset (processor.presetManager.getCurrentIndex() - 1, true);
    };

    nextButton.onClick = [this]
    {
        selectPreset (processor.presetManager.getCurrentIndex() + 1, true);
    };

    saveButton.onClick = [this]
    {
        const auto name = presetNameEditor.getText().trim();
        juce::String error;

        if (! processor.presetManager.saveUserPreset (name, error))
        {
            juce::AlertWindow::showAsync (
                juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::WarningIcon)
                    .withTitle (U8 ("保存预设失败"))
                    .withMessage (error)
                    .withButton (U8 ("好")),
                nullptr);
            return;
        }

        presetNameEditor.clear();

        // 用户预设按名称排序，新存的未必在末尾，这里按文件名定位并选中它
        const auto savedName = juce::File::createLegalFileName (name);
        int found = processor.presetManager.getNumPresets() - 1;

        for (int i = PresetManager::getNumFactoryPresets();
             i < processor.presetManager.getNumPresets(); ++i)
        {
            if (processor.presetManager.getPresetName (i).equalsIgnoreCase (savedName))
            {
                found = i;
                break;
            }
        }

        selectPreset (found, false);
    };

    deleteButton.onClick = [this]
    {
        const int index = presetBox.getSelectedId() - 1;

        if (! processor.presetManager.isUserPreset (index))
        {
            juce::AlertWindow::showAsync (
                juce::MessageBoxOptions()
                    .withIconType (juce::MessageBoxIconType::InfoIcon)
                    .withTitle (U8 ("删除预设"))
                    .withMessage (U8 ("出厂预设不可删除，只能删除你自己保存的预设。"))
                    .withButton (U8 ("好")),
                nullptr);
            return;
        }

        const auto name = processor.presetManager.getPresetName (index);

        juce::AlertWindow::showAsync (
            juce::MessageBoxOptions()
                .withIconType (juce::MessageBoxIconType::QuestionIcon)
                .withTitle (U8 ("删除预设"))
                .withMessage (U8 ("确定删除用户预设 \"") + name + U8 ("\" 吗？"))
                .withButton (U8 ("删除"))
                .withButton (U8 ("取消")),
            [this, index] (int result)
            {
                if (result != 1)   // 1 = 第一个按钮（删除）
                    return;

                juce::String error;

                if (processor.presetManager.deleteUserPreset (index, error))
                    selectPreset (juce::jmin (index, processor.presetManager.getNumPresets() - 1), false);
                else
                    juce::AlertWindow::showAsync (
                        juce::MessageBoxOptions()
                            .withIconType (juce::MessageBoxIconType::WarningIcon)
                            .withTitle (U8 ("删除预设失败"))
                            .withMessage (error)
                            .withButton (U8 ("好")),
                        nullptr);
            });
    };

    for (int i = 0; i < kNumKnobs; ++i)
        addSliderControl (params::floatSpec (i).id, params::floatSpec (i).skinId,
                          params::floatSpec (i).displayName,
                          sliderControls[static_cast<size_t> (i)]);

    processor.presetManager.addChangeListener (this);
    skin::SkinBundle::getInstance().addChangeListener (this);

#if OOR_ENABLE_SKIN_DESIGNER
    // ---- 皮肤设计器的编辑层（默认隐藏，Ctrl+Shift+E 打开）----
    designerOverlay.onSelectionChanged = [this] (const juce::String& id)
    {
        if (designerWindow != nullptr)
            designerWindow->selectElement (id);
    };

    // 拖动过程只改画面（不写文件）：编辑层直接改控件 bounds，松手才落盘
    designerOverlay.onRectPreview = [this] (const juce::String& id, juce::Rectangle<int> rect)
    {
        if (auto* component = componentForElement (id))
        {
            component->setBounds (rect);

            // 图层渲染层是控件的子组件：控件尺寸变了要同步，否则图层会停在旧尺寸上
            if (const auto it = controlOverlays.find (id); it != controlOverlays.end())
                if (it->second != nullptr)
                    it->second->setBounds (component->getLocalBounds());
        }

        // 锚点是相对矩形算的：矩形一改就得重算变换，否则拖动时控件会绕着旧锚点转
        setElementTransform (id, skin::SkinBundle::getInstance().getCurrent().elementTransformFor (id));
    };

    designerOverlay.onRectChanged = [this] (const juce::String& id, juce::Rectangle<int> rect)
    {
        // 编辑层给的是编辑器本地坐标；清单里存的是设计坐标（3e 的出血偏移在这里减掉）
        const auto bleed = bleedOffset();

        designerDocument.setRect (id, rect.translated (-bleed.x, -bleed.y));

        juce::String error;

        if (designerDocument.writeThrough (error))
        {
            refreshDesignerItems();

            // 面板上的 X/Y/宽/高 滑块要跟着拖拽结果走，否则画布动了、数字还是旧的。
            if (designerWindow != nullptr)
                designerWindow->refreshAll();
        }
        else
        {
            showStatus (U8 ("保存失败：") + error, true);
        }
    };

    // ---- 装饰图层 ----
    designerOverlay.onLayerSelected = [this] (const juce::String& id)
    {
        if (designerWindow != nullptr)
            designerWindow->selectLayer (id);
    };

    // 拖动过程只改画面：临时矩形记在编辑器里，paint() 优先使用
    designerOverlay.onLayerRectPreview = [this] (const juce::String& id, juce::Rectangle<int> rect)
    {
        layerPreviewRects[id] = rect;
        repaint();
    };

    designerOverlay.onLayerRectChanged = [this] (const juce::String& id, juce::Rectangle<int> rect)
    {
        layerPreviewRects.erase (id);

        // 同上：编辑层给本地坐标，清单存设计坐标
        const auto bleed = bleedOffset();
        const auto designRect = rect.translated (-bleed.x, -bleed.y);

        designerDocument.setLayerValue (id, "rect",
                                        juce::var (juce::Array<juce::var> { designRect.getX(), designRect.getY(),
                                                                            designRect.getWidth(), designRect.getHeight() }));

        juce::String error;

        if (designerDocument.writeThrough (error))
        {
            refreshDesignerItems();

            // 同上：画布上拖完虚线框，面板里的 X/Y/宽/高 必须立刻同步。
            if (designerWindow != nullptr)
                designerWindow->refreshAll();
        }
        else
        {
            showStatus (U8 ("保存失败：") + error, true);
        }
    };

    // 轨迹控制点在编辑层里拖动：松手才写盘（拖动过程只改编辑层里的曲线）
    designerOverlay.onTravelPointChanged = [this] (const juce::String& id, const juce::String& part,
                                                   int index, juce::Point<float> point)
    {
        // 拖动即启用：编辑层拖动时把曲线显示为实线，写盘也必须同步启用，
        // 否则会出现"设计器里是这条线、运行时却不走这条线"的不一致。
        designerDocument.setTravelEnabled (id, part, true);
        designerDocument.setTravelPoint (id, part, index, point);

        juce::String error;

        if (! designerDocument.writeThrough (error))
        {
            showStatus (U8 ("保存失败：") + error, true);
            return;
        }

        refreshDesignerItems();

        if (designerWindow != nullptr)
            designerWindow->refreshAll();
    };

    // ---- 控件图层（elements[].layers[]）----
    designerOverlay.onControlLayerSelected = [this] (const juce::String& ownerId,
                                                     const juce::String& layerId)
    {
        if (designerWindow != nullptr)
            designerWindow->selectControlLayer (ownerId, layerId);
    };

    // 拖动过程只改预览：图层渲染层读的是归一化矩形，松手才落盘
    designerOverlay.onControlLayerRectPreview = [this] (const juce::String& ownerId,
                                                        const juce::String& layerId,
                                                        juce::Rectangle<float> normalized)
    {
        if (const auto it = controlOverlays.find (ownerId); it != controlOverlays.end())
            if (it->second != nullptr)
                it->second->setPreview (layerId, normalized);
    };

    designerOverlay.onControlLayerRectChanged = [this] (const juce::String& ownerId,
                                                        const juce::String& layerId,
                                                        juce::Rectangle<float> normalized)
    {
        if (const auto it = controlOverlays.find (ownerId); it != controlOverlays.end())
            if (it->second != nullptr)
                it->second->clearPreview();

        designerDocument.setControlLayerRect (ownerId, layerId, normalized);

        juce::String error;

        if (! designerDocument.writeThrough (error))
        {
            showStatus (U8 ("保存失败：") + error, true);
            return;
        }

        refreshDesignerItems();

        // 面板里的 X/Y/宽/高 与归一化矩形同源：拖完必须立刻同步数字
        if (designerWindow != nullptr)
            designerWindow->refreshAll();
    };

    addChildComponent (designerOverlay);

    // 3e：编辑层会把"没命中任何条目"的点击吃掉，异形窗口的无标题栏拖动与右键菜单
    // 正是落在这些空白处 —— 让它把这类事件转交回来
    designerOverlay.onEmptyAreaPressed = [this] (const juce::MouseEvent& e)
    {
        beginWindowMouseInteraction (e);
    };

    designerOverlay.onEmptyAreaDragged = [this] (const juce::MouseEvent& e)
    {
        continueWindowDrag (e);
    };

    // 开发用：<用户配置>/OnlyObsessionRemained/designer.on 存在时启动即打开设计器。
    // 用途是让"改动→编译→截图核对"这条链路可以脚本化，不必盲发快捷键。
    if constexpr (kLegacySkinUiEnabled)
    {
        if (designerAvailable()
            && juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("OnlyObsessionRemained").getChildFile ("designer.on").existsAsFile())
        {
            juce::Component::SafePointer<OnlyObsessionRemainedEditor> safeThis (this);

            juce::MessageManager::callAsync ([safeThis]
            {
                if (auto* editor = safeThis.getComponent())
                    editor->toggleDesigner();
            });
        }
    }
#endif

    if constexpr (kLegacySkinUiEnabled)
        rebuildPresetList();

    // M1 定稿 HUD：作为最上层子组件覆盖整窗；旧皮肤控件只保留逻辑，不再显示。
    hudView = std::make_unique<HudView> (processor);
    addAndMakeVisible (*hudView);

    // ---- 动态着色器背景（阶段三 3c）----
    // HUD 模式：背景着色器由 HudView 自己持有（源码内嵌，见 HudSceneShader.h）。
    // 编辑器不再挂第二个 GL 上下文——原 `if (hudView == nullptr)` 永远为假，是死条件。
    // 旧皮肤着色器通路（skinHasShader / advanceFrame）仍保留在代码里，但不会启动。

    applySkin();

    for (int i = 0; i < getNumChildComponents(); ++i)
        if (auto* child = getChildComponent (i); child != hudView.get())
            child->setVisible (false);

    // 启动时若外部皮肤有问题，把原因说出来（否则用户只会看到"外观没变"）
    if constexpr (kLegacySkinUiEnabled)
    {
        const auto bundleStatus = skin::SkinBundle::getInstance().getStatus();

        if (bundleStatus.isNotEmpty())
            showStatus (bundleStatus, bundleStatus.contains (U8 ("失败")));
    }
}

OnlyObsessionRemainedEditor::~OnlyObsessionRemainedEditor()
{
    processor.presetManager.removeChangeListener (this);
    skin::SkinBundle::getInstance().removeChangeListener (this);

    setLookAndFeel (nullptr);
}

void OnlyObsessionRemainedEditor::addSliderControl (const juce::String& paramID, const juce::String& skinID,
                                       const juce::String& displayName, SliderControl& control)
{
    // 旧皮肤路径已停用：不创建隐藏控件，也不挂 SliderAttachment
    if constexpr (! kLegacySkinUiEnabled)
    {
        juce::ignoreUnused (paramID, skinID, displayName, control);
        return;
    }

    setSkinId (control.label, skinID + "Label");
    setSkinId (control.slider, skinID);

    control.label.setText (displayName, juce::dontSendNotification);
    addAndMakeVisible (control.label);

    // 数值框：JUCE 在"创建文本框"的那一刻就把当时的颜色烤进内部 Label
    // （见 LookAndFeel_V2::createSliderTextBox），所以必须在 setTextBoxStyle 之前
    // 把底色/边框压成透明——否则 V4 默认的灰边框会烤进 Label、皮肤再也接管不了它。
    // 数值框的真正外观由 SkinnedLookAndFeel::drawLabel 按 defaults.valueBox 绘制。
    control.slider.setColour (juce::Slider::textBoxBackgroundColourId, juce::Colours::transparentBlack);
    control.slider.setColour (juce::Slider::textBoxOutlineColourId, juce::Colours::transparentBlack);

    addAndMakeVisible (control.slider);

    control.attachment = std::make_unique<juce::AudioProcessorValueTreeState::SliderAttachment> (
        processor.apvts, paramID, control.slider);

    applySliderSpec (paramID, skinID, control);
}

void OnlyObsessionRemainedEditor::applySliderSpec (const juce::String& paramID, const juce::String& skinID,
                                      SliderControl& control)
{
    const auto& s = skin::SkinBundle::getInstance().getCurrent();

    auto param = [&s, &skinID] (const char* key, const juce::var& fallback)
    {
        return s.paramFor (skinID, key, fallback);
    };

    auto* slider = &control.slider;

    // ---- 形态：旋钮 / 横推子 / 竖推子 / 条形 ----
    const auto style = param ("style", juce::var ("rotary")).toString().trim().toLowerCase();

    if      (style == "vfader" || style == "vertical")   slider->setSliderStyle (juce::Slider::LinearVertical);
    else if (style == "hfader" || style == "horizontal") slider->setSliderStyle (juce::Slider::LinearHorizontal);
    else if (style == "vbar")                            slider->setSliderStyle (juce::Slider::LinearBarVertical);
    else if (style == "hbar")                            slider->setSliderStyle (juce::Slider::LinearBar);
    else                                                 slider->setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);

    // ---- 旋钮角度映射（度，0° = 正上方，顺时针为正）----
    auto degreesToRadians = [] (const juce::var& value, double fallbackDegrees)
    {
        const auto degrees = value.isVoid() ? fallbackDegrees : value.toString().getDoubleValue();
        return static_cast<float> (degrees * juce::MathConstants<double>::pi / 180.0);
    };

    slider->setRotaryParameters (degreesToRadians (param ("angleStart", juce::var()), 216.0),
                                 degreesToRadians (param ("angleEnd",   juce::var()), 504.0),
                                 true);

    // ---- 拖动逻辑 ----
    // auto：跟随形态（旋钮用旋转拖动，推子用相对拖动）；absolute：绝对定位（点哪跳哪）；
    // vertical / horizontal：相对拖动，灵敏度 = 拖满量程需要的像素数。
    const auto dragMode  = param ("dragMode", juce::var ("auto")).toString().trim().toLowerCase();
    const auto sensitivity = juce::jmax (10, static_cast<int> (param ("sensitivity", juce::var (200))
                                                                   .toString().getIntValue()));

    if (dragMode == "absolute")
    {
        slider->setSliderSnapsToMousePosition (true);
    }
    else if (dragMode == "vertical" || dragMode == "horizontal")
    {
        slider->setSliderSnapsToMousePosition (false);
        slider->setMouseDragSensitivity (sensitivity);
    }

    slider->setScrollWheelEnabled (static_cast<bool> (param ("scrollEnabled", juce::var (true))));

    // 双击复位到参数默认值（默认关闭，与改动前的行为一致）
    if (static_cast<bool> (param ("doubleClickReset", juce::var (false))))
        if (auto* p = processor.apvts.getParameter (paramID))
            slider->setDoubleClickReturnValue (true, p->convertFrom0to1 (p->getDefaultValue()));

    // ---- 数值框方位与尺寸 ----
    const auto position = param ("valueBox", juce::var ("below")).toString().trim().toLowerCase();

    const auto boxWidth  = static_cast<int> (param ("valueBoxWidth",  juce::var (72)).toString().getIntValue());
    const auto boxHeight = static_cast<int> (param ("valueBoxHeight", juce::var (18)).toString().getIntValue());

    if (position == "none" || boxHeight <= 0)
    {
        slider->setTextBoxStyle (juce::Slider::NoTextBox, false, 0, 0);
    }
    else
    {
        const auto textBoxPosition
            = position == "above" ? juce::Slider::TextBoxAbove
            : position == "left"  ? juce::Slider::TextBoxLeft
            : position == "right" ? juce::Slider::TextBoxRight
                                  : juce::Slider::TextBoxBelow;

        slider->setTextBoxStyle (textBoxPosition, false, boxWidth, boxHeight);
    }
}

// =======================================================================
// 皮肤应用
// =======================================================================

void OnlyObsessionRemainedEditor::applySkin()
{
    if constexpr (! kLegacySkinUiEnabled)
    {
        // 旧皮肤 UI 已停用：只保证 HUD 的固定窗口尺寸；
        // LookAndFeel / 贴图 / 控件 / 设计器 / 皮肤着色器全部不参与。
        if (getWidth() != kHudWidth || getHeight() != kHudHeight)
            setSize (kHudWidth, kHudHeight);

        return;
    }

    const auto& s = skin::SkinBundle::getInstance().getCurrent();

    lookAndFeel.refreshFromSkin();

    // ---- 贴图身份 id ----
    setSkinId (titleLabel, "title");
    setSkinId (typeLabel, "typeLabel");
    setSkinId (typeBox, "type");
    setSkinId (presetBox, "preset");
    setSkinId (presetNameEditor, "presetName");
    setSkinId (prevButton, "presetPrev");
    setSkinId (nextButton, "presetNext");
    setSkinId (saveButton, "presetSave");
    setSkinId (deleteButton, "presetDelete");

    // ---- 文案（清单可改）----
    // 标题不再显示插件名（清单里 title 元素的 text 为空；这里的兜底也为空）
    titleLabel.setText (s.textFor ("title", {}), juce::dontSendNotification);
    typeLabel.setText (s.textFor ("typeLabel", "Type"), juce::dontSendNotification);

    prevButton  .setButtonText (s.textFor ("presetPrev",   "<"));
    nextButton  .setButtonText (s.textFor ("presetNext",   ">"));
    saveButton  .setButtonText (s.textFor ("presetSave",   "Save"));
    deleteButton.setButtonText (s.textFor ("presetDelete", "Del"));

    presetNameEditor.setTextToShowWhenEmpty (s.stringFor ("presetName", "placeholder", U8 ("新预设名")),
                                             s.disabledColour);

    // ---- 字体与对齐（清单没写就沿用当前默认）----
    titleLabel.setFont (s.fontFor ("title", s.titleFont));
    titleLabel.setJustificationType (parseJustification (s.stringFor ("title", "justification", {}),
                                                         juce::Justification::centredLeft));

    typeLabel.setFont (s.fontFor ("typeLabel", s.labelFont));
    typeLabel.setJustificationType (parseJustification (s.stringFor ("typeLabel", "justification", {}),
                                                        juce::Justification::centred));

    for (int i = 0; i < kNumKnobs; ++i)
    {
        auto& control = sliderControls[static_cast<size_t> (i)];

        control.label.setText (s.textFor (knobLabelId (i), params::floatSpec (i).displayName),
                               juce::dontSendNotification);
        control.label.setFont (s.fontFor (knobLabelId (i), s.labelFont));
        control.label.setJustificationType (parseJustification (s.stringFor (knobLabelId (i),
                                                                            "justification", {}),
                                                                juce::Justification::centred));

        // 形态与操作逻辑也要跟着皮肤重设：设计器里把旋钮改成推子、改行程/灵敏度，
        // 预览需立刻反映（这条路径同时保证换皮肤后不会残留上一份皮肤的交互设置）
        applySliderSpec (params::floatSpec (i).id, params::floatSpec (i).skinId, control);
    }

    // ---- 窗口尺寸与元素矩形（清单说了算）----
    // 尺寸没变时 setSize() 不会触发 resized()，但清单里的元素矩形/形态这时可能已经改了
    // （设计器每改一次都会重载皮肤）——那样控件会停在旧位置，看上去就是"改动失效"。
    // 所以尺寸没变时显式再走一遍布局。
    //
    // 3e：窗口 = 画布 + 四边出血。清单里的 rect 相对画布左上角，编辑器本地坐标再叠一层偏移
    // （偏移只在 applyLayout / paint / 设计器边界上加，见 bleedOffset()）。
    // HUD 是固定外观：窗口固定 960×520；皮肤的窗口尺寸/出血不再参与。
    if (getWidth() != kHudWidth || getHeight() != kHudHeight)
        setSize (kHudWidth, kHudHeight);
    else
        resized();

#if OOR_ENABLE_SKIN_DESIGNER
    if (designerOverlay.isVisible())
        refreshDesignerItems();   // 尺寸没变时 resized() 不会被调用，这里补一次
#endif

    // 皮肤可能换了着色器源码与帧率
    shaderBackground.loadFromSkin();
    restartFrameClock();

    // 3e：窗口外形（无标题栏 / 区域裁剪）跟着这份皮肤走
    applyWindowFrame();
    applyShapeRegion();

    repaint();
}

OnlyObsessionRemainedEditor::Layout OnlyObsessionRemainedEditor::computeDefaultLayout (int canvasWidth, int canvasHeight) const
{
    Layout layout;

    auto area = juce::Rectangle<int> (0, 0, canvasWidth, canvasHeight).reduced (16);

    layout.title = area.removeFromTop (24);
    area.removeFromTop (6);

    auto presetRow = area.removeFromTop (kPresetRowHeight);

    layout.presetPrev = presetRow.removeFromLeft (26);
    presetRow.removeFromLeft (2);
    layout.presetNext = presetRow.removeFromLeft (26);
    presetRow.removeFromLeft (10);

    layout.presetDelete = presetRow.removeFromRight (46);
    presetRow.removeFromRight (4);
    layout.presetSave   = presetRow.removeFromRight (56);
    presetRow.removeFromRight (4);
    layout.presetName   = presetRow.removeFromRight (130);
    presetRow.removeFromRight (10);

    layout.preset = presetRow;

    area.removeFromTop (10);

    constexpr int columns = 4;
    constexpr int rows    = 2;

    const int cellWidth  = area.getWidth()  / columns;
    const int cellHeight = area.getHeight() / rows;

    auto cellAt = [&] (int index)
    {
        const int col = index % columns;
        const int row = index / columns;

        return juce::Rectangle<int> (area.getX() + col * cellWidth,
                                     area.getY() + row * cellHeight,
                                     cellWidth, cellHeight).reduced (8);
    };

    // 0 号格子：Type 选择器
    {
        auto cell = cellAt (0);

        layout.typeLabel = cell.removeFromTop (18);
        layout.type = cell.withSizeKeepingCentre (juce::jmax (80, cell.getWidth() - 24), 28);
    }

    // 1..N 号格子：旋钮（标签在上，旋钮占其余）
    for (int i = 0; i < kNumKnobs; ++i)
    {
        auto cell = cellAt (i + 1);

        layout.knobLabel[i] = cell.removeFromTop (18);
        layout.knob[i]      = cell;
    }

    return layout;
}

void OnlyObsessionRemainedEditor::applyLayout (const Layout& layout)
{
    // 3e：这里是**唯一**一处把设计坐标（相对画布）换成编辑器本地坐标的地方。
    // 出血不为 0 时，整张设计在窗口里整体平移，相对关系完全不变。
    const auto offset = bleedOffset();

    auto place = [offset] (juce::Component& component, juce::Rectangle<int> bounds)
    {
        component.setBounds (bounds.translated (offset.x, offset.y));
    };

    place (titleLabel,       layout.title);
    place (prevButton,       layout.presetPrev);
    place (nextButton,       layout.presetNext);
    place (presetBox,        layout.preset);
    place (presetNameEditor, layout.presetName);
    place (saveButton,       layout.presetSave);
    place (deleteButton,     layout.presetDelete);

    place (typeLabel,        layout.typeLabel);
    place (typeBox,          layout.type);

    for (int i = 0; i < kNumKnobs; ++i)
    {
        place (sliderControls[static_cast<size_t> (i)].label,  layout.knobLabel[i]);
        place (sliderControls[static_cast<size_t> (i)].slider, layout.knob[i]);
    }

    // 变换必须排在 setBounds 之后：锚点是相对矩形算的，矩形换了锚点也要跟着换
    refreshElementTransforms();
}

juce::Point<int> OnlyObsessionRemainedEditor::bleedOffset() const
{
    const auto& s = skin::SkinBundle::getInstance().getCurrent();

    return { s.bleedLeft, s.bleedTop };
}

void OnlyObsessionRemainedEditor::refreshElementTransforms()
{
    // 先复位：换到一份"没写变换"的皮肤时，上一份皮肤留下的旋转/缩放必须清掉，
    // 否则控件会带着旧皮肤的弯度停在原地。
    const std::initializer_list<juce::Component*> fixedControls
        { &titleLabel, &prevButton, &nextButton, &presetBox, &presetNameEditor,
          &saveButton, &deleteButton, &typeLabel, &typeBox };

    for (auto* component : fixedControls)
        component->setTransform (juce::AffineTransform());

    for (auto& control : sliderControls)
    {
        control.label .setTransform (juce::AffineTransform());
        control.slider.setTransform (juce::AffineTransform());
    }

    const auto& s = skin::SkinBundle::getInstance().getCurrent();

    // 只遍历清单里声明过的元素：没声明的元素连类型都没有，本来也取不到类型默认变换
    for (const auto& element : s.elements)
        setElementTransform (element.id, s.elementTransformFor (element.id));
}

void OnlyObsessionRemainedEditor::setElementTransform (const juce::String& id,
                                                       const skin::Transform& transform)
{
    auto* component = componentForElement (id);

    if (component == nullptr)
        return;

    // scale 为 0 的变换没有逆矩阵，JUCE 的绘制与命中判定都会走样 —— 这种情况当"没有变换"
    if (transform.isIdentity() || transform.scale.x == 0.0f || transform.scale.y == 0.0f)
    {
        component->setTransform (juce::AffineTransform());
        return;
    }

    // Component::setTransform 的变换是"相对父组件左上角原点"的，所以锚点要用父坐标里的像素。
    // 取控件当前的**未变换**矩形来算，于是设计器拖动改矩形时锚点也自动跟着走。
    component->setTransform (transform.toAffine (component->getBounds().toFloat()));
}

void OnlyObsessionRemainedEditor::resized()
{
    const auto& s = skin::SkinBundle::getInstance().getCurrent();

    auto layout = computeDefaultLayout (s.canvasWidth, s.canvasHeight);

    layout.title        = s.rectFor ("title",        layout.title);
    layout.presetPrev   = s.rectFor ("presetPrev",   layout.presetPrev);
    layout.presetNext   = s.rectFor ("presetNext",   layout.presetNext);
    layout.preset       = s.rectFor ("preset",       layout.preset);
    layout.presetName   = s.rectFor ("presetName",   layout.presetName);
    layout.presetSave   = s.rectFor ("presetSave",   layout.presetSave);
    layout.presetDelete = s.rectFor ("presetDelete", layout.presetDelete);

    layout.typeLabel = s.rectFor ("typeLabel", layout.typeLabel);
    layout.type      = s.rectFor ("type",      layout.type);

    for (int i = 0; i < kNumKnobs; ++i)
    {
        layout.knob[i]      = s.rectFor (params::floatSpec (i).skinId, layout.knob[i]);
        layout.knobLabel[i] = s.rectFor (knobLabelId (i), layout.knobLabel[i]);
    }

    applyLayout (layout);

    currentLayout = layout;   // 设计器与编辑层都读这份结果（**设计坐标**，不含出血偏移）

    // 控件图层要跟随控件尺寸：布局落定后统一同步一次
    refreshControlLayerOverlays();

#if OOR_ENABLE_SKIN_DESIGNER
    designerOverlay.setBounds (getLocalBounds());
#endif

    if (hudView != nullptr)
        hudView->setBounds (getLocalBounds());

#if OOR_ENABLE_SKIN_DESIGNER
    // 3e：编辑层要知道画布在窗口里的位置（画出出血区与画布边框），以及形状蒙版（画轮廓）
    designerOverlay.setCanvasInfo (s.canvasRect(),
                                   shouldClipPainting() ? s.shapeMask() : juce::Image());

    if (designerOverlay.isVisible())
        refreshDesignerItems();
#endif

    // 宿主（或独立运行外壳）改过窗口尺寸后，之前设的区域是按旧尺寸算的，必须重算。
    // 签名缓存会挡掉"尺寸没变"的重复调用，所以这里每帧调也不心疼。
    applyShapeRegion();
}

void OnlyObsessionRemainedEditor::paint (juce::Graphics& g)
{
    // HUD 自己绘制全部内容（含背景）；编辑器只负责填底，避免着色器白烧。
    if (hudView != nullptr && hudView->isVisible())
    {
        g.fillAll (kHudBackground);
        return;
    }

    const auto& s = skin::SkinBundle::getInstance().getCurrent();

    // 3e 视觉异形：有蒙版、且 mode 不是 off 时，把**我们自己画的东西**裁到形状里。
    // 注意两点：
    //  1) 控件是子组件，在 paint() 之后由 JUCE 画，这里的裁剪管不到它们；
    //  2) GL 着色器通道不吃 Graphics 裁剪（它直接画在窗口表面上）。
    // 所以"形状外真的没有像素"要靠窗口区域裁剪（Standalone / VST3 的 mode: force），
    // 这一层裁剪是让矩形窗口下（VST3 auto）看起来也像那个形状。
    const juce::Graphics::ScopedSaveState saveState (g);

    if (shouldClipPainting())
    {
        if (const auto mask = s.shapeMask(); ! mask.isNull())
            g.reduceClipRegion (mask, juce::AffineTransform::scale (
                static_cast<float> (getWidth())  / static_cast<float> (mask.getWidth()),
                static_cast<float> (getHeight()) / static_cast<float> (mask.getHeight())));
    }

    // 背景：皮肤带了着色器、且用户没关掉时走着色器通路（GPU 不可用会自动降级到 CPU 绘制）；
    // 否则维持原来的一张底色 —— 即"没有着色器的皮肤，外观完全不变"。
    if (shaderBackground.skinHasShader() && shaderBackground.isEnabled())
        shaderBackground.paintBackground (g, getLocalBounds(), s.backgroundColour);
    else
        g.fillAll (s.backgroundColour);

    // 画布在窗口里的矩形：背景贴图与脸框都按**画布**铺，不能按窗口铺 ——
    // 否则非对称出血会把画面拉伸变形，脸框也会画到出血区里去
    const auto canvasArea = s.canvasRect();

    // 整幅背景贴图叠在着色器之上（静态装饰压在动态底上）
    if (s.backgroundImagePath.isNotEmpty())
        if (const auto image = s.imageFor (s.backgroundImagePath); ! image.isNull())
            g.drawImage (image, canvasArea.toFloat(), juce::RectanglePlacement::stretchToFit);

    // 装饰图层：按清单顺序叠在背景之上、控件之下（控件是子组件，在 paint() 之后才绘制）
    const auto offset = bleedOffset();

    for (const auto& layer : s.layers)
    {
        if (! layer.visible)
            continue;

        // 清单里的矩形是设计坐标；拖动预览用的是编辑器本地坐标（编辑层本来就活在本地系里）
        auto rect = layer.rect.translated (offset.x, offset.y);

        // 设计器拖动中的临时矩形优先（只改画面，松手才落盘）
        if (const auto it = layerPreviewRects.find (layer.id); it != layerPreviewRects.end())
            rect = it->second;

        if (! layer.hasRect || rect.isEmpty())
            continue;

        const auto image = s.imageFor (layer.imagePath);

        if (image.isNull())
            continue;

        const auto area = rect.toFloat();

        // 变换与透明度都要还原：否则后面画的东西会一直带着它们
        const juce::Graphics::ScopedSaveState save (g);

        if (! layer.transform.isIdentity())
            g.addTransform (layer.transform.toAffine (area));

        g.setOpacity (juce::jlimit (0.0f, 1.0f, layer.opacity));
        g.drawImage (image, area, layer.transform.placement (false));
    }

    // 脸框：围的是画布，不是窗口。HUD 风格 = 切角双层框（外粗框 + 暗缝 + 内细框）；
    // 清单里写了 cornerRadius 的旧皮肤保持圆角单框，只把颜色换成结构线
    if (s.backgroundOutlineThickness > 0.0f)
        SkinnedLookAndFeel::drawHudFaceFrame (g,
                                              canvasArea.toFloat()
                                                  .reduced (static_cast<float> (s.backgroundOutlineInset)),
                                              static_cast<float> (s.backgroundOutlineCorner),
                                              s.backgroundOutlineThickness);

    // 皮肤加载提示（几秒后自动消失）：贴着窗口底部，跟出血无关
    if (statusText.isNotEmpty())
    {
        const auto area = juce::Rectangle<int> (10, getHeight() - 26, getWidth() - 20, 20);

        g.setColour (s.textBoxColour.withAlpha (0.92f));
        g.fillRoundedRectangle (area.toFloat(), 4.0f);

        g.setColour (statusIsError ? juce::Colours::orange : s.accentColour);
        g.setFont (juce::FontOptions (12.0f));
        g.drawText (statusText, area.reduced (8, 0), juce::Justification::centredLeft, true);
    }
}

void OnlyObsessionRemainedEditor::mouseDown (const juce::MouseEvent& e)
{
    grabKeyboardFocus();
    beginWindowMouseInteraction (e);
}

void OnlyObsessionRemainedEditor::beginWindowMouseInteraction (const juce::MouseEvent& e)
{
    // 右键：无标题栏时标题栏上的入口（音频设置/退出）没了，这里补一份
    if (e.mods.isPopupMenu())
    {
        showWindowMenu();
        return;
    }

    // 无标题栏的窗口没有地方可抓，只能靠"没被控件吃掉的那些点击"来拖
    if (isWindowDragArea (e.getPosition()))
        windowDragger.startDraggingComponent (getTopLevelComponent(), e);
}

void OnlyObsessionRemainedEditor::continueWindowDrag (const juce::MouseEvent& e)
{
    if (isWindowDragArea (e.getMouseDownPosition()))
        windowDragger.dragComponent (getTopLevelComponent(), e, nullptr);
}

void OnlyObsessionRemainedEditor::mouseDrag (const juce::MouseEvent& e)
{
    continueWindowDrag (e);
}

void OnlyObsessionRemainedEditor::parentHierarchyChanged()
{
    // 被挂进顶层窗口（独立运行外壳）之后才知道真正的 host 是谁：这时才谈得上隐标题栏、裁窗口
    scheduleWindowFrameApply();
}

void OnlyObsessionRemainedEditor::visibilityChanged()
{
    // 窗口刚显示出来（peer 就绪）也是补施加一次的机会
    if (isVisible())
        scheduleWindowFrameApply();
}

void OnlyObsessionRemainedEditor::scheduleWindowFrameApply()
{
    if (frameApplyScheduled)
        return;

    frameApplyScheduled = true;

    juce::Component::SafePointer<OnlyObsessionRemainedEditor> safeThis (this);

    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis == nullptr)
            return;

        safeThis->frameApplyScheduled = false;
        safeThis->applyWindowFrame();
        safeThis->applyShapeRegion();
    });
}

void OnlyObsessionRemainedEditor::rebuildPresetList()
{
    const int previousId = presetBox.getSelectedId();

    presetBox.clear (juce::dontSendNotification);

    const int numFactory = PresetManager::getNumFactoryPresets();
    const int numTotal   = processor.presetManager.getNumPresets();

    presetBox.addSectionHeading (U8 ("出厂预设"));

    for (int i = 0; i < numFactory; ++i)
        presetBox.addItem (processor.presetManager.getPresetName (i), i + 1);

    if (numTotal > numFactory)
    {
        presetBox.addSectionHeading (U8 ("用户预设"));

        for (int i = numFactory; i < numTotal; ++i)
            presetBox.addItem (processor.presetManager.getPresetName (i), i + 1);
    }

    // 优先保留原选中项，否则跟随 PresetManager 的当前索引
    int idToSelect = previousId;

    if (idToSelect <= 0 || idToSelect > numTotal)
        idToSelect = processor.presetManager.getCurrentIndex() + 1;

    presetBox.setSelectedId (juce::jlimit (1, juce::jmax (1, numTotal), idToSelect),
                             juce::dontSendNotification);

    deleteButton.setEnabled (processor.presetManager.isUserPreset (presetBox.getSelectedId() - 1));
}

void OnlyObsessionRemainedEditor::selectPreset (int index, bool loadPreset)
{
    const int numTotal = processor.presetManager.getNumPresets();

    if (numTotal <= 0)
        return;

    index = (index % numTotal + numTotal) % numTotal;   // 环绕

    presetBox.setSelectedId (index + 1, juce::dontSendNotification);
    deleteButton.setEnabled (processor.presetManager.isUserPreset (index));

    if (loadPreset)
        processor.presetManager.loadPreset (index);
}

void OnlyObsessionRemainedEditor::changeListenerCallback (juce::ChangeBroadcaster* source)
{
    if (source == &skin::SkinBundle::getInstance())
    {
        applySkin();

        // 皮肤换了：设计器面板里的着色器编辑区也必须跟着换文件，
        // 否则用户会在新皮肤上"应用"上一套皮肤的源码。
#if OOR_ENABLE_SKIN_DESIGNER
        if (designerWindow != nullptr)
            designerWindow->onSkinChanged();
#endif

        return;
    }

    if constexpr (kLegacySkinUiEnabled)
        rebuildPresetList();
}

void OnlyObsessionRemainedEditor::timerCallback()
{
    stopTimer();
    statusText.clear();
    repaint();
}

void OnlyObsessionRemainedEditor::showStatus (const juce::String& text, bool isError)
{
    statusText   = text;
    statusIsError = isError;

    startTimer (4000);
    repaint();
}

// =======================================================================
// 外部皮肤快捷键（美术迭代用，见 docs/skin-guide.md）
// =======================================================================

bool OnlyObsessionRemainedEditor::keyPressed (const juce::KeyPress& key)
{
    const auto mods = key.getModifiers();

    if (! (mods.isCtrlDown() && mods.isShiftDown()))
        return false;

    switch (key.getKeyCode())
    {
        // 旧皮肤 / 设计器入口：HUD 为唯一界面时全部停用
        case 'O': if constexpr (kLegacySkinUiEnabled) { chooseSkinFolder(); } return true;
        case 'R': if constexpr (kLegacySkinUiEnabled) { reloadSkin(); }        return true;
        case 'E':
#if OOR_ENABLE_SKIN_DESIGNER
            if constexpr (kLegacySkinUiEnabled) { toggleDesigner(); }
#endif
            return true;
        case 'S': toggleShaderBackground(); return true;
        case 'A': openAudioSettings();      return true;   // 无标题栏的异形皮肤顶上标题栏入口

        case 'L':
            if (hudView != nullptr)
            {
                hudView->toggleLayoutEditor();
                showStatus (U8 (hudView->isLayoutEditorOpen()
                                    ? "版式编辑已开：拖拽移动 / 滚轮缩放 / Shift+滚轮旋转 / R 复位 / Esc 退出"
                                    : "版式编辑已关闭并保存"),
                            false);
            }
            return true;

        case 'D':
            skin::SkinBundle::getInstance().loadBuiltIn();
            showStatus (U8 ("已回到内置皮肤"), false);
            return true;

        default: break;
    }

    return false;
}

// =======================================================================
// 动态着色器背景（阶段三 3c）
// =======================================================================

void OnlyObsessionRemainedEditor::advanceFrame()
{
    // HUD 模式不使用皮肤着色器（界面由 HudView 全矢量绘制）。
    if (hudView != nullptr)
        return;

    if (! shaderBackground.skinHasShader() || ! shaderBackground.isEnabled())
        return;

    // 探测结束后若 GPU 没就绪，说明走的是 CPU 降级路径 —— 只提示一次
    if (shaderBackground.isProbeDone() && ! shaderBackground.isGpuActive()
        && ! shaderDegradeNotified && shaderBackground.skinHasShader())
    {
        shaderDegradeNotified = true;
        showStatus (U8 ("GPU 不可用，动态背景已降级为 CPU 绘制"), true);
    }

    // 着色器编译失败：保留上一个可用版本（界面照常），但要把原因说出来，
    // 否则用户只会看到"背景没变"，无从下手。
    if (const auto error = shaderBackground.getLastError();
        error.isNotEmpty() && error != shaderErrorNotified)
    {
        shaderErrorNotified = error;
        showStatus (U8 ("着色器编译失败，已保留上一个可用版本：") + error, true);
    }

    // 窗口不可见时不烧 CPU/GPU（见 ADR-0002）；回来时时间不跳
    if (! isShowing())
    {
        lastFrameTime = 0.0;
        return;
    }

    const double now = juce::Time::getMillisecondCounterHiRes() * 0.001;

    if (lastFrameTime > 0.0)
        shaderTime += static_cast<float> (now - lastFrameTime);

    lastFrameTime = now;

    // 参数一律走"引擎实际使用的那份归一化值"，界面里不再重复百分比换算
    const auto params = processor.getNormalizedParameters();

    ShaderBackground::FrameState state;
    state.time       = shaderTime;
    state.mix        = params.mix;          // 0…1
    state.decay      = params.decay;        // 0…1
    state.preDelayMs = params.preDelayMs;   // 毫秒（0…500）
    state.mod        = params.mod;          // 0…1
    state.highDamp   = params.highDamp;     // 0…1
    state.lowDamp    = params.lowDamp;      // 0…1
    state.shimmer    = params.shimmer;      // 0…1
    state.type       = static_cast<float> (params.type);   // 0 板式 / 1 房间 / 2 弹簧
    state.level      = processor.getWetOutputLevel();
    state.dark       = juce::Desktop::getInstance().isDarkModeActive() ? 1.0f : 0.0f;

    // 鼠标直接读全局指针再换算到本地：控件会把 mouseMove 吃掉，走事件不可靠。
    // 指针不在窗口内时保持上一次的值（着色器契约如此）。
    if (const auto local = getLocalPoint (nullptr,
                                          juce::Desktop::getInstance().getMainMouseSource().getScreenPosition());
        getLocalBounds().contains (local.toInt()))
        lastMousePosition = local;

    state.mouse = lastMousePosition;

    shaderBackground.setFrameState (state);
    repaint();
}

void OnlyObsessionRemainedEditor::restartFrameClock()
{
    frameClock.stopTimer();

    if (hudView != nullptr)
        return;

    if (! shaderBackground.skinHasShader() || ! shaderBackground.isEnabled())
        return;

    frameClock.startTimerHz (juce::jlimit (5, 120, shaderBackground.getFrameRate()));
}

void OnlyObsessionRemainedEditor::toggleShaderBackground()
{
    // HUD 模式：背景着色器由 HudView 自己持有（源码内嵌，不走皮肤清单）。
    if (hudView != nullptr)
    {
        hudView->setBackdropEnabled (! hudView->isBackdropEnabled());

        showStatus (hudView->isBackdropEnabled() ? U8 ("动态背景：开")
                                                 : U8 ("动态背景：关（已记住）"), false);
        return;
    }

    if (! shaderBackground.skinHasShader())
    {
        showStatus (U8 ("当前皮肤没有着色器（清单里的 shader.source）"), true);
        return;
    }

    shaderBackground.setUserEnabled (! shaderBackground.isEnabled());
    restartFrameClock();
    repaint();

    showStatus (shaderBackground.isEnabled() ? U8 ("动态着色器背景：开")
                                             : U8 ("动态着色器背景：关（已记住）"), false);
}

juce::String OnlyObsessionRemainedEditor::describeShaderStatus() const
{
    if (! shaderBackground.skinHasShader())
        return U8 ("这套皮肤没有着色器");

    if (! shaderBackground.isEnabled())
        return U8 ("已被用户关掉（Ctrl+Shift+S 打开）");

    if (! shaderBackground.isProbeDone())
        return U8 ("正在探测 GPU…");

    return shaderBackground.isGpuActive() ? U8 ("GPU 通路正常")
                                          : U8 ("GPU 不可用，已降级为 CPU 绘制");
}

void OnlyObsessionRemainedEditor::chooseSkinFolder()
{
    if constexpr (! kLegacySkinUiEnabled)
        return;   // 旧换肤入口已停用

    skinFolderChooser = std::make_unique<juce::FileChooser> (U8 ("选择皮肤目录（含 skin.json）"),
                                                             juce::File::getSpecialLocation (
                                                                 juce::File::userDocumentsDirectory),
                                                             juce::String());

    skinFolderChooser->launchAsync (juce::FileBrowserComponent::openMode
                                        | juce::FileBrowserComponent::canSelectDirectories,
                                    [this] (const juce::FileChooser& chooser)
    {
        const auto folder = chooser.getResult();

        if (! folder.isDirectory())
            return;

        juce::String error;

        if (skin::SkinBundle::getInstance().loadExternalFolder (folder, error))
            showStatus (U8 ("已加载外部皮肤：") + folder.getFileName(), false);
        else
            showStatus (U8 ("加载失败：") + error, true);
    });
}

void OnlyObsessionRemainedEditor::reloadSkin()
{
    if constexpr (kLegacySkinUiEnabled)
    {
        juce::String error;

        if (skin::SkinBundle::getInstance().reload (error))
            showStatus (U8 ("已重新加载皮肤"), false);
        else
            showStatus (error, true);
    }
}

// =======================================================================
// 异形窗口与画布出血（阶段三 3e）
// =======================================================================

bool OnlyObsessionRemainedEditor::isStandalone() const
{
    return processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone;
}

juce::Component* OnlyObsessionRemainedEditor::windowHostComponent()
{
    // Standalone：整块顶层窗口都是我们的（GL 着色器子窗口也挂在顶层下面，
    // 所以区域必须裁在顶层身上，才能把 GL 画出来的部分一起裁掉）。
    if (isStandalone())
        return getTopLevelComponent();

    // VST3：编辑器自己就是宿主给的那块 HWND
    return this;
}

bool OnlyObsessionRemainedEditor::shouldClipWindowRegion() const
{
    // HUD 固定为矩形窗口，皮肤的形状裁切不再参与。
    if (hudView != nullptr)
        return false;

    const auto& s = skin::SkinBundle::getInstance().getCurrent();

    if (s.shapeMode == "off" || ! s.hasShapeMask())
        return false;

    // Standalone：auto / force 都真裁 —— 形状外就是桌面，这是异形窗口最自然的形态
    if (isStandalone())
        return true;

    // VST3：auto 只做视觉（裁编辑器那块地会露出宿主背景，观感不一定好），
    // force 才真裁（文档里标为实验性）
    return s.shapeMode == "force";
}

bool OnlyObsessionRemainedEditor::shouldClipPainting() const
{
    if (hudView != nullptr)
        return false;

    const auto& s = skin::SkinBundle::getInstance().getCurrent();

    return s.hasShapeMask() && s.shapeMode != "off";
}

void OnlyObsessionRemainedEditor::applyShapeRegion()
{
    auto* host = windowHostComponent();

    if (host == nullptr || host->getPeer() == nullptr)
        return;   // 窗口还没建好：等下一次（resized / applySkin）再试

    const auto& s = skin::SkinBundle::getInstance().getCurrent();

    if (! shouldClipWindowRegion())
    {
        if (appliedShapeSignature.isNotEmpty() && appliedShapeSignature != "none")
            skin::window_shape::clearFromWindow (*host);

        appliedShapeSignature = "none";
        shapeStatus.clear();
        return;
    }

    const auto mask = s.shapeMask();

    if (mask.isNull())
    {
        shapeStatus = U8 ("蒙版读不出来：") + s.shapeMaskPath;
        return;
    }

    // 签名：目标窗口 / 蒙版 / 窗口尺寸 / 要不要裁 / 有没有隐标题栏，只要这些没变就不碰系统调用 ——
    // 设计器每改一次都会重载皮肤，每次都重设区域会让窗口闪一下
    const auto signature = juce::String (host == this ? "self" : "top") + "|" + s.shapeMaskPath
                         + "|" + juce::String (getWidth()) + "x" + juce::String (getHeight())
                         + "|" + s.shapeMode + "|" + (framelessApplied ? "bare" : "titled");

    if (signature == appliedShapeSignature)
        return;

    // 蒙版铺在**编辑器那块**上（编辑器尺寸 = 画布 + 出血，是我们能精确控制的量），
    // 而不是铺在"顶层窗口的 bounds"上：去掉标题栏会让外壳重新套一遍窗口尺寸，
    // 那一刻顶层组件的 bounds 还是旧的，按它算出来的形状会偏。
    // 编辑器上方那条（JUCE 标题栏 / 窗口边框）在还留着标题栏时并进区域，
    // 隐了标题栏就让它一起被裁掉 —— 那本来就只是边框，不是内容。
    juce::Rectangle<int> maskArea = host->getLocalArea (this, getLocalBounds());
    juce::Rectangle<int> extraArea;

    if (! framelessApplied && maskArea.getY() > 0)
        extraArea = host->getLocalBounds().withBottom (maskArea.getY());

    if (skin::window_shape::applyToWindow (*host, mask, maskArea, extraArea))
    {
        // 成功才记签名：失败（例如窗口还没就绪）时下次还要再试
        appliedShapeSignature = signature;
        shapeStatus.clear();
    }
    else
    {
        shapeStatus = skin::window_shape::lastError();
    }
}

void OnlyObsessionRemainedEditor::applyWindowFrame()
{
    // HUD 固定为标准标题栏窗口，皮肤的 frameless 不再参与。
    if (hudView != nullptr)
        return;

    // 只有独立运行的外壳才有标题栏可隐 —— 插件的窗口由宿主管
    auto* window = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent());
    const auto& s = skin::SkinBundle::getInstance().getCurrent();

    const bool wantFrameless = isStandalone() && s.frameless && window != nullptr;

    if (wantFrameless == framelessApplied)
        return;

    if (window == nullptr)
        return;

    if (wantFrameless)
    {
        savedTitleBarHeight = window->getTitleBarHeight();

        // 标题栏是本插件自己画的（StandaloneFilterWindow 把 Options 按钮放在上面），
        // 高度归零即可；窗口本身本来就不可缩放（编辑器尺寸由皮肤决定）
        window->setTitleBarHeight (0);
        window->setDropShadowEnabled (false);
        window->setResizable (false, false);
        framelessApplied = true;

        // 外壳会把编辑器拉伸到"内容区大小"：标题栏一去掉，内容区变大，编辑器就被拉高
        // （实测多出 27 px）。把尺寸再写回去，让外壳按编辑器重新套窗口，而不是反过来。
        const auto& skinNow = skin::SkinBundle::getInstance().getCurrent();
        setSize (skinNow.windowWidth(), skinNow.windowHeight());

        showStatus (U8 ("已切到无标题栏窗口：拖动窗口请按住空白处，"
                        "音频设置用右键菜单或 Ctrl+Shift+A"), false);
    }
    else
    {
        window->setTitleBarHeight (savedTitleBarHeight > 0 ? savedTitleBarHeight : 26);
        window->setDropShadowEnabled (true);
        window->setResizable (false, false);
        framelessApplied = false;

        const auto& skinNow = skin::SkinBundle::getInstance().getCurrent();
        setSize (skinNow.windowWidth(), skinNow.windowHeight());
    }
}

bool OnlyObsessionRemainedEditor::isWindowDragArea (juce::Point<int> position) const
{
    if (! framelessApplied)
        return false;

    const auto& s = skin::SkinBundle::getInstance().getCurrent();

    if (s.dragAreaKind != "rect")
        return true;   // auto：能从控件缝隙落到编辑器上的点击都算（JUCE 只会把没被控件吃掉的给到这里）

    // 清单里的拖动区是设计坐标，鼠标位置是编辑器本地坐标
    const auto offset = bleedOffset();

    return s.dragAreaRect.contains (position - juce::Point<int> (offset.x, offset.y));
}

void OnlyObsessionRemainedEditor::showWindowMenu()
{
    juce::PopupMenu menu;

    menu.addSectionHeader (U8 ("OnlyObsessionRemained"));
    menu.addItem (1, U8 ("音频设置…"), isStandalone());
    menu.addItem (2, U8 ("重新加载皮肤"));
    menu.addItem (3, U8 ("回到内置皮肤"));
    menu.addItem (4, U8 ("打开皮肤目录"), designerDocument.getFolder().isDirectory());
    menu.addSeparator();
    menu.addItem (5, U8 ("退出"), isStandalone());

    if (shapeStatus.isNotEmpty())
    {
        menu.addSeparator();
        menu.addItem (99, U8 ("窗口形状没生效：") + shapeStatus, false);
    }

    menu.showMenuAsync (juce::PopupMenu::Options().withTargetComponent (this),
                        [this] (int result)
    {
        switch (result)
        {
            case 1: openAudioSettings(); return;
            case 2: reloadSkin();        return;
            case 3: skin::SkinBundle::getInstance().loadBuiltIn();
                    showStatus (U8 ("已回到内置皮肤"), false);
                    return;
            case 4: designerDocument.getFolder().revealToUser(); return;
            case 5: if (auto* window = dynamic_cast<juce::DocumentWindow*> (getTopLevelComponent()))
                        window->closeButtonPressed();
                    return;
            default: return;
        }
    });
}

void OnlyObsessionRemainedEditor::openAudioSettings()
{
    // 音频设置挂在独立运行外壳标题栏的 "Options" 按钮上；无标题栏时它被隐藏了，
    // 这里直接按一下那个按钮 —— 复用 JUCE 自己的设备对话框，不另做一套
    if (auto* top = getTopLevelComponent())
        for (auto* child : top->getChildren())
            if (auto* button = dynamic_cast<juce::Button*> (child))
                if (button->getButtonText() == "Options")
                {
                    button->triggerClick();
                    return;
                }

    showStatus (U8 ("没找到独立运行的音频设置入口（插件模式下由宿主提供）"), true);
}

// =======================================================================
// 皮肤设计器（绘图软件式的：拖动改布局 + 改形态/交互 + 实时导入贴图）
// =======================================================================

#if OOR_ENABLE_SKIN_DESIGNER
bool OnlyObsessionRemainedEditor::designerAvailable() const
{
    return processor.wrapperType == juce::AudioProcessor::wrapperType_Standalone;
}

juce::Rectangle<int> OnlyObsessionRemainedEditor::elementBounds (const juce::String& id) const
{
    if (id == "title")        return currentLayout.title;
    if (id == "presetPrev")   return currentLayout.presetPrev;
    if (id == "presetNext")   return currentLayout.presetNext;
    if (id == "preset")       return currentLayout.preset;
    if (id == "presetName")   return currentLayout.presetName;
    if (id == "presetSave")   return currentLayout.presetSave;
    if (id == "presetDelete") return currentLayout.presetDelete;
    if (id == "typeLabel")    return currentLayout.typeLabel;
    if (id == "type")         return currentLayout.type;

    for (int i = 0; i < kNumKnobs; ++i)
        if (id == params::floatSpec (i).skinId)
            return currentLayout.knob[i];

    for (int i = 0; i < kNumKnobs; ++i)
        if (id == knobLabelId (i))
            return currentLayout.knobLabel[i];

    return {};
}
#endif

juce::Component* OnlyObsessionRemainedEditor::componentForElement (const juce::String& id)
{
    if (id == "title")        return &titleLabel;
    if (id == "presetPrev")   return &prevButton;
    if (id == "presetNext")   return &nextButton;
    if (id == "preset")       return &presetBox;
    if (id == "presetName")   return &presetNameEditor;
    if (id == "presetSave")   return &saveButton;
    if (id == "presetDelete") return &deleteButton;
    if (id == "typeLabel")    return &typeLabel;
    if (id == "type")         return &typeBox;

    for (int i = 0; i < kNumKnobs; ++i)
    {
        if (id == params::floatSpec (i).skinId)   return &sliderControls[static_cast<size_t> (i)].slider;
        if (id == knobLabelId (i))   return &sliderControls[static_cast<size_t> (i)].label;
    }

    return nullptr;
}

#if OOR_ENABLE_SKIN_DESIGNER
void OnlyObsessionRemainedEditor::refreshDesignerItems()
{
    const auto& s = skin::SkinBundle::getInstance().getCurrent();

    // 文档里的矩形是设计坐标（相对画布），编辑层活在编辑器本地坐标里 —— 3e 的出血
    // 偏移在这里加一次；落盘时（onRectChanged / onLayerRectChanged）再减回去。
    const auto bleed = bleedOffset();

    std::vector<skin::DesignerOverlay::Item> items;

    for (const auto& element : s.elements)
    {
        skin::DesignerOverlay::Item item;
        item.id    = element.id;
        item.type  = element.type;

        // 设计会话期间文档（skin.json 的内存副本）是唯一事实源。
        // 注意不能直接用 elementBounds()：它返回的是"上一次 applyLayout 的布局"，
        // 刚拖完控件时 currentLayout 还是拖动之前那一份 —— 框就会弹回旧位置，
        // 而且下一次拖动会从那个旧矩形起算，改动看着就像失效了。
        item.label = designerDocument.textFor (element.id);

        if (item.label.isEmpty())
            item.label = s.textFor (element.id, element.id);

        item.rect  = designerDocument.rectFor (element.id, elementBounds (element.id))
                         .translated (bleed.x, bleed.y);
        item.hasRect = ! item.rect.isEmpty();

        // 变换同样以文档为准：编辑层据此画旋转后的框、并按逆变换判定命中
        item.transform = designerDocument.transformFor (element.id);

        // 轨迹同样来自文档：编辑层画的曲线与运行时 Skin::travelForPart 读的是同一份数据
        item.travel = designerDocument.travelFor (element.id);

        item.travelParts.clear();

        for (const auto& part : designerDocument.travelPartsFor (element.id))
        {
            auto partTravel = designerDocument.travelForPart (element.id, part);

            // 与运行时一致：推子头的悬停 / 按下态没启用轨迹时，实际回落到底态 thumb。
            if ((part == "thumbOver" || part == "thumbDown") && ! partTravel.enabled)
                partTravel = designerDocument.travelForPart (element.id, "thumb");

            item.travelParts.emplace_back (part, partTravel);
        }

        items.push_back (item);

        // 控件自己的图层：坐标是相对控件的归一化矩形，编辑层据此换算成实际区域。
        // 它们排在控件后面，命中时"已选中的图层优先"（见 DesignerOverlay::findItem）。
        for (const auto& layer : designerDocument.controlLayers (element.id))
        {
            skin::DesignerOverlay::Item controlItem;
            controlItem.id             = element.id + "/" + layer.id;
            controlItem.type           = element.type;
            controlItem.label          = layer.name.isNotEmpty() ? layer.name : layer.id;
            controlItem.isControlLayer = true;
            controlItem.ownerId        = element.id;
            controlItem.layerId        = layer.id;
            controlItem.rect           = item.rect;
            controlItem.hasRect        = item.hasRect;
            controlItem.transform      = item.transform;
            controlItem.normalizedRect = layer.rect;
            controlItem.layerTransform = layer.transform;

            items.push_back (controlItem);
        }
    }

    // 装饰图层：同样以文档为准；文档读不到（未挂载）时退回皮肤里已解析的图层
    auto layerInfos = designerDocument.layers();

    if (layerInfos.empty() && ! s.layers.empty())
        for (const auto& layer : s.layers)
        {
            skin::SkinDocument::LayerInfo info;
            info.id = layer.id;
            info.name = layer.name;
            info.imagePath = layer.imagePath;
            info.visible = layer.visible;
            info.hasRect = layer.hasRect;
            info.rect = layer.rect;
            info.opacity = layer.opacity;
            info.transform = layer.transform;
            layerInfos.push_back (info);
        }

    for (const auto& layer : layerInfos)
    {
        skin::DesignerOverlay::Item item;
        item.id       = layer.id;
        item.isLayer  = true;
        item.label    = layer.name.isNotEmpty() ? layer.name : layer.id;
        item.rect     = layer.rect.translated (bleed.x, bleed.y);
        item.hasRect  = layer.hasRect;
        item.transform = layer.transform;

        items.push_back (item);
    }

    designerOverlay.setItems (std::move (items));
}
#endif

void OnlyObsessionRemainedEditor::refreshControlLayerOverlays()
{
    const auto& s = skin::SkinBundle::getInstance().getCurrent();

    // 回收：元素消失或图层被删光后，子组件不能继续挂着（它会一直画旧内容）
    for (auto it = controlOverlays.begin(); it != controlOverlays.end();)
    {
        const auto* element = s.findElement (it->first);

        if (element == nullptr || element->layers.empty())
            it = controlOverlays.erase (it);
        else
            ++it;
    }

    for (const auto& element : s.elements)
    {
        if (element.layers.empty())
            continue;

        auto* component = componentForElement (element.id);

        if (component == nullptr)
            continue;

        auto& overlay = controlOverlays[element.id];

        if (overlay == nullptr)
        {
            overlay = std::make_unique<skin::ControlLayerOverlay> (element.id);

            // 挂在控件内部：位置/尺寸相对控件，控件的移动、缩放、旋转会自动带上它
            component->addChildComponent (*overlay);
        }

        overlay->setBounds (component->getLocalBounds());
        overlay->setVisible (true);
        overlay->toFront (false);   // 盖在数值框等子控件之上（图层本来就叠在控件之上）
        overlay->repaint();
    }
}

#if OOR_ENABLE_SKIN_DESIGNER
void OnlyObsessionRemainedEditor::toggleDesigner()
{
    if constexpr (! kLegacySkinUiEnabled)
        return;   // 旧设计器已停用（HUD 为唯一界面）

    if (hudView != nullptr)
    {
        showStatus (U8 ("当前为 HUD 固定外观，皮肤设计器不可用"), true);
        return;
    }

    if (! designerAvailable())
    {
        showStatus (U8 ("皮肤设计器只在 Standalone（独立运行）里可用"), true);
        return;
    }

    if (designerOverlay.isVisible())
    {
        designerOverlay.setVisible (false);

        if (designerWindow != nullptr)
            designerWindow->setVisible (false);

        showStatus (U8 ("已退出编辑层（现在可以直接试控件手感）"), false);
        return;
    }

    // 设计器要能写文件，所以先把皮肤落到一个可编辑的外部目录
    juce::String error;

    if (! designerDocument.attach (error) && ! designerDocument.createEditableCopy (error))
    {
        showStatus (U8 ("设计器：无法准备可编辑皮肤（") + error + U8 ("）"), true);
        return;
    }

    designerOverlay.setVisible (true);
    refreshDesignerItems();
    designerOverlay.toFront (true);
    designerOverlay.grabKeyboardFocus();

    if (designerWindow == nullptr)
    {
        skin::DesignerWindow::Callbacks callbacks;

        callbacks.editorBoundsFor   = [this] (const juce::String& id) { return elementBounds (id); };
        callbacks.documentChanged   = [this] { refreshDesignerItems(); };
        callbacks.elementSelected   = [this] (const juce::String& id)
                                      { designerOverlay.setSelected (id); };
        callbacks.travelPartSelected = [this] (const juce::String& part)
                                      { designerOverlay.setActiveTravelPart (part); };
        callbacks.shaderCompileError  = [this] { return shaderBackground.getLastError(); };
        callbacks.shaderRuntimeStatus = [this] { return describeShaderStatus(); };

        designerWindow = std::make_unique<skin::DesignerWindow> (designerDocument,
                                                                std::move (callbacks));
    }
    else
    {
        designerWindow->refreshAll();
    }

    designerWindow->setVisible (true);
    designerWindow->toFront (true);

    // 首次打开时给编辑层一个默认选中项，面板才有内容可显示
    if (designerOverlay.getSelected().isEmpty())
        designerOverlay.setSelected ("mix");

    designerWindow->selectElement (designerOverlay.getSelected());

    showStatus (U8 ("编辑层已开：拖动元素改位置 / 右下角小方块改尺寸 / 方向键微调；"
                    "再按 Ctrl+Shift+E 退出编辑层试手感"), false);
}
#endif
