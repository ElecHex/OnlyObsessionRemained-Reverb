#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <map>
#include <memory>
#include <vector>

/**
    皮肤系统（阶段三 3a）。

    设计要点：

    1. 清单（skin.json）是"外观的唯一真相"：颜色、字体、每个元素的矩形与贴图槽位。
       代码里的硬编码布局只作为**缺失时的回退**，因此清单可以是部分的——你只写想改的
       那几个元素，其余保持内置外观。

    2. 贴图槽位缺失 → 自动回退到内置矢量画法。默认皮肤一张 PNG 都不需要，
       也可以一次只替换一个元素。

    3. 两种来源：
        - 内置：构建期由 tools/skin_pack.cpp 打包成 SkinBinaryData（见 docs/skin-guide.md）
        - 外部：运行时直接读磁盘上的皮肤目录（美术迭代用，改完贴图按 Ctrl+Shift+R 即生效）

    4. 加载失败（清单语法错、目录不存在）**永远不破坏当前皮肤**：报错字符串交给界面提示，
       继续用上一份可用的皮肤。界面绝不能因为皮肤坏掉而变白屏。

    （3b 的动态 shader、3d 的图层与变换、3e 的异形窗口字段都在清单里，见 Skin 的对应成员。
     3e 的两个字段目前只解析不生效。）
*/
namespace skin
{
    /** 剥掉清单里的 // 行注释（字符串内部不受影响）。设计器也要用同一套规则。 */
    juce::String stripJsonComments (const juce::String& text);

    /**
        皮肤设置（纯文本 key=value，放在用户配置目录）。
        用途是用户级偏置——与本皮肤无关的东西，例如动态着色器的总开关。
    */
    juce::String readSkinSetting (const juce::String& key);
    void         writeSkinSetting (const juce::String& key, const juce::String& value);

    /** 资源来源：内置（二进制）或外部目录。 */
    class AssetSource
    {
    public:
        virtual ~AssetSource() = default;

        virtual bool contains (const juce::String& relativePath) const = 0;
        virtual juce::MemoryBlock read (const juce::String& relativePath) const = 0;
        virtual juce::String description() const = 0;
    };

    /**
        变换：旋转 / 平移 / 缩放 + 一个缩放模式。默认值即恒等，所以"不写就等于没有"。

        键名（可以直接写在元素对象、`assets.<槽位>` 的对象形式、或 `layers[]` 的一项上）：

        - `rotation` 旋转角（度，顺时针为正）
        - `pivot`    锚点（`[x, y]`，相对目标矩形的归一化坐标，默认 `[0.5, 0.5]`）
        - `offset`   平移（`[dx, dy]` 像素，默认 `[0, 0]`）
        - `scale`    缩放（`[sx, sy]`，默认 `[1, 1]`）
        - `fit`      缩放模式：`stretch` 拉伸 / `contain` 等比放入 / `cover` 等比铺满 / `native` 原始尺寸

        锚点为什么用归一化坐标：它要跟着矩形走，写像素的话改一次尺寸就得重算。
    */
    struct Transform
    {
        float rotation = 0.0f;
        juce::Point<float> pivot  { 0.5f, 0.5f };
        juce::Point<float> offset { 0.0f, 0.0f };
        juce::Point<float> scale  { 1.0f, 1.0f };
        juce::String fit;

        /** 旋转 / 平移 / 缩放都是默认值时为真（`fit` 不参与：它只是摆放方式）。 */
        bool isIdentity() const noexcept;

        /** 围绕锚点的仿射变换；rect 是目标矩形，锚点按它换算成像素。 */
        juce::AffineTransform toAffine (juce::Rectangle<float> rect) const;

        /** 缩放模式 → JUCE 的贴图摆放方式；`fit` 为空时用 stretchFallback。 */
        juce::RectanglePlacement placement (bool stretchFallback) const;
    };

    /** 从一个节点读变换字段；不是对象或缺项都保持默认值。 */
    Transform parseTransform (const juce::var& node);

