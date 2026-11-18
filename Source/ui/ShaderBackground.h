#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_opengl/juce_opengl.h>

/**
    背景层的动态着色器（阶段三 3c）。

    职责：把片元着色器铺在界面最底层，并把实时参数与湿声电平喂给它。
    源码有两个来源：皮肤清单里的 `shader.source`（`loadFromSkin`），
    或由调用方直接注入（`loadDirectSource`，HUD 背景走这条）。
    只负责背景层——控件仍走 CPU 侧的贴图绘制（见 docs/adr/0001-shader-is-background-only.md）。

    ---- 两条渲染通路与降级 ----

    挂载 GPU 上下文后**延时探测**是否真的就绪；就绪则 `paintBackground` 走 GPU，
    否则本次会话内永久走 CPU 兜底绘制（兜底图，或缺图时参数响应的渐变）。
    探测用 `isAttached()`（"已挂载且在屏"），不是 `isActive()`（那只表示"当前线程的
    活跃上下文"，与就绪无关）。详见 docs/adr/0002-degrade-to-cpu-and-own-timer.md。

    ---- 对用户着色器的契约（写模板时必须说清）----

    JUCE 会在用户源码**之前**自动注入这三个声明，所以用户**不得重复声明**它们：

        varying vec4 frontColour;      // 当前像素的颜色（含剪裁 alpha）
        varying vec2 pixelPos;         // 当前像素在绘图上下文中的位置（设备像素）
        #define pixelAlpha frontColour.a

    用户只需声明自己用到的 uniform 与 `main()`；输出写 `gl_FragColor`
    （JUCE 会翻译成 GLSL v3）。不要写 `#version`。

    本类自动绑定的 uniform（尺寸一律是设备像素，125% DPI 下已乘 1.25）：

        vec2  iResolution   背景区域尺寸（设备像素）
        vec2  iOffset       背景区域左上角在画布中的位置（设备像素）
        float iTime         秒
        vec2  iMouse        鼠标在画布中的位置（设备像素，未进入窗口时保持上次值）
        float pMix pDecay pPreDelay pMod pHighDamp pLowDamp pShimmer   0…1
        float pType         0=板式 1=房间 2=弹簧
        float pLevel        湿声电平 0…1（约 100 ms 释放包络）
        float pDark         宿主/系统深色 1 或 0
*/
class ShaderBackground final : private juce::Timer
{
public:
    ShaderBackground();
    ~ShaderBackground();

    /** 把 GPU 上下文挂到编辑器上。编辑器构造时调用即可（组件此时还不必可见）。 */
    void attachTo (juce::Component& editor);

    /** 皮肤切换后重新读取着色器源码 / 兜底图 / 帧率；源码有变化才重编译。 */
    void loadFromSkin();

    /**
        直接用一段源码驱动（HUD 背景走这条：不经过皮肤清单）。

        与 loadFromSkin() 的区别只有两处：不读兜底图（HUD 的兜底就是纯黑底），
        以及 CPU 降级时不画参数响应的强调色光晕 —— 免得把皮肤的强调色带进 HUD。
        源码有变化才重编译；同一份源码重复调用是空操作。
    */
    void loadDirectSource (const juce::String& source, int fps);

    /** 每帧的 uniform 取值。由编辑器每帧填好传来，本类不自己计时。 */
    struct FrameState
    {
        float time = 0.0f;          // 秒，由编辑器的时钟推进（后续动画系统复用同一时钟）

        float mix = 0.0f, decay = 0.0f, preDelayMs = 0.0f, mod = 0.0f;
        float highDamp = 0.0f, lowDamp = 0.0f, shimmer = 0.0f;
        float type = 0.0f;          // 0=板式 1=房间 2=弹簧
        float level = 0.0f;         // 湿声电平
        float dark = 1.0f;

        juce::Point<float> mouse;
    };

    void setFrameState (const FrameState& state) noexcept { frameState = state; }

