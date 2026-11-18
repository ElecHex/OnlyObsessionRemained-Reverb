#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>

#include "PluginProcessor.h"
#include "ui/ControlLayerOverlay.h"
#include "ui/HudView.h"
#include "ui/ShaderBackground.h"
#include "ui/SkinBundle.h"
#include "ui/SkinDesigner.h"
#include "ui/SkinnedLookAndFeel.h"

/**
    插件界面。

    当前外观是 M1 定稿 HUD（HudView，960×520）：全矢量绘制（无静态底图），
    三个板块（推子组 / MIX / SHMR）可各自平移缩放旋转。
    皮肤系统仍保留在代码里作为旧路径，但 HUD 模式下不显示皮肤控件，也不跑皮肤着色器。

    Ctrl+Shift+O / R / D：选择外部皮肤目录 / 重新加载 / 回到内置（旧皮肤路径）。
    Ctrl+Shift+E：皮肤设计器（仅 Standalone；HUD 模式下不可用）。
    Ctrl+Shift+S：开 / 关动态着色器背景（HUD 模式下无效）。
    Ctrl+Shift+A：独立运行的音频设置（无标题栏的异形皮肤顶上标题栏那条入口）。
*/
class OnlyObsessionRemainedEditor final : public juce::AudioProcessorEditor,
                             private juce::ChangeListener,
                             private juce::Timer
{
public:
    explicit OnlyObsessionRemainedEditor (OnlyObsessionRemainedProcessor&);
    ~OnlyObsessionRemainedEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    bool keyPressed (const juce::KeyPress&) override;

    /** 3e：窗口外形要在"被挂进独立运行外壳之后"再施加一遍 —— 编辑器构造时还没有顶层窗口，
        那时算出来的 host / peer 都是空的；挂上去、显示出来时才是真正能裁窗口的时机。 */
    void parentHierarchyChanged() override;
    void visibilityChanged() override;

private:
    /**
        旧皮肤 UI（LookAndFeel / 隐藏参数控件 / 异形窗口）与皮肤设计器在 HUD 模式下不可达。
        置 false 后：不再创建 SliderAttachment、不再响应换肤/设计器快捷键、不再驱动皮肤着色器；
        代码全部保留，改 true 即可恢复旧路径。
    */
    static constexpr bool kLegacySkinUiEnabled = false;

    /** 旋钮数量；必须与 PluginEditor.cpp 里 kKnobSkinIDs 的条目数一致。 */
    static constexpr int kNumKnobs = 7;

    struct SliderControl
    {
        juce::Label  label;
        SkinnedSlider slider;
        std::unique_ptr<juce::AudioProcessorValueTreeState::SliderAttachment> attachment;
    };

    /** 一次布局的全部矩形：先由内置算法算出，再用皮肤清单逐项覆盖。 */
    struct Layout
    {
        juce::Rectangle<int> title, presetPrev, presetNext, preset, presetName,
                             presetSave, presetDelete, typeLabel, type;
        juce::Rectangle<int> knob[kNumKnobs], knobLabel[kNumKnobs];
    };

    void addSliderControl (const juce::String& paramID, const juce::String& skinID,
                           const juce::String& displayName, SliderControl& control);

    /**
        应用清单里的"控件形态与操作逻辑"：旋钮 / 推子 / 条形、旋钮角度、拖动方式、
        拖动灵敏度、数值框方位与尺寸、滚轮与双击复位。皮肤切换时会被重新调用，
        因此设计器里改一个参数、预览立刻变化。
    */
    void applySliderSpec (const juce::String& paramID, const juce::String& skinID,
                          SliderControl& control);

    /** 重建预设下拉列表（出厂 + 用户两组），并选中 PresetManager 的当前项。 */
    void rebuildPresetList();

    /** 选中指定索引（自动环绕）；loadPreset 为真时同时载入。 */
    void selectPreset (int index, bool loadPreset);

    /** PresetManager 载入/保存/删除后回调；皮肤切换走同一个回调。 */
    void changeListenerCallback (juce::ChangeBroadcaster* source) override;

    void timerCallback() override;

    /** 把当前皮肤应用到全部控件（颜色、字体、文案、贴图身份 id、画布尺寸）。 */
    void applySkin();

    Layout computeDefaultLayout (int canvasWidth, int canvasHeight) const;
    void   applyLayout (const Layout& layout);

    /**
        把清单里的元素变换（旋转 / 缩放 / 平移）套到**控件本体**上。

        用 Component::setTransform：JUCE 会把变换作用到该组件及其子组件的渲染**和**命中判定，
        所以"设计器里的框转了多少，控件就转多少"——而不是只有贴图跟着转（矢量兜底画法、
        数值框文字、按钮底色这些都要一起转）。
    */
    void refreshElementTransforms();

    /** 给某个元素对应的控件设变换；锚点按该控件当前的未变换矩形换算。 */
    void setElementTransform (const juce::String& id, const skin::Transform& transform);

    void chooseSkinFolder();
    void reloadSkin();

    // ---- 异形窗口与画布出血（阶段三 3e）----

    /**
        画布在窗口里的偏移量（编辑器本地坐标 = 设计坐标 + 这个偏移）。

        这是 3e 的**唯一坐标换算点**：清单/文档/`currentLayout`/`elementBounds` 全部是
        "相对画布左上角"的设计坐标；只有真正摆控件（applyLayout）、画装饰图层、喂编辑层、
        以及从编辑层落盘时，才在两边各加/减这么一次。
    */
    juce::Point<int> bleedOffset() const;

    /** 谁承载这个窗口的 peer：Standalone 是顶层窗口（GL 子窗口也挂在它下面），VST3 是编辑器自己。 */
    juce::Component* windowHostComponent();

    /** 无标题栏（frameless）现在是否生效 —— 只有 Standalone 做得到。 */
    bool isStandalone() const;

    /** 现在要不要真的裁窗口区域（Standalone 的 auto/force，或 VST3 的 force）。 */
    bool shouldClipWindowRegion() const;

    /** 现在要不要按蒙版裁剪绘制（有蒙版且 mode 不是 off）。 */
    bool shouldClipPainting() const;

    /** 按皮肤要求施加/撤销窗口区域；按签名缓存，尺寸或蒙版没变就不动系统调用。 */
    void applyShapeRegion();

    /** 按皮肤要求施加/撤销无标题栏（含阴影与可缩放性），并补上音频设置入口。 */
    void applyWindowFrame();

    /**
        把窗口外形的施加推到消息循环下一轮再做。

        不能在 parentHierarchyChanged / visibilityChanged 里直接改标题栏与可缩放性：那一刻
        JUCE 正在装配窗口，改样式会让 peer 重建，独立运行外壳的内容尺寸还没落定，
        窗口会缩成一个很小的方块（实测 128×128）。推到下一轮，窗口已经稳了。
    */
    void scheduleWindowFrameApply();

    /** 按住窗口的空白处拖动（仅 frameless 的 Standalone）；位置是编辑器本地坐标。 */
    bool isWindowDragArea (juce::Point<int> position) const;

    /** 黄金路径里"落在空白处"的按下：右键弹菜单，否则开始拖窗口（无标题栏时）。
        编辑层开着时事件先到编辑层，它会通过 onEmptyAreaPressed 转交到这里。 */
    void beginWindowMouseInteraction (const juce::MouseEvent& event);

    /** 上面那个按下之后的拖动过程。 */
    void continueWindowDrag (const juce::MouseEvent& event);

    /** 右键菜单：无标题栏时顶上标题栏丢掉的入口（音频设置 / 刷新 / 退出）。 */
    void showWindowMenu();

    /** 打开独立运行外壳的音频设置（点它标题栏上的 Options 按钮，复用 JUCE 自己的设备对话框）。 */
    void openAudioSettings();

    // ---- 皮肤设计器（只在 Standalone 里开放，见 toggleDesigner）----

    /** 设计器只在独立运行时可用的原因：DAW 里编辑皮肤会写到用户的皮肤目录，
        而且宿主窗口尺寸由宿主决定、拖拽体验很差。 */
    bool designerAvailable() const;

    void toggleDesigner();

    /** 把元素几何喂给编辑层（与真实控件 bounds 完全一致）。 */
    void refreshDesignerItems();

    /**
        同步控件图层渲染层：给带 `elements[].layers[]` 的控件挂透明子组件，
        控件尺寸变化后重设子组件 bounds，元素/图层消失时回收。
    */
    void refreshControlLayerOverlays();

    /** 元素 id → 当前布局矩形（清单没写矩形时用它兜底）。 */
    juce::Rectangle<int> elementBounds (const juce::String& id) const;

    juce::Component* componentForElement (const juce::String& id);

    // ---- 动态着色器背景（阶段三 3c）----

    /**
        界面帧时钟：驱动动态着色器（以及后续的界面动画）重绘。
        刻意做成可复用的形状——动画系统复用同一个时钟，不另起一套定时器，
        见 docs/adr/0002-degrade-to-cpu-and-own-timer.md。
    */
    struct FrameClock final : juce::Timer
    {
        std::function<void()> onTick;

        void timerCallback() override
        {
            if (onTick != nullptr)
                onTick();
        }
    };

    /** 每帧：推进时间、采集实时参数与鼠标位置、交给着色器，然后重绘。 */
    void advanceFrame();

    /** 帧率变化（换皮肤）后按新帧率重启时钟。 */
    void restartFrameClock();

    void toggleShaderBackground();

    /** 着色器运行状态一句话，供设计器面板显示（GPU 就绪 / 已降级 / 未启用…）。 */
    juce::String describeShaderStatus() const;

    ShaderBackground shaderBackground;
    FrameClock        frameClock;
    double            lastFrameTime = 0.0;
    float             shaderTime    = 0.0f;   // 只累计"可见"的时间，暂停回来不会跳
    juce::Point<float> lastMousePosition;     // 指针离开窗口时保持上次值（着色器契约如此）
    bool              shaderDegradeNotified = false;
    juce::String      shaderErrorNotified;    // 最近一次已提示过的编译错误（避免每帧重复弹）

    /** 界面底部的一条短提示（皮肤加载结果/报错），几秒后自动消失。 */
    void showStatus (const juce::String& text, bool isError);

    OnlyObsessionRemainedProcessor& processor;

    SkinnedLookAndFeel lookAndFeel;

    /** M1 定稿 HUD：覆盖整窗的运行时视图（默认界面）。 */
    std::unique_ptr<HudView> hudView;

    juce::Label    titleLabel;
    juce::Label    typeLabel;
    juce::ComboBox typeBox;
    std::unique_ptr<juce::AudioProcessorValueTreeState::ComboBoxAttachment> typeAttachment;

    juce::ComboBox   presetBox;
    juce::TextButton prevButton   { "<" };
    juce::TextButton nextButton   { ">" };
    juce::TextButton saveButton   { "Save" };
    juce::TextButton deleteButton { "Del" };
    juce::TextEditor presetNameEditor;

    std::array<SliderControl, kNumKnobs> sliderControls;

    std::unique_ptr<juce::FileChooser> skinFolderChooser;

    // 设计器
    Layout                  currentLayout;      // resized() 的结果，供编辑层与设计器读取
    skin::SkinDocument      designerDocument;
    skin::DesignerOverlay   designerOverlay;
    std::unique_ptr<skin::DesignerWindow> designerWindow;

    // 图层拖动过程中的临时矩形：只影响绘制，松手才写进清单并热重载
    std::map<juce::String, juce::Rectangle<int>> layerPreviewRects;

    // 控件图层渲染层：元素 id → 挂在控件上的透明子组件
    std::map<juce::String, std::unique_ptr<skin::ControlLayerOverlay>> controlOverlays;

    // ---- 异形窗口（3e）----
    juce::ComponentDragger windowDragger;                 // 无标题栏时拖窗口用
    bool          framelessApplied  = false;              // 当前是否已经把标题栏隐掉了
    bool          frameApplyScheduled = false;            // 已排过一轮"下一轮再施加外壳"
    int           savedTitleBarHeight = 0;                // 隐之前的高度，撤销时要还原
    juce::String  appliedShapeSignature;                  // 区域签名：没变就不重复动系统调用
    juce::String  shapeStatus;                            // 区域施加失败的原因（状态条 / 设计器用）

    juce::String statusText;
    bool         statusIsError = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OnlyObsessionRemainedEditor)
};