    /** 元素级变换：以 typeDefaultNode（`defaults.<type>`）为底，elementNode 写了的键覆盖上去。 */
    Transform elementTransformFrom (const juce::var& typeDefaultNode, const juce::var& elementNode);

    /**
        运动轨迹：一个三次贝塞尔（P0…P3），坐标是**控件自身矩形的归一化坐标**（0…1）。

        用途是让"会动的部位"沿这条曲线走：旋钮的贴图主体、叠加装饰、推子头。
        编辑器和运行时共用同一套 pointAt()，因此设计器里画出来的线就是运行时真正走的路径。

        清单写法（元素级，或 `defaults.<type>` 里作为默认值）：

            "travel": {
                "enabled": true,
                "p0": [0.5, 1.0], "p1": [0.5, 0.667],
                "p2": [0.5, 0.333], "p3": [0.5, 0.0],
                // 可选：单独给某个部位换一条路径（缺项继承上面的曲线）
                "parts": { "overlay": { "p0": [0.2, 0.5], ... } }
            }

        约定：写了 `travel` 对象就默认启用；`parts.<slot>.enabled = false` 可让某个部位
        保持原来的固定位置（例如主体走弧线、数值框不跟着动）。
    */
    struct Travel
    {
        bool enabled = false;

        /**
            该部位在 `parts.<slot>` 里显式写了 `enabled: false`。

            用途：轨道配了曲线、推子头没单独配时，推子头应当跟着轨道走；
            但如果推子头被明确关掉（例如想让头固定在直线上），就不能再回落。
        */
        bool partDisabled = false;

        juce::Point<float> p0 { 0.5f, 1.0f }, p1 { 0.5f, 0.667f },
                           p2 { 0.5f, 0.333f }, p3 { 0.5f, 0.0f };

        /** 未启用且控制点就是默认直线。 */
        bool isDefault() const noexcept;

        /** 控制点正好是内置的默认竖直直线（与 enabled 无关）。 */
        bool hasDefaultPoints() const noexcept;

        /** 三次贝塞尔上的点（t = 0…1），返回归一化坐标。 */
        juce::Point<float> pointAt (float t) const;

        /** 把归一化轨迹换算到某个矩形里。 */
        juce::Point<float> pointIn (juce::Rectangle<float> area, float t) const;

        /** 切线方向（未归一化）；退化时返回 (0, -1)。 */
        juce::Point<float> tangentAt (float t) const;

        /** 轨迹线路径（可只取 t0…t1 一段，供动画/裁剪用）。 */
        juce::Path pathIn (juce::Rectangle<float> area, float t0 = 0.0f, float t1 = 1.0f) const;
    };

    /** 读一个 travel 节点；不是对象时返回默认值。 */
    Travel parseTravel (const juce::var& node);

    /** 元素级轨迹：以 typeDefaultNode 的 travel 为底，elementNode 的 travel 逐键覆盖。 */
    Travel travelFrom (const juce::var& typeDefaultNode, const juce::var& elementNode);

    /**
        该 style 的零轨迹直线方向：水平形态（hfader / horizontal / hbar）是横线，其余是竖线。

        用途：清单只写了 `travel.enabled` 而没写控制点时，默认直线必须与控件形态一致，
        否则水平推子启用轨迹后推子头会跳到竖直两端。
    */
    Travel defaultTravelForStyle (const juce::String& style);

    /**
        控件图层（`elements[].layers[]` 里的一项）：叠在**某个控件之上**的一张图。

        与画布级 Layer 的区别：rect 是归一化坐标（相对控件矩形 0…1），因此控件移动、
        缩放、旋转时图层天然跟着走；顺序按数组从前到后绘制（靠后 = 更靠上）。

        清单写法：

            "elements": [
                { "id": "mix", "type": "knob",
                  "layers": [
                      { "id": "clayer-1", "name": "外圈", "image": "assets/ring.png",
                        "rect": [0, 0, 1, 1], "opacity": 0.8, "rotation": 0 }
                  ] }
            ]
    */
    struct ControlLayer
    {
        juce::String id;
        juce::String name;
        juce::String imagePath;
        bool         visible = true;
        float        opacity = 1.0f;