    /**
        在 paint() 里画背景。GPU 通路可用时用着色器填充，否则画 CPU 兜底。

        绑给着色器的尺寸必须乘上绘图上下文的 `getPhysicalPixelScaleFactor()`：pixelPos 是
        设备像素（顶点位置已被 DPI 变换换算到物理像素），125% / 150% 缩放下如果只给逻辑尺寸，
        uv 就铺不满背景，构图会整体偏左上。CPU 兜底仍按 area 的逻辑坐标画。

        注意不要用 `OpenGLContext::getRenderingScale()`：它只有装了自定义渲染器时才更新，
        这里恒为 1.0（见 juce_OpenGLContext.cpp:410-415）。
    */
    void paintBackground (juce::Graphics&, juce::Rectangle<int> area, juce::Colour fallbackColour);

    // ---- 状态查询（界面与设计器用）----

    /** GPU 通路是否真的就绪（探测通过）。 */
    bool isGpuActive() const noexcept { return gpuActive; }

    /** 探测是否已结束（结束前 isGpuActive 为假属正常，不代表要降级）。 */
    bool isProbeDone() const noexcept { return probeDone; }

    /** 当前皮肤是否提供了着色器源码。 */
    bool hasShader() const noexcept { return activeShader != nullptr || candidateShader != nullptr; }

    /**
        皮肤**声明**了着色器（清单里有 shader.source）。

        注意与 hasShader() 的区别：编译失败后 hasShader() 会变回假（我们不再持有可用的
        着色器对象），但皮肤依然"声明了着色器"——这时应该走 CPU 兜底绘制并重试编译，
        而不是退化成一整块纯底色。界面判定一律用这个。
    */
    bool skinHasShader() const noexcept { return sourcePath.isNotEmpty(); }

    /** 用户级总开关（逃生门，与本皮肤无关）。 */
    bool isEnabled() const noexcept { return userEnabled; }
    void setUserEnabled (bool shouldBeEnabled);

    /** 帧率（Hz），来自皮肤的 `shader.params.fps`。 */
    int getFrameRate() const noexcept { return frameRate; }

    /** 最近一次编译/链接错误；空串表示正常。 */
    juce::String getLastError() const { return lastError; }

    /** 着色器源码在皮肤里的相对路径（空串 = 该皮肤没有着色器）。 */
    juce::String getSourcePath() const { return sourcePath; }

    // ---- 用户级开关的持久化（与皮肤设置同文件）----

    static bool readEnabledFromSettings();
    static void writeEnabledToSettings (bool shouldBeEnabled);

private:
    /** 延时探测 isAttached()，就绪则转 GPU，超时则永久降级。 */
    void timerCallback() override;

    /** GPU 对象必须先在当前线程激活上下文才能安全销毁。 */
    void releaseGpuObjects();

    void applyUniforms (juce::OpenGLShaderProgram& program);

    /** CPU 兜底：兜底图，或缺图时参数响应的渐变（不随 time 动，省 CPU）。 */
    void paintCpuFallback (juce::Graphics&, juce::Rectangle<int> area, juce::Colour fallbackColour);

    juce::Component* target = nullptr;

    std::unique_ptr<juce::OpenGLContext> context;
    std::unique_ptr<juce::OpenGLGraphicsContextCustomShader> activeShader;    // 已验证可用
    std::unique_ptr<juce::OpenGLGraphicsContextCustomShader> candidateShader; // 待验证

    juce::String shaderSource, sourcePath, fallbackImagePath;

    bool directSource = false;   // 源码由 loadDirectSource() 注入（HUD），不是皮肤声明的

    int  frameRate = 30;
    bool userEnabled = true;
    bool gpuActive   = false;
    bool probeDone   = false;
    int  probeAttempts = 0;

    juce::String lastError;

    FrameState frameState;
    juce::Rectangle<int> paintArea;
    float paintScale = 1.0f;   // 本次绘制的设备像素 / 逻辑像素（125% DPI 时为 1.25）

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ShaderBackground)
};
