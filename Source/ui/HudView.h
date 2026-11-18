#pragma once

#include <array>
#include <memory>

#include <juce_gui_basics/juce_gui_basics.h>

#include "PluginProcessor.h"
#include "ShaderBackground.h"

/**
    HUD 运行时视图（v30 白色版）。

    设计稿：docs/ui-m1-sheet.svg（960×520 逻辑坐标）。
    整套界面由 JUCE 矢量绘制：没有静态底图，因此三个板块
    （5 根推子 / MIX 大旋钮 / SHMR 小旋钮）可以各自平移、缩放、旋转，
    并按 6:3:1 的版式摆放。布局可用 Ctrl+Shift+L 的编辑层实时调整，
    结果写入用户数据目录下的 hud-layout.json。

    背景另有一层 GPU 场景着色器（源码内嵌在 HudSceneShader.h，不走皮肤清单）；
    GPU 不可用时退回原来的纯黑底，控件绘制与手感不受影响。

    动效仍按 docs/ui-motion-guide.md：30Hz 状态翻帧 + 60Hz 弹簧，
    硬切、错帧、扫描脉冲。
*/
class HudView final : public juce::Component,
                      private juce::Timer,
                      private juce::ChangeListener
{
public:
    explicit HudView (OnlyObsessionRemainedProcessor&);
    ~HudView() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseMove (const juce::MouseEvent&) override;
    void mouseExit (const juce::MouseEvent&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp (const juce::MouseEvent&) override;
    void mouseWheelMove (const juce::MouseEvent&, const juce::MouseWheelDetails&) override;
    void visibilityChanged() override;

    /** Ctrl+Shift+L：进入 / 退出板块布局编辑层。 */
    void toggleLayoutEditor();
    bool isLayoutEditorOpen() const noexcept { return layoutMode; }

    // ---- 背景着色器（Ctrl+Shift+S 的总开关与状态）----

    bool isBackdropEnabled() const noexcept { return backdrop.isEnabled(); }
    void setBackdropEnabled (bool shouldBeEnabled);

private:
    static constexpr float designWidth  = 960.0f;
    static constexpr float designHeight = 520.0f;

    /** 逐个控件入场的总窗口（30 Hz 下约 0.47 s）。现有入场从这一帧之后才开始计时。 */
    static constexpr int kPopFrames = 14;

    /** 3D 浮现入场开关。当前关闭，恢复为浮现之前的入场；改成 true 即可重新启用。 */
    static constexpr bool kPopInEnabled = false;

    // ------------------------------------------------------------------
    // 板块变换（6:3:1 版式的可编辑部分）
    // ------------------------------------------------------------------

    enum Block { blockFaders = 0, blockMix, blockShimmer, blockDataA, blockDataB, blockCount };

    struct BlockTransform
    {
        juce::Point<float> pivot { 0.0f, 0.0f };     // 旋转 / 缩放中心（本地坐标）
        juce::Point<float> offset { 0.0f, 0.0f };    // 平移量（设计坐标）
        float scale = 1.0f;
        float rotation = 0.0f;                       // 角度

        juce::AffineTransform matrix() const noexcept;
        juce::Point<float> toLocal (juce::Point<float>) const noexcept;
    };

    // ------------------------------------------------------------------
    // 控件状态
    // ------------------------------------------------------------------

    struct KnobState
    {
        const char* paramId = nullptr;
        juce::Point<float> centre;
        float radius = 0.0f;
        float needleInner = 0.0f;
        float needleOuter = 0.0f;
        float valueCx = 0.0f;
        float valueBaseline = 0.0f;
        float bracketSize = 18.0f;
        float valueSize = 27.0f;
        float unitSize = 8.0f;
        const char* label = nullptr;
        juce::Rectangle<float> labelBox;      // 非空：白底黑字的标签块
        float labelX = 0.0f;
        float labelBaseline = 0.0f;
        float labelSize = 7.0f;
        int   appearFrame = 0;
        int   scanPhase = 0;

        float displayNorm = 0.0f;
        float targetNorm  = 0.0f;
        float denormValue = 0.0f;

        bool  hovered = false;
        int   hoverFrame = -1000;
        bool  dragging = false;
        float dragStartY = 0.0f;
        float dragStartNorm = 0.0f;
        int   grabFrame = -1000;
        int   releaseFrame = -1000;

        juce::RangedAudioParameter* param = nullptr;
        std::unique_ptr<juce::ParameterAttachment> attachment;
    };

    struct FaderState
    {
        const char* paramId = nullptr;
        const char* label = nullptr;
        juce::Point<float> root;
        juce::Point<float> end;
        float labelX = 0.0f;
        float labelBaseline = 0.0f;
        bool  outlinedHandle = false;
        float handleRadius = 4.0f;          // 手柄菱形沿两条轴的半径
        juce::Rectangle<float> valueBox;
        int   decimals = 1;
        int   appearFrame = 0;

        float displayNorm = 0.0f;
        float targetNorm  = 0.0f;
        float denormValue = 0.0f;
        float glowNorm    = 0.0f;           // 跟随光条的滞后位置（比 displayNorm 慢一档的弹簧）

        bool  hovered = false;
        int   hoverFrame = -1000;
        int   hoverEndFrame = -1000;
        bool  dragging = false;
        float dragStartProj = 0.0f;
        float dragStartNorm = 0.0f;
        int   grabFrame = -1000;
        int   releaseFrame = -1000;

        juce::RangedAudioParameter* param = nullptr;
        std::unique_ptr<juce::ParameterAttachment> attachment;
    };

    enum class Button
    {
        prev, next, presetField, save, del, type, typeUp, typeDown, layout,
        layoutSave, layoutLoad, layoutReset, count
    };

    struct Hit
    {
        enum class Kind { none, button, knob, fader };
        Kind kind = Kind::none;
        int  index = -1;      // button: Button 数值；knob/fader: 数组下标
    };

    // ------------------------------------------------------------------
    // 构建与交互
    // ------------------------------------------------------------------

    void buildControls();
    void buildAttachments();

    Hit  hitTest (juce::Point<float> designPoint) const;
    void updateHover (const Hit&);
    void handleButton (Button, juce::Point<float> designPoint);

    void beginKnobDrag (int index, juce::Point<float> designPoint);
    void beginFaderDrag (int index, juce::Point<float> designPoint);
    void setKnobNorm (int index, float norm);
    void setFaderNorm (int index, float norm);
    float faderProjection (const FaderState&, juce::Point<float> designPoint) const;

    void showPresetMenu();
    void beginSave();
    void commitSave();
    void cancelSave();
    void deleteCurrentPreset();
    void cycleType (int delta);

    void refreshFromParams (bool snap);
    juce::Rectangle<float> buttonRect (Button) const;
    juce::Rectangle<float> presetTextRect() const;

    /** 当前控件状态（用于状态栏与日志）。 */
    juce::String currentStatus() const;
    void setEvent (const juce::String& eventText);
    void setTransient (const juce::String& status, int frames);

    // ------------------------------------------------------------------
    // 板块布局
    // ------------------------------------------------------------------

    const BlockTransform& transformFor (int block) const;
    juce::AffineTransform matrixFor (int block) const;
    juce::Rectangle<float> blockDesignBounds (int block) const;
    int  blockAt (juce::Point<float> designPoint) const;
    void loadLayout();
    void saveLayout() const;
    void resetLayout();
    juce::File layoutFile() const;
    void readLayout (const juce::File&);
    void writeLayout (const juce::File&) const;

    // 布局预设：layout-presets/<name>.json，与自动保存的 hud-layout.json 独立
    juce::File layoutPresetDirectory() const;
    juce::StringArray layoutPresetNames() const;
    juce::String nextLayoutPresetName() const;
    int  layoutFooterButtonAt (juce::Point<float> designPoint) const;
    void beginLayoutSave();
    void commitLayoutSave();
    void showLayoutPresetMenu();
    void applyLayoutPreset (const juce::String& name);

    // ------------------------------------------------------------------
    // 动效与绘制
    // ------------------------------------------------------------------

    int   frameNow() const;
    float alphaFor (int appearFrame) const;

    /** 现有入场的时间轴：3D 浮现阶段结束后的帧数。 */
    int introFrame() const;

    /** 现有入场里"结构层"（分隔线 / 鳍片）的硬切透明度；控件不再二次硬切。 */
    float structureAlpha (int appearFrame) const;

    /** 某个设计坐标枢轴的 3D 浮现变换（缩放 + 向中心收拢 + 轻转，均已缓动）。 */
    juce::AffineTransform popMatrixAt (juce::Point<float> pivot, float delayFrames) const;

    bool  needsRepaint (int frame) const;
    void  timerCallback() override;
    void  changeListenerCallback (juce::ChangeBroadcaster*) override;
    bool  keyPressed (const juce::KeyPress&) override;

    void drawChrome (juce::Graphics&);
    void drawDecorations (juce::Graphics&);
    void drawTopBar (juce::Graphics&, int frame);
    void drawFooter (juce::Graphics&, int frame);
    void drawKnob (juce::Graphics&, int index, int frame);
    void drawFader (juce::Graphics&, int index, int frame);
    void drawFins (juce::Graphics&, float alpha) const;
    void drawKnobStatic (juce::Graphics&, const KnobState&, float alpha) const;
    void drawFaderStatic (juce::Graphics&, const FaderState&, float alpha) const;
    void drawKnobValueText (juce::Graphics&, const KnobState&, float alpha, float bracketExpand) const;
    void drawAdjustedTag (juce::Graphics&, juce::Point<float> anchor, bool visible) const;
    void drawButtonHover (juce::Graphics&, juce::Rectangle<float>, float phase) const;
    void drawHoverBrackets (juce::Graphics&, juce::Rectangle<float>, float phase, float alpha) const;
    void drawLayoutOverlay (juce::Graphics&, int frame);

    // ------------------------------------------------------------------
    // 数据
    // ------------------------------------------------------------------

    OnlyObsessionRemainedProcessor& processor;

    // 背景着色器：独立于皮肤清单，直接注入场景源码（见 HudSceneShader.h）。
    // 组件自己挂 GL 上下文，所以 HUD 的每次重绘都先铺这层，不依赖编辑器。
    ShaderBackground backdrop;
    double backdropTime = 0.0;
    double lastBackdropFrame = 0.0;
    juce::String lastBackdropError;

    std::array<BlockTransform, blockCount> blockTransform;
    std::array<juce::Rectangle<float>, blockCount> blockBounds;
    bool layoutMode = false;
    int  layoutSelected = blockFaders;
    bool layoutDragging = false;
    juce::Point<float> layoutDragPoint;
    juce::Point<float> layoutDragOffset;
    bool layoutSaveMode = false;                 // nameEditor 正在为布局预设命名

    int blockForKnob (int index) const noexcept
    {
        return index == 0 ? blockMix : blockShimmer;
    }

    std::array<KnobState, 2>  knobs;
    std::array<FaderState, 5> faders;

    std::unique_ptr<juce::ParameterAttachment> typeAttachment;
    juce::RangedAudioParameter* typeParam = nullptr;
    int typeIndex = 0;

    juce::TextEditor nameEditor;

    double startTime = 0.0;
    double lastTickTime = 0.0;
    int    lastFrame = -1;
    bool   initialised = false;

    int hoveredButton = -1;
    int buttonHoverFrame = -1000;
    int hoveredKnob   = -1;
    int hoveredFader  = -1;

    bool   saveMode = false;
    bool   presetFieldPressed = false;
    double deleteArmedUntil = 0.0;

    juce::String transientStatus;
    int          transientUntilFrame = -1000;
    juce::String lastEvent;
    juce::String lastEventTime;

    int buttonFlashFrame = -1000;
    int buttonFlashIndex = -1;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (HudView)
};