        /** 归一化矩形 [x, y, w, h]，相对所属控件的本地矩形。缺省铺满控件。 */
        juce::Rectangle<float> rect { 0.0f, 0.0f, 1.0f, 1.0f };
        bool         hasRect = true;

        Transform    transform;
    };

    /** 清单里的一个元素（控件）。json 保留原始节点，供具体控件读取自定义字段。 */
    struct Element
    {
        juce::String id;
        juce::String type;
        bool         hasRect = false;
        juce::Rectangle<int> rect;
        juce::var    json;

        /** 叠加在该控件之上的图层（按数组顺序绘制，靠后 = 更靠上）。 */
        std::vector<ControlLayer> layers;
    };

    /**
        纯装饰图层（清单顶层 `layers` 数组里的一项）：一张贴图 + 自己的矩形 + 自己的变换。

        与**贴图槽位**的分工：槽位挂在某个控件上、随控件状态变化；图层不属于任何控件，
        按数组顺序叠在背景之上、控件之下。
    */
    struct Layer
    {
        juce::String id;
        juce::String name;           // 面板里的图层名（空 = 用 id 或序号显示）
        juce::String imagePath;      // 空 = 这一层没有贴图（可见性与顺序仍然保留）
        bool         visible = true; // 关掉 = 不绘制，但图层仍留在列表里供编辑
        bool         hasRect = false;
        juce::Rectangle<int> rect;
        float        opacity = 1.0f; // 0…1
        Transform    transform;
    };

    /** 一张解析好的皮肤。加载失败时不要构造它——用 Skin::create() 的返回值判断。 */
    class Skin
    {
    public:
        /** 从清单文本构造；失败返回 nullptr 并填充 error（含行号/原因）。 */
        static std::unique_ptr<Skin> create (const juce::String& manifestText,
                                             std::shared_ptr<AssetSource> assets,
                                             juce::String& error);

        // ---- 画布 ----
        juce::String name = "default";
        int canvasWidth  = 720;
        int canvasHeight = 392;

        // ---- 颜色集 ----
        // 缺省值取 docs/ui-style-guide.md 的 HUD 调色板：皮肤 colors 里没写的键落到这里。
        // 前四个仍然各有职责：background = 窗口底、text = 文字、accent = 背景兜底辉光/状态文字、
        // disabled = 占位符与禁用文字；outline / textBox 现在只作为旧清单的兼容名存在
        // （控件画面改走 HUD 语义 token，见 SkinnedLookAndFeel::hudColour）。
        juce::Colour backgroundColour { 0xff0b0914 };
        juce::Colour outlineColour    { 0xff6e6a88 };
        juce::Colour textColour       { 0xffd8d4e6 };
        juce::Colour accentColour     { 0xffffffff };
        juce::Colour textBoxColour    { 0xff050408 };
        juce::Colour disabledColour   { 0xff8a86a0 };

        // ---- 字体 ----
        juce::Font titleFont { juce::FontOptions (16.0f, juce::Font::bold) };
        juce::Font labelFont { juce::FontOptions (13.0f) };
        juce::Font valueFont { juce::FontOptions (12.0f) };

        // ---- 背景 ----
        juce::String backgroundImagePath;
        juce::Colour backgroundOutlineColour { 0xff2c2c2e };
        int   backgroundOutlineInset     = 6;
        int   backgroundOutlineCorner    = 8;
        float backgroundOutlineThickness = 1.5f;

        // ---- 动态着色器（3b / 3c）与异形窗口（3e）----
        // 动态着色器与它的兜底图都是"皮肤内的相对资源路径"——源码文本在读取时按需取，
        // 避免把可能的数万字着色器常驻内存。
        juce::String shaderSourcePath, shaderFallbackImage;
        juce::var    shaderParamsNode;
        juce::String shapeMode = "auto", shapeMaskPath;

        // ---- 窗口（3e）----
        // 出血：窗口比画布大出来的四边像素。**设计坐标系统不变** —— 清单里的 rect 始终相对
        // 画布左上角，编辑器本地坐标 = 设计坐标 + (bleedLeft, bleedTop)。
        int bleedLeft = 0, bleedTop = 0, bleedRight = 0, bleedBottom = 0;

