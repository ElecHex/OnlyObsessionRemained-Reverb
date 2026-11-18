#include "ui/ShaderBackground.h"

#include <cmath>

#include "ui/SkinBundle.h"
#include "ui/UiText.h"

namespace
{
    /** 探测节奏：每 250 ms 一次，最多 8 次（约 2 s）后就永久降级。 */
    constexpr int kProbeIntervalMs = 250;
    constexpr int kMaxProbeAttempts = 8;

    /** 没有兜底图时，参数响应渐变的时间常数与强度。 */
    constexpr float kFallbackGlowAlpha = 0.22f;
}

ShaderBackground::ShaderBackground()
{
    userEnabled = readEnabledFromSettings();
}

ShaderBackground::~ShaderBackground()
{
    stopTimer();

    // 顺序：先摘除上下文（避免上下文在挂载状态下被销毁），再释放着色器包装，
    // 最后销毁上下文本身。全程在消息线程。
    if (context != nullptr)
        context->detach();

    releaseGpuObjects();

    if (context != nullptr)
        context.reset();
}

void ShaderBackground::releaseGpuObjects()
{
    candidateShader.reset();
    activeShader.reset();
}

void ShaderBackground::attachTo (juce::Component& editor)
{
    target = &editor;

    if (context != nullptr)
        return;   // 已挂载过

    context = std::make_unique<juce::OpenGLContext>();

    // 这两个必须在 attachTo 之前设置（JUCE 的明确要求）
    context->setComponentPaintingEnabled (true);   // 组件照常绘制：着色器铺底，控件画在其上
    context->setContinuousRepainting (false);      // 连续重绘交由我们自己的时钟控制

    context->attachTo (editor);

    // 之后由我们驱动重绘（帧率有限、窗口不可见时停），所以不换帧同步
    context->setSwapInterval (0);

    startTimer (kProbeIntervalMs);
}

void ShaderBackground::timerCallback()
{
    if (probeDone)
    {
        stopTimer();
        return;
    }

    if (context != nullptr && context->isAttached())
    {
        // isAttached() = 已挂载且在屏，这才是"真的能用"的判据
        gpuActive = true;
        probeDone = true;
        stopTimer();
        return;
    }

    if (++probeAttempts >= kMaxProbeAttempts)
    {
        probeDone = true;
        gpuActive = false;
        stopTimer();
    }
}

void ShaderBackground::loadFromSkin()
{
    directSource = false;

    const auto& s = skin::SkinBundle::getInstance().getCurrent();

    sourcePath        = s.shaderSourcePath;
    fallbackImagePath = s.shaderFallbackImage;

    const auto newSource = sourcePath.isEmpty() ? juce::String()
                                                : s.readAssetText (sourcePath);

    // 帧率可由皮肤覆盖；夹在合理区间，避免清单写出 0 或 10000 这种值
    frameRate = juce::jlimit (5, 120,
                              static_cast<int> (s.shaderParam ("fps", juce::var (30))
                                                    .toString().getIntValue()));

    if (newSource == shaderSource && candidateShader == nullptr)
        return;   // 源码没变，且没有待验证的候选，直接复用当前着色器

    shaderSource = newSource;

    if (shaderSource.isEmpty())
    {
        releaseGpuObjects();
        lastError.clear();
        return;
    }

    // 构造时不编译（JUCE 的构造函数只存源码），真正的编译校验发生在下一次绘制时。
    // 于是"保留上一个可用着色器"就落在这里：候选翻译失败时 activeShader 原封不动。
    candidateShader = std::make_unique<juce::OpenGLGraphicsContextCustomShader> (shaderSource);
    candidateShader->onShaderActivated = [this] (juce::OpenGLShaderProgram& program)
    {
        applyUniforms (program);
    };
}

void ShaderBackground::loadDirectSource (const juce::String& source, int fps)
{
    directSource = true;
    fallbackImagePath.clear();
    frameRate = juce::jlimit (5, 120, fps);

    if (source == shaderSource && candidateShader == nullptr)
        return;   // 源码没变，且没有待验证的候选

    shaderSource = source;

    if (shaderSource.isEmpty())
    {
        releaseGpuObjects();
        lastError.clear();
        return;
    }

    // 与 loadFromSkin() 同一套"候选 → 下次绘制时编译"的流程：
    // 编译失败时保留上一个可用版本（首次调用时没有旧版本，就只剩黑底）。
    candidateShader = std::make_unique<juce::OpenGLGraphicsContextCustomShader> (shaderSource);
    candidateShader->onShaderActivated = [this] (juce::OpenGLShaderProgram& program)
    {
        applyUniforms (program);
    };
}