        /** 仅 Standalone 生效：隐掉标题栏（真异形的观感前提；代价是标题栏上的音频设置入口没了，
            编辑器会补一个右键菜单 + Ctrl+Shift+A 顶上）。 */
        bool frameless = false;

        /** 无标题栏时按住哪里可以拖窗口："auto" = 背景任意处；"rect" = 用 dragAreaRect。 */
        juce::String dragAreaKind = "auto";
        juce::Rectangle<int> dragAreaRect;

        /** 窗口尺寸 = 画布 + 四边出血。 */
        int windowWidth()  const noexcept { return canvasWidth  + bleedLeft + bleedRight; }
        int windowHeight() const noexcept { return canvasHeight + bleedTop  + bleedBottom; }

        /** 画布在窗口里的位置（编辑器本地坐标）。 */
        juce::Rectangle<int> canvasRect() const noexcept
        {
            return { bleedLeft, bleedTop, canvasWidth, canvasHeight };
        }

        /** 有没有异形蒙版（写了 shape.mask 才算，mode 只是决定要不要真裁窗口）。 */
        bool hasShapeMask() const noexcept { return shapeMaskPath.isNotEmpty(); }

        // ---- 元素 ----
        std::vector<Element> elements;

        // ---- 装饰图层 ----
        std::vector<Layer> layers;

        const Element* findElement (const juce::String& id) const noexcept;

        /** 元素矩形；清单里没写（或整份清单缺这个元素）时返回 fallback。 */
        juce::Rectangle<int> rectFor (const juce::String& id, juce::Rectangle<int> fallback) const;

        /** 元素文本（如标签文案、按钮文案）；缺省返回 fallback。 */
        juce::String textFor (const juce::String& id, const juce::String& fallback) const;

        /** 取元素的任意字符串字段（如 placeholder / justification）。 */
        juce::String stringFor (const juce::String& id, const juce::String& key,
                                const juce::String& fallback) const;

        /**
            取贴图槽位：先查元素自身的 assets.<slot>，再查 defaults.<type>.assets.<slot>。
            返回空字符串表示"没有贴图，请用内置矢量画法"。
            `assets.<slot>` 既可以是路径字符串，也可以是 `{ "path": ..., rotation / pivot / ... }`。
        */
        juce::String assetFor (const juce::String& id, const juce::String& slot) const;
        juce::String assetForType (const juce::String& type, const juce::String& slot) const;

        /** 元素级变换：元素自身写了的键优先，其次 defaults.<type> 上的同名键。 */
        Transform elementTransformFor (const juce::String& id) const;

        /** 元素级运动轨迹；`defaults.<type>.travel` 作为底，元素自己的 travel 覆盖上去。 */
        Travel travelFor (const juce::String& id) const;

        /**
            某个部位（贴图槽位）实际使用的轨迹：在 travelFor() 的基础上再套
            `travel.parts.<part>`。运行时与设计器都必须走这一个函数，才能保证所见即所得。
        */
        Travel travelForPart (const juce::String& id, const juce::String& part) const;

        /** 槽位级变换：只认 `assets.<slot>` 的对象形式；写字符串（老写法）时是恒等。 */
        Transform slotTransformFor (const juce::String& id, const juce::String& slot) const;

        /**
            画某个槽位时该用的变换：**只有槽位自己那一份**（`fit` 缺省时回落到元素级）。

            为什么不再把元素级变换合并进来：元素级的旋转/缩放/平移是套在**控件本体**上的
            （编辑器走 Component::setTransform，见 applyElementTransforms），贴图绘制在控件
            自己的坐标系里，本来就跟着转了。再合并一次就会转两遍。
        */
        Transform transformForSlot (const juce::String& id, const juce::String& slot) const;

        /** 取数值/混合参数（先元素 params.<name>，再 defaults.<type>.params.<name>）。 */
        juce::var paramFor (const juce::String& id, const juce::String& paramName,
                            const juce::var& fallback) const;

        /** 取某个元素/类型指定的字体名（fonts 集里的键），缺省返回 fallback 字体。 */
        juce::Font fontFor (const juce::String& id, juce::Font fallback) const;

        /** 元素/类型显式指定的字体节点；未指定返回 void（用于"没写就沿用 JUCE 默认"）。 */
        juce::var fontNodeFor (const juce::String& id) const;

        /** 把字体节点解析成 Font（接受 fonts 集里的键名或 { size, bold, family }）。 */
        juce::Font fontFromNode (const juce::var& node, juce::Font fallback) const;

        /** 取 defaults.<type>.<key>，缺省返回 fallback。 */
        juce::var defaultForType (const juce::String& type, const juce::String& key,
                                  const juce::var& fallback) const;

        /** 取元素（或该类型的默认）颜色；缺省返回 fallback。 */
        juce::Colour elementColourFor (const juce::String& id, juce::Colour fallback) const;

        /** 按颜色名（颜色集键）或 #RRGGBB / #AARRGGBB 解析；失败返回 fallback。 */
        juce::Colour colourFor (const juce::var& value, juce::Colour fallback) const;

        /** 贴图（带缓存；路径为空或解码失败返回空图，调用方应回退到矢量画法）。 */
        juce::Image imageFor (const juce::String& assetPath) const;

        /** 把皮肤内的文本资源（着色器源码等）按 UTF-8 读出；缺失返回空串。 */
        juce::String readAssetText (const juce::String& assetPath) const;

        /** 取 `shader.params` 下的一个值（帧率等）；缺省返回 fallback。 */
        juce::var shaderParam (const juce::String& name, const juce::var& fallback) const;

        /** 相对画布的 alpha 蒙版（3c 用）；无蒙版返回空图。 */
        juce::Image shapeMask() const;

        /** 校验：把画布尺寸与环境不匹配之类的问题变成可读信息（供状态栏显示）。 */
        juce::String describe() const;

        /** 清单原文（设计器做"另存为可编辑副本"时用）。 */
        const juce::String& getManifestText() const noexcept { return manifestText; }

    private:
        Skin() = default;

        std::shared_ptr<AssetSource> assets;
        mutable std::map<juce::String, juce::Image> imageCache;   // 解码缓存（绘制期按需填充）

        juce::String manifestText;
        juce::var colorsNode, fontsNode, defaultsNode;
    };

    /** 皮肤仓库：加载（内置/外部）、热加载、变更通知。 */
    class SkinBundle : public juce::ChangeBroadcaster
    {
    public:
        static SkinBundle& getInstance();

        /** 当前皮肤。启动时若外部皮肤不可用会自动退回内置皮肤，因此永远有效。 */
        const Skin& getCurrent() const noexcept { return *current; }

        /** 最近一次加载结果/错误，供界面提示或排查（空 = 一切正常）。 */
        juce::String getStatus() const;

        /** 最近一次成功加载的外部皮肤目录（空 = 正在用内置皮肤）。 */
        juce::String getExternalFolder() const;

        /** 加载外部皮肤目录；失败返回 false 并填充 error，当前皮肤保持不变。 */
        bool loadExternalFolder (const juce::File& folder, juce::String& error);

        /** 回到内置皮肤。 */
        void loadBuiltIn();

        /** 重新读取当前来源（外部皮肤才有效）。 */
        bool reload (juce::String& error);

        /** 启动时调用：若上次用过外部皮肤且它仍有效，则恢复它。 */
        void restoreLastUsed();

    private:
        SkinBundle();

        /** 切换皮肤；调用方保证 makeSkin 返回非空。 */
        void setCurrent (std::unique_ptr<Skin> newSkin, std::shared_ptr<AssetSource> newAssets,
                         const juce::String& externalFolder, const juce::String& status);

        static std::shared_ptr<AssetSource> makeBuiltInSource();
        static std::unique_ptr<Skin> makeSkin (std::shared_ptr<AssetSource> assets, juce::String& error);

        std::unique_ptr<Skin>        current;
        std::shared_ptr<AssetSource> currentAssets;
        juce::String externalFolder, status;
    };
}