void ShaderBackground::applyUniforms (juce::OpenGLShaderProgram& program)
{
    // pixelPos 是**设备像素**：paintComponent() 会先给绘图上下文加一个 DPI 缩放变换，
    // 顶点位置因此落在物理像素上；而 OpenGLContext::getRenderingScale() 只有装了自定义
    // 渲染器时才更新，这里恒为 1.0，不能用它。真实比例取自绘图上下文的
    // getPhysicalPixelScaleFactor()（在 paintBackground 里存下来）。
    // 不乘这个比例的话，125% 缩放下 uv 只铺满背景左上角 720×392，构图会偏左偏上。
    const auto pixelScale = paintScale > 0.0f ? paintScale : 1.0f;

    program.setUniform ("iResolution",
                        static_cast<GLfloat> (paintArea.getWidth()  * pixelScale),
                        static_cast<GLfloat> (paintArea.getHeight() * pixelScale));

    program.setUniform ("iOffset",
                        static_cast<GLfloat> (paintArea.getX() * pixelScale),
                        static_cast<GLfloat> (paintArea.getY() * pixelScale));

    program.setUniform ("iTime", frameState.time);

    program.setUniform ("iMouse",
                        frameState.mouse.x * pixelScale,
                        frameState.mouse.y * pixelScale);

    program.setUniform ("pMix",       frameState.mix);
    program.setUniform ("pDecay",     frameState.decay);
    program.setUniform ("pPreDelay",  frameState.preDelayMs);
    program.setUniform ("pMod",       frameState.mod);
    program.setUniform ("pHighDamp",  frameState.highDamp);
    program.setUniform ("pLowDamp",   frameState.lowDamp);
    program.setUniform ("pShimmer",   frameState.shimmer);
    program.setUniform ("pType",      frameState.type);
    program.setUniform ("pLevel",     frameState.level);
    program.setUniform ("pDark",      frameState.dark);
}

void ShaderBackground::paintBackground (juce::Graphics& g, juce::Rectangle<int> area,
                                       juce::Colour fallbackColour)
{
    if (area.isEmpty())
        return;

    if (userEnabled && gpuActive)
    {
        // 先记下这一帧的绘制范围与 DPI，再编译候选。
        // 顺序很重要：checkCompilation 成功后内部会立刻 applyUniforms，
        // 若此时 paintArea/paintScale 还是上一帧（首帧为空）的值，
        // iResolution/iOffset 会用过期数据，表现为首帧黑屏或构图错一拍。
        paintArea  = area;
        paintScale = g.getInternalContext().getPhysicalPixelScaleFactor();

        if (paintScale <= 0.0f)
            paintScale = 1.0f;

        // 待验证的候选：借这次绘图的上下文编译一次。成功则升级为"当前着色器"，
        // 失败则丢弃候选、保留上一个可用版本，并把编译日志留给设计器显示。
        if (candidateShader != nullptr)
        {
            const auto result = candidateShader->checkCompilation (g.getInternalContext());

            if (result.wasOk())
            {
                activeShader = std::move (candidateShader);
                lastError.clear();
            }
            else
            {
                lastError = result.getErrorMessage();
                candidateShader.reset();
            }
        }

        if (activeShader != nullptr)
        {
            activeShader->fillRect (g.getInternalContext(), area);
            return;
        }
    }

    paintCpuFallback (g, area, fallbackColour);
}

void ShaderBackground::paintCpuFallback (juce::Graphics& g, juce::Rectangle<int> area,
                                        juce::Colour fallbackColour)
{
    const auto& s = skin::SkinBundle::getInstance().getCurrent();

    // 1) 皮肤给了兜底图就用它（静态，最省 CPU）
    if (fallbackImagePath.isNotEmpty())
    {
        if (const auto image = s.imageFor (fallbackImagePath); ! image.isNull())
        {
            g.drawImage (image, area.toFloat(), juce::RectanglePlacement::stretchToFit);
            return;
        }
    }

    // 2) 否则画参数响应的渐变：随 Decay 变深、随湿声电平"呼吸"。
    //    刻意不随 time 动——GPU 都不可用了就别再烧 CPU。
    const float level = juce::jlimit (0.0f, 1.0f, frameState.level);
    const float decay = juce::jlimit (0.0f, 1.0f, frameState.decay);

    const auto top    = fallbackColour.brighter (0.06f + 0.30f * decay);
    const auto bottom = fallbackColour.darker  (0.12f + 0.40f * decay);

    g.setGradientFill ({ top,    static_cast<float> (area.getX()),
                                static_cast<float> (area.getY()),
                         bottom, static_cast<float> (area.getX()),
                                static_cast<float> (area.getBottom()), false });
    g.fillRect (area);

    // 直接注入的源码（HUD）没有对应皮肤：降级只留黑底渐变，不借用皮肤的强调色。
    if (level > 0.001f && ! directSource)
    {
        const auto centre = area.getCentre().toFloat();
        const auto radius = static_cast<float> (area.getWidth()) * (0.20f + 0.35f * level);

        g.setGradientFill ({ s.accentColour.withAlpha (kFallbackGlowAlpha * level),
                             centre.x, centre.y,
                             s.accentColour.withAlpha (0.0f),
                             centre.x + radius, centre.y, true });
        g.fillRect (area);
    }
}

void ShaderBackground::setUserEnabled (bool shouldBeEnabled)
{
    if (userEnabled == shouldBeEnabled)
        return;

    userEnabled = shouldBeEnabled;
    writeEnabledToSettings (userEnabled);
}

bool ShaderBackground::readEnabledFromSettings()
{
    const auto value = skin::readSkinSetting ("shaderEnabled");
    return value.isEmpty() || value != "0";   // 缺省开启
}

void ShaderBackground::writeEnabledToSettings (bool shouldBeEnabled)
{
    skin::writeSkinSetting ("shaderEnabled", shouldBeEnabled ? "1" : "0");
}
