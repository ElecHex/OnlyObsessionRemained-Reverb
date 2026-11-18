#include "ui/SkinBundle.h"

#include "SkinBinaryData.h"
#include "ui/UiText.h"

namespace skin
{
    juce::String stripJsonComments (const juce::String& text)
    {
        juce::String out;
        out.preallocateBytes (text.getNumBytesAsUTF8() + 8);

        bool inString = false;

        for (int i = 0; i < text.length(); ++i)
        {
            const auto c = text[i];

            if (inString)
            {
                out << c;

                if (c == '\\')
                {
                    if (i + 1 < text.length())
                        out << text[++i];
                }
                else if (c == '"')
                {
                    inString = false;
                }

                continue;
            }

            if (c == '"')
            {
                inString = true;
                out << c;
                continue;
            }

            if (c == '/' && i + 1 < text.length() && text[i + 1] == '/')
            {
                while (i < text.length() && text[i] != '\n')
                    ++i;

                out << '\n';
                continue;
            }

            out << c;
        }

        return out;
    }

    namespace
    {
        // ---------------------------------------------------------------
        // 取值工具
        // ---------------------------------------------------------------

        /** 安全地取对象属性（属性不存在时返回 fallback，不创建空节点）。 */
        juce::var propertyOr (const juce::var& node, const juce::String& key, const juce::var& fallback)
        {
            if (auto* obj = node.getDynamicObject())
                if (obj->hasProperty (key))
                    return obj->getProperty (key);

            return fallback;
        }

        juce::String stringOr (const juce::var& node, const juce::String& key, const juce::String& fallback)
        {
            const auto v = propertyOr (node, key, juce::var());
            return v.isVoid() ? fallback : v.toString();
        }

        /** 读一个 `[x, y]` 形式的点；缺项或不是数组时返回 fallback。 */
        juce::Point<float> pointOr (const juce::var& node, const juce::String& key,
                                    juce::Point<float> fallback)
        {
            if (auto* array = propertyOr (node, key, juce::var()).getArray();
                array != nullptr && array->size() >= 2)
                return { static_cast<float> ((*array)[0].toString().getDoubleValue()),
                         static_cast<float> ((*array)[1].toString().getDoubleValue()) };

            return fallback;
        }

        /**
            贴图槽位的取值：老写法是路径字符串，新写法是 `{ "path": ..., 变换字段... }`。
            两种都返回路径，好让所有调用处只关心"有没有贴图"。
        */
        juce::String assetPathFromNode (const juce::var& node)
        {
            if (node.isString())
                return node.toString();

            return stringOr (node, "path", {});
        }

        /** 把 "#RRGGBB" / "#AARRGGBB" 解析成颜色；不是颜色字面量时返回透明黑并置 ok=false。 */
        juce::Colour parseColourLiteral (const juce::String& text, bool& ok)
        {
            ok = false;

            auto hex = text.trim();

            if (! hex.startsWithChar ('#'))
                return juce::Colours::transparentBlack;

            hex = hex.substring (1);

            if (hex.length() != 6 && hex.length() != 8)
                return juce::Colours::transparentBlack;

            juce::uint32 value = 0;

            for (auto c : hex)
            {
                const int digit = juce::CharacterFunctions::getHexDigitValue (c);

                if (digit < 0)
                    return juce::Colours::transparentBlack;

                value = (value << 4) | static_cast<juce::uint32> (digit);
            }

            ok = true;
            return hex.length() == 6 ? juce::Colour (0xff000000u | value) : juce::Colour (value);
        }

        // ---------------------------------------------------------------
        // 资源来源
        // ---------------------------------------------------------------

        /** 构建期打包进二进制的那份皮肤。 */
        class BuiltInAssetSource final : public AssetSource
        {
        public:
            bool contains (const juce::String& relativePath) const override
            {
                for (int i = 0; i < skin_binary::numFiles; ++i)
                    if (relativePath == skin_binary::files[i].path)
                        return true;

                return false;
            }

            juce::MemoryBlock read (const juce::String& relativePath) const override
            {
                for (int i = 0; i < skin_binary::numFiles; ++i)
                    if (relativePath == skin_binary::files[i].path)
                        return { skin_binary::files[i].data,
                                 static_cast<size_t> (skin_binary::files[i].size) };

                return {};
            }

            juce::String description() const override { return U8 ("内置皮肤"); }
        };

        /** 磁盘上的皮肤目录（美术迭代用，可热加载）。 */
        class FolderAssetSource final : public AssetSource
        {
        public:
            explicit FolderAssetSource (juce::File folder) : root (std::move (folder)) {}

            bool contains (const juce::String& relativePath) const override
            {
                return root.getChildFile (relativePath).existsAsFile();
            }

            juce::MemoryBlock read (const juce::String& relativePath) const override
            {
                juce::MemoryBlock block;
                root.getChildFile (relativePath).loadFileAsData (block);
                return block;
            }

            juce::String description() const override { return U8 ("外部皮肤：") + root.getFullPathName(); }

        private:
            juce::File root;
        };

        // ---------------------------------------------------------------
        // 皮肤设置（纯文本 key=value，放在用户配置目录）
        //
        // 为什么不用 juce::PropertiesFile：它默认写二进制格式（magic number +
        // 以 NUL 结尾的字符串），文件既不可读也不可手改。皮肤路径是美术要来回切换的
        // 东西，一条人能看懂的 skinFolder=... 比二进制存储合适得多。
        // ---------------------------------------------------------------

        juce::File skinSettingsFile()
        {
            // 注意：不能和 Standalone 自己的设置文件同名——JUCE 的独立运行外壳会把
            // 窗口位置/音频设备写进 <用户配置>/OnlyObsessionRemained/OnlyObsessionRemained.settings（XML），
            // 用同一个文件名会被它覆盖掉。
            return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                       .getChildFile ("OnlyObsessionRemained")
                       .getChildFile ("skin.settings");
        }

        juce::String readSettingFromDisk (const juce::String& key)
        {
            const auto file = skinSettingsFile();

            if (! file.existsAsFile())
                return {};

            for (const auto& line : juce::StringArray::fromLines (file.loadFileAsString()))
            {
                const auto trimmed = line.trim();

                if (trimmed.startsWithChar ('#') || ! trimmed.containsChar ('='))
                    continue;

                if (trimmed.upToFirstOccurrenceOf ("=", false, false).trim() == key)
                    return trimmed.fromFirstOccurrenceOf ("=", false, false).trim();
            }

            return {};
        }

        /** 写设置；value 为空表示删除这一条。 */
        void writeSettingToDisk (const juce::String& key, const juce::String& value)
        {
            const auto file = skinSettingsFile();
            file.getParentDirectory().createDirectory();

            juce::StringArray lines;
            bool replaced = false;

            if (file.existsAsFile())
            {
                for (auto& line : juce::StringArray::fromLines (file.loadFileAsString()))
                {
                    const auto trimmed = line.trim();

                    if (! trimmed.startsWithChar ('#') && trimmed.containsChar ('='))
                    {
                        if (trimmed.upToFirstOccurrenceOf ("=", false, false).trim() == key)
                        {
                            if (value.isNotEmpty())
                            {
                                lines.add (key + "=" + value);
                                replaced = true;
                            }

                            continue;   // 值为空 = 删除这条
                        }
                    }

                    lines.add (line);
                }
            }

            if (! replaced && value.isNotEmpty())
                lines.add (key + "=" + value);

            file.replaceWithText (lines.joinIntoString ("\n") + "\n");
        }
    }

    // ===================================================================
    // Transform（元素级 / 槽位级 / 图层级共用）
    // ===================================================================

    bool Transform::isIdentity() const noexcept
    {
        return rotation == 0.0f
            && offset.x == 0.0f && offset.y == 0.0f
            && scale.x  == 1.0f && scale.y  == 1.0f;
    }

    juce::AffineTransform Transform::toAffine (juce::Rectangle<float> rect) const
    {
        const auto anchor = juce::Point<float> (rect.getX() + pivot.x * rect.getWidth(),
                                                rect.getY() + pivot.y * rect.getHeight());

        // followedBy 的含义是"先左后右"，所以这条链读起来就是它真正做的顺序：
        // 把锚点移到原点 → 缩放 → 旋转 → 把锚点放回去并叠加平移。
        juce::AffineTransform t;
        t = t.followedBy (juce::AffineTransform::translation (-anchor.x, -anchor.y));
        t = t.followedBy (juce::AffineTransform::scale (scale.x, scale.y));
        t = t.followedBy (juce::AffineTransform::rotation (juce::degreesToRadians (rotation)));
        t = t.followedBy (juce::AffineTransform::translation (anchor.x + offset.x,
                                                              anchor.y + offset.y));
        return t;
    }

    juce::RectanglePlacement Transform::placement (bool stretchFallback) const
    {
        const auto mode = fit.isNotEmpty() ? fit : (stretchFallback ? "stretch" : "contain");

        if (mode == "stretch") return juce::RectanglePlacement::stretchToFit;
        if (mode == "native")  return juce::RectanglePlacement (juce::RectanglePlacement::centred
                                                                | juce::RectanglePlacement::doNotResize);
        if (mode == "cover")   return juce::RectanglePlacement (juce::RectanglePlacement::centred
                                                                | juce::RectanglePlacement::fillDestination);

        return juce::RectanglePlacement::centred;   // contain（等比放入并居中）
    }

    Transform parseTransform (const juce::var& node)
    {
        Transform t;

        if (! node.isObject())
            return t;

        const auto* obj = node.getDynamicObject();

        // 逐键判断"写了没"，而不是先取默认值再覆盖：这样缺项才真的保持默认，
        // 也不会把外层（类型默认值）已经设好的值冲掉。
        if (obj->hasProperty ("rotation"))
            t.rotation = static_cast<float> (obj->getProperty ("rotation").toString().getDoubleValue());

        t.pivot  = pointOr (node, "pivot",  t.pivot);
        t.offset = pointOr (node, "offset", t.offset);
        t.scale  = pointOr (node, "scale",  t.scale);
        t.fit    = stringOr (node, "fit",   t.fit);

        return t;
    }

    Transform elementTransformFrom (const juce::var& typeDefaultNode, const juce::var& elementNode)
    {
        auto t = parseTransform (typeDefaultNode);
        const auto own = parseTransform (elementNode);

        // 逐键判断"元素写了没"：只有写了才覆盖类型默认值
        if (const auto* obj = elementNode.getDynamicObject())
        {
            if (obj->hasProperty ("rotation")) t.rotation = own.rotation;
            if (obj->hasProperty ("pivot"))    t.pivot    = own.pivot;
            if (obj->hasProperty ("offset"))   t.offset   = own.offset;
            if (obj->hasProperty ("scale"))    t.scale    = own.scale;
            if (obj->hasProperty ("fit"))      t.fit      = own.fit;
        }

        return t;
    }

    // ===================================================================
    // Travel（运动轨迹）
    // ===================================================================

    bool Travel::isDefault() const noexcept
    {
        return ! enabled && hasDefaultPoints();
    }

    bool Travel::hasDefaultPoints() const noexcept
    {
        return p0 == juce::Point<float> (0.5f, 1.0f)
            && p1 == juce::Point<float> (0.5f, 0.667f)
            && p2 == juce::Point<float> (0.5f, 0.333f)
            && p3 == juce::Point<float> (0.5f, 0.0f);
    }

    juce::Point<float> Travel::pointAt (float t) const
    {
        const float u  = 1.0f - t;
        const float w0 = u * u * u;
        const float w1 = 3.0f * u * u * t;
        const float w2 = 3.0f * u * t * t;
        const float w3 = t * t * t;

        return { w0 * p0.x + w1 * p1.x + w2 * p2.x + w3 * p3.x,
                 w0 * p0.y + w1 * p1.y + w2 * p2.y + w3 * p3.y };
    }

    juce::Point<float> Travel::pointIn (juce::Rectangle<float> area, float t) const
    {
        const auto p = pointAt (t);
        return { area.getX() + p.x * area.getWidth(),
                 area.getY() + p.y * area.getHeight() };
    }

    juce::Point<float> Travel::tangentAt (float t) const
    {
        const float u = 1.0f - t;

        const auto d = (p1 - p0) * (3.0f * u * u)
                     + (p2 - p1) * (6.0f * u * t)
                     + (p3 - p2) * (3.0f * t * t);

        // 控制点重合时导数会退化成 0；给一个朝上的方向，调用方不必再判零。
        if (d.getDistanceFromOrigin() < 1.0e-6f)
            return { 0.0f, -1.0f };

        return d;
    }

    juce::Path Travel::pathIn (juce::Rectangle<float> area, float t0, float t1) const
    {
        juce::Path path;
        path.startNewSubPath (pointIn (area, t0));

        if (t0 <= 0.0f && t1 >= 1.0f)
        {
            // 归一化坐标到矩形只差一个仿射映射，整段可以直接用三次贝塞尔表达。
            const auto toArea = [&area] (juce::Point<float> p)
            {
                return juce::Point<float> (area.getX() + p.x * area.getWidth(),
                                           area.getY() + p.y * area.getHeight());
            };

            path.cubicTo (toArea (p1), toArea (p2), toArea (p3));
        }
        else
        {
            // 局部区间：采样足够密，编辑器与运行时的取点逻辑仍然完全一致。
            constexpr int steps = 48;

            for (int i = 1; i <= steps; ++i)
                path.lineTo (pointIn (area, t0 + (t1 - t0) * static_cast<float> (i) / steps));
        }

        return path;
    }

    Travel parseTravel (const juce::var& node)
    {
        Travel t;

        if (! node.isObject())
            return t;

        auto* obj = node.getDynamicObject();

        // 约定：写了 travel 对象就默认启用，除非明确写 enabled: false。
        t.enabled = obj->hasProperty ("enabled")
                        ? static_cast<bool> (obj->getProperty ("enabled"))
                        : true;

        t.p0 = pointOr (node, "p0", t.p0);
        t.p1 = pointOr (node, "p1", t.p1);
        t.p2 = pointOr (node, "p2", t.p2);
        t.p3 = pointOr (node, "p3", t.p3);

        return t;
    }

    Travel travelFrom (const juce::var& typeDefaultNode, const juce::var& elementNode)
    {
        auto t = parseTravel (propertyOr (typeDefaultNode, "travel", juce::var()));

        const auto own = propertyOr (elementNode, "travel", juce::var());

        if (auto* obj = own.getDynamicObject())
        {
            // 元素自己写了 travel：缺 enabled 时按"写下即启用"处理。
            if (obj->hasProperty ("enabled"))
                t.enabled = static_cast<bool> (obj->getProperty ("enabled"));
            else
                t.enabled = true;

            t.p0 = pointOr (own, "p0", t.p0);
            t.p1 = pointOr (own, "p1", t.p1);
            t.p2 = pointOr (own, "p2", t.p2);
            t.p3 = pointOr (own, "p3", t.p3);
        }

        return t;
    }

    Travel defaultTravelForStyle (const juce::String& style)
    {
        Travel t;

        const auto s = style.trim().toLowerCase();

        if (s == "hfader" || s == "horizontal" || s == "hbar")
        {
            t.p0 = { 0.0f, 0.5f };
            t.p1 = { 1.0f / 3.0f, 0.5f };
            t.p2 = { 2.0f / 3.0f, 0.5f };
            t.p3 = { 1.0f, 0.5f };
        }

        return t;
    }

    // ===================================================================
    // Skin
    // ===================================================================

    std::unique_ptr<Skin> Skin::create (const juce::String& manifestText,
                                        std::shared_ptr<AssetSource> source,
                                        juce::String& error)
    {
        error.clear();

        juce::var manifest;

        if (juce::JSON::parse (stripJsonComments (manifestText), manifest).failed())
        {
            error = U8 ("skin.json 不是合法的 JSON（先用构建日志里的 skin_pack 报错定位行号）");
            return nullptr;
        }

        if (! manifest.isObject())
        {
            error = U8 ("skin.json 顶层必须是一个对象");
            return nullptr;
        }

        auto s = std::unique_ptr<Skin> (new Skin());
        s->assets = std::move (source);
        s->manifestText = manifestText;

        s->name = stringOr (manifest, "name", "default");

        // ---- 画布 ----
        const auto canvas = propertyOr (manifest, "canvas", juce::var());

        s->canvasWidth  = static_cast<int> (propertyOr (canvas, "width",  s->canvasWidth).toString().getIntValue());
        s->canvasHeight = static_cast<int> (propertyOr (canvas, "height", s->canvasHeight).toString().getIntValue());

        if (s->canvasWidth < 1 || s->canvasHeight < 1)
        {
            error = U8 ("canvas 尺寸不合理（至少要 1×1）");
            return nullptr;
        }

        s->colorsNode   = propertyOr (manifest, "colors",   juce::var());
        s->fontsNode    = propertyOr (manifest, "fonts",    juce::var());
        s->defaultsNode = propertyOr (manifest, "defaults", juce::var());

        // ---- 颜色集（缺项保持内置默认）----
        auto colourFromSet = [&] (const juce::String& key, juce::Colour fallback)
        {
            return s->colourFor (propertyOr (s->colorsNode, key, juce::var()), fallback);
        };

        s->backgroundColour = colourFromSet ("background", s->backgroundColour);
        s->outlineColour    = colourFromSet ("outline",    s->outlineColour);
        s->textColour       = colourFromSet ("text",       s->textColour);
        s->accentColour     = colourFromSet ("accent",     s->accentColour);
        s->textBoxColour    = colourFromSet ("textBox",    s->textBoxColour);
        s->disabledColour   = colourFromSet ("disabled",   s->disabledColour);

        // ---- 字体集 ----
        s->titleFont = s->fontFromNode (propertyOr (s->fontsNode, "title", juce::var()), s->titleFont);
        s->labelFont = s->fontFromNode (propertyOr (s->fontsNode, "label", juce::var()), s->labelFont);
        s->valueFont = s->fontFromNode (propertyOr (s->fontsNode, "value", juce::var()), s->valueFont);

        // ---- 背景 ----
        const auto background = propertyOr (manifest, "background", juce::var());

        s->backgroundImagePath = stringOr (background, "image", {});
        s->backgroundColour    = s->colourFor (propertyOr (background, "colour", juce::var()),
                                               s->backgroundColour);

        const auto outline = propertyOr (background, "outline", juce::var());

        if (outline.isObject())
        {
            s->backgroundOutlineColour = s->colourFor (propertyOr (outline, "colour", juce::var()),
                                                       s->outlineColour);
            s->backgroundOutlineInset  = static_cast<int> (propertyOr (outline, "inset",
                                                                      s->backgroundOutlineInset)
                                                              .toString().getIntValue());
            s->backgroundOutlineCorner = static_cast<int> (propertyOr (outline, "cornerRadius",
                                                                      s->backgroundOutlineCorner)
                                                              .toString().getIntValue());
            s->backgroundOutlineThickness = static_cast<float> (propertyOr (outline, "thickness",
                                                                           s->backgroundOutlineThickness)
                                                                   .toString().getDoubleValue());
        }
        else if (! outline.isVoid())
        {
            s->backgroundOutlineThickness = 0.0f;   // 写了 outline 但不是对象 = 明确不要脸框
        }

        // ---- 3b / 3c 预留字段 ----
        const auto shader = propertyOr (manifest, "shader", juce::var());
        s->shaderSourcePath    = stringOr (shader, "source", {});
        s->shaderFallbackImage = stringOr (shader, "fallbackImage", {});
        s->shaderParamsNode    = propertyOr (shader, "params", juce::var());

        const auto shape = propertyOr (manifest, "shape", juce::var());
        s->shapeMode     = stringOr (shape, "mode", "auto");
        s->shapeMaskPath = stringOr (shape, "mask", {});

        // ---- 窗口（3e）：出血 / 无标题栏 / 拖动区 ----
        // 画布尺寸的下限校验改成对**窗口尺寸**做：出血把画布撑大之后，
        // 真正要交给 JUCE 的是窗口尺寸，拿画布去校验会放过 0 尺寸的窗口。
        const auto window = propertyOr (manifest, "window", juce::var());

        const auto intsFromArray = [] (const juce::var& node)
        {
            juce::Array<int> values;

            if (const auto* a = node.getArray(); a != nullptr)
                for (const auto& item : *a)
                    values.add (static_cast<int> (item.toString().getIntValue()));

            return values;
        };

        if (const auto expand = intsFromArray (propertyOr (window, "expand", juce::var()));
            expand.size() >= 4)
        {
            // 出血是像素数，负值没有意义（窗口只会变大），上限按屏幕量级兜住手滑
            s->bleedLeft   = juce::jlimit (0, 4000, expand[0]);
            s->bleedTop    = juce::jlimit (0, 4000, expand[1]);
            s->bleedRight  = juce::jlimit (0, 4000, expand[2]);
            s->bleedBottom = juce::jlimit (0, 4000, expand[3]);
        }

        s->frameless = static_cast<bool> (propertyOr (window, "frameless", juce::var (false)));

        {
            const auto dragArea = propertyOr (window, "dragArea", juce::var ("auto"));

            if (dragArea.isArray())
            {
                const auto values = intsFromArray (dragArea);

                if (values.size() >= 4)
                {
                    s->dragAreaKind = "rect";
                    s->dragAreaRect = { values[0], values[1], values[2], values[3] };
                }
            }
            else
            {
                const auto text = dragArea.toString();
                s->dragAreaKind = text.isEmpty() ? "auto" : text;
            }
        }

        if (s->windowWidth() < 200 || s->windowHeight() < 120)
        {
            error = U8 ("窗口尺寸不合理（画布 + 出血至少 200×120）");
            return nullptr;
        }

        // ---- 元素 ----
        if (auto* list = propertyOr (manifest, "elements", juce::var()).getArray())
        {
            for (const auto& node : *list)
            {
                Element e;
                e.id   = stringOr (node, "id", {});
                e.type = stringOr (node, "type", {});
                e.json = node;

                if (const auto rect = propertyOr (node, "rect", juce::var()); rect.isArray())
                {
                    if (auto* r = rect.getArray(); r != nullptr && r->size() >= 4)
                    {
                        e.rect = { static_cast<int> ((*r)[0]), static_cast<int> ((*r)[1]),
                                   static_cast<int> ((*r)[2]), static_cast<int> ((*r)[3]) };
                        e.hasRect = true;
                    }
                }

                if (e.id.isEmpty())
                {
                    error = U8 ("elements 里有一项缺少 id");
                    return nullptr;
                }

                // ---- 控件图层 ----
                // rect 是归一化坐标（相对控件矩形），因此控件移动/缩放/旋转时图层自动跟随。
                if (auto* layerList = propertyOr (node, "layers", juce::var()).getArray())
                {
                    for (const auto& layerNode : *layerList)
                    {
                        ControlLayer layer;
                        layer.id        = stringOr (layerNode, "id", {});
                        layer.name      = stringOr (layerNode, "name", {});
                        layer.imagePath = stringOr (layerNode, "image", {});
                        layer.visible   = static_cast<bool> (propertyOr (layerNode, "visible",
                                                                         juce::var (true)));
                        layer.opacity   = static_cast<float> (propertyOr (layerNode, "opacity",
                                                                          juce::var (1.0))
                                                                  .toString().getDoubleValue());
                        layer.transform = parseTransform (layerNode);

                        if (const auto rect = propertyOr (layerNode, "rect", juce::var()); rect.isArray())
                        {
                            if (auto* r = rect.getArray(); r != nullptr && r->size() >= 4)
                            {
                                layer.rect = { static_cast<float> ((*r)[0].toString().getDoubleValue()),
                                               static_cast<float> ((*r)[1].toString().getDoubleValue()),
                                               static_cast<float> ((*r)[2].toString().getDoubleValue()),
                                               static_cast<float> ((*r)[3].toString().getDoubleValue()) };
                                layer.hasRect = true;
                            }
                        }

                        // 缺 id 时补一个稳定的默认名：设计器/运行时都靠它定位图层
                        if (layer.id.isEmpty())
                            layer.id = "clayer-" + juce::String (static_cast<int> (e.layers.size()) + 1);

                        e.layers.push_back (layer);
                    }
                }

                s->elements.push_back (e);
            }
        }

        // ---- 装饰图层 ----
        if (auto* list = propertyOr (manifest, "layers", juce::var()).getArray())
        {
            for (const auto& node : *list)
            {
                Layer l;
                l.id        = stringOr (node, "id", {});
                l.name      = stringOr (node, "name", {});
                l.imagePath = stringOr (node, "image", {});
                l.visible   = static_cast<bool> (propertyOr (node, "visible", juce::var (true)));
                l.opacity   = static_cast<float> (propertyOr (node, "opacity", juce::var (1.0))
                                                      .toString().getDoubleValue());
                l.transform = parseTransform (node);

                if (const auto rect = propertyOr (node, "rect", juce::var()); rect.isArray())
                {
                    if (auto* r = rect.getArray(); r != nullptr && r->size() >= 4)
                    {
                        l.rect = { static_cast<int> ((*r)[0]), static_cast<int> ((*r)[1]),
                                   static_cast<int> ((*r)[2]), static_cast<int> ((*r)[3]) };
                        l.hasRect = true;
                    }
                }

                // 没有贴图的图层也保留：设计器要能选中/排序/稍后再补图，
                // 运行时绘制空路径自然不会画出内容。
                s->layers.push_back (l);
            }
        }

        return s;
    }

    juce::Font Skin::fontFromNode (const juce::var& node, juce::Font fallback) const
    {
        if (node.isVoid())
            return fallback;

        // 字符串 = 引用 fonts 集里的另一个键
        if (node.isString())
        {
            if (auto* obj = fontsNode.getDynamicObject())
            {
                const auto key = node.toString();

                if (obj->hasProperty (key))
                    return fontFromNode (obj->getProperty (key), fallback);
            }

            return fallback;
        }

        if (! node.isObject())
            return fallback;

        const auto size = propertyOr (node, "size", juce::var());

        if (! size.isVoid())
        {
            juce::FontOptions options (static_cast<float> (size.toString().getDoubleValue()));

            if (static_cast<bool> (propertyOr (node, "bold", false)))
                options = options.withStyle ("Bold");

            const auto family = stringOr (node, "family", {});

            if (family.isNotEmpty())
                options = options.withName (family);

            return juce::Font (options);
        }

        return fallback;
    }

    const Element* Skin::findElement (const juce::String& id) const noexcept
    {
        for (const auto& e : elements)
            if (e.id == id)
                return &e;

        return nullptr;
    }

    juce::Rectangle<int> Skin::rectFor (const juce::String& id, juce::Rectangle<int> fallback) const
    {
        if (const auto* e = findElement (id); e != nullptr && e->hasRect)
            return e->rect;

        return fallback;
    }

    juce::String Skin::textFor (const juce::String& id, const juce::String& fallback) const
    {
        return stringFor (id, "text", fallback);
    }

    juce::String Skin::stringFor (const juce::String& id, const juce::String& key,
                                  const juce::String& fallback) const
    {
        if (const auto* e = findElement (id))
            return stringOr (e->json, key, fallback);

        return fallback;
    }

    juce::var Skin::paramFor (const juce::String& id, const juce::String& paramName, const juce::var& fallback) const
    {
        const auto* e = findElement (id);

        if (e == nullptr)
            return fallback;

        const auto own = propertyOr (e->json, "params", juce::var());
        const auto v   = propertyOr (own, paramName, juce::var());

        if (! v.isVoid())
            return v;

        const auto typeNode = propertyOr (defaultsNode, e->type, juce::var());
        const auto defaults = propertyOr (typeNode, "params", juce::var());

        return propertyOr (defaults, paramName, fallback);
    }

    juce::String Skin::assetFor (const juce::String& id, const juce::String& slot) const
    {
        const auto* e = findElement (id);

        if (e == nullptr)
            return {};

        const auto own = propertyOr (propertyOr (e->json, "assets", juce::var()), slot, juce::var());

        if (! own.isVoid())
            return assetPathFromNode (own);

        return assetForType (e->type, slot);
    }

    juce::String Skin::assetForType (const juce::String& type, const juce::String& slot) const
    {
        if (type.isEmpty())
            return {};

        const auto typeNode  = propertyOr (defaultsNode, type, juce::var());
        const auto assetNode = propertyOr (typeNode, "assets", juce::var());

        return assetPathFromNode (propertyOr (assetNode, slot, juce::var()));
    }

    Transform Skin::elementTransformFor (const juce::String& id) const
    {
        const auto* e = findElement (id);

        if (e == nullptr)
            return {};

        return elementTransformFrom (propertyOr (defaultsNode, e->type, juce::var()), e->json);
    }

    Travel Skin::travelFor (const juce::String& id) const
    {
        const auto* e = findElement (id);

        if (e == nullptr)
            return {};

        auto t = travelFrom (propertyOr (defaultsNode, e->type, juce::var()), e->json);

        // 只写了 enabled、没写控制点时，默认直线要跟控件形态一致：
        // 水平推子用横线，否则推子头会跳到竖直两端。
        if (t.hasDefaultPoints())
        {
            const auto style = paramFor (id, "style", e->type == "knob" ? juce::var ("rotary")
                                                                       : juce::var()).toString();
            const auto def = defaultTravelForStyle (style);

            t.p0 = def.p0;
            t.p1 = def.p1;
            t.p2 = def.p2;
            t.p3 = def.p3;
        }

        return t;
    }

    Travel Skin::travelForPart (const juce::String& id, const juce::String& part) const
    {
        auto t = travelFor (id);

        const auto* e = findElement (id);

        if (e == nullptr || part.isEmpty())
            return t;

        // parts.<槽位> 逐键覆盖父级曲线；没写 enabled 的部位继承父级的启用状态。
        auto applyPart = [&t, &part] (const juce::var& travelNode)
        {
            const auto partNode = propertyOr (propertyOr (travelNode, "parts", juce::var()),
                                              part, juce::var());

            auto* obj = partNode.getDynamicObject();

            if (obj == nullptr)
                return;

            if (obj->hasProperty ("enabled"))
            {
                t.enabled = static_cast<bool> (obj->getProperty ("enabled"));
                t.partDisabled = ! t.enabled;
            }

            t.p0 = pointOr (partNode, "p0", t.p0);
            t.p1 = pointOr (partNode, "p1", t.p1);
            t.p2 = pointOr (partNode, "p2", t.p2);
            t.p3 = pointOr (partNode, "p3", t.p3);
        };

        applyPart (propertyOr (propertyOr (defaultsNode, e->type, juce::var()),
                               "travel", juce::var()));
        applyPart (propertyOr (e->json, "travel", juce::var()));

        return t;
    }

    Transform Skin::slotTransformFor (const juce::String& id, const juce::String& slot) const
    {
        const auto* e = findElement (id);

        if (e == nullptr)
            return {};

        const auto own = propertyOr (propertyOr (e->json, "assets", juce::var()), slot, juce::var());

        if (own.isObject())
            return parseTransform (own);

        const auto typeAsset = propertyOr (propertyOr (propertyOr (defaultsNode, e->type, juce::var()),
                                                        "assets", juce::var()),
                                           slot, juce::var());

        return typeAsset.isObject() ? parseTransform (typeAsset) : Transform();
    }

    Transform Skin::transformForSlot (const juce::String& id, const juce::String& slot) const
    {
        auto t = slotTransformFor (id, slot);

        // 元素级的 fit 只是"这类贴图默认怎么摆"；旋转/缩放/平移属于控件本体，不在这里合并
        if (t.fit.isEmpty())
            t.fit = elementTransformFor (id).fit;

        return t;
    }

    juce::Font Skin::fontFor (const juce::String& id, juce::Font fallback) const
    {
        return fontFromNode (fontNodeFor (id), fallback);
    }

    juce::var Skin::fontNodeFor (const juce::String& id) const
    {
        if (const auto* e = findElement (id))
        {
            if (const auto own = propertyOr (e->json, "font", juce::var()); ! own.isVoid())
                return own;

            return defaultForType (e->type, "font", juce::var());
        }

        return {};
    }

    juce::var Skin::defaultForType (const juce::String& type, const juce::String& key,
                                    const juce::var& fallback) const
    {
        if (type.isEmpty())
            return fallback;

        return propertyOr (propertyOr (defaultsNode, type, juce::var()), key, fallback);
    }

    juce::Colour Skin::elementColourFor (const juce::String& id, juce::Colour fallback) const
    {
        const auto* e = findElement (id);

        if (e == nullptr)
            return fallback;

        if (const auto own = propertyOr (e->json, "colour", juce::var()); ! own.isVoid())
            return colourFor (own, fallback);

        const auto typeNode = propertyOr (defaultsNode, e->type, juce::var());

        return colourFor (propertyOr (typeNode, "colour", juce::var()), fallback);
    }

    juce::Colour Skin::colourFor (const juce::var& value, juce::Colour fallback) const
    {
        if (value.isVoid())
            return fallback;

        if (value.isString())
        {
            bool ok = false;
            const auto literal = parseColourLiteral (value.toString(), ok);

            if (ok)
                return literal;

            // 不是字面量 → 当作颜色集的键
            const auto entry = propertyOr (colorsNode, value.toString(), juce::var());

            if (! entry.isVoid())
                return colourFor (entry, fallback);

            return fallback;
        }

        if (value.isObject())
        {
            // 允许 { "colour": "accent" } 这种嵌套写法（background.outline 用）
            return colourFor (propertyOr (value, "colour", juce::var()), fallback);
        }

        return fallback;
    }

    juce::String Skin::readAssetText (const juce::String& assetPath) const
    {
        if (assetPath.isEmpty() || assets == nullptr || ! assets->contains (assetPath))
            return {};

        const auto data = assets->read (assetPath);

        if (data.getSize() <= 0)
            return {};

        return juce::String::fromUTF8 (static_cast<const char*> (data.getData()),
                                       static_cast<int> (data.getSize()));
    }

    juce::var Skin::shaderParam (const juce::String& name, const juce::var& fallback) const
    {
        return propertyOr (shaderParamsNode, name, fallback);
    }

    juce::Image Skin::imageFor (const juce::String& assetPath) const
    {
        if (assetPath.isEmpty() || assets == nullptr)
            return {};

        // 高分屏（Windows 125%/150% 缩放）优先用同名的 @2x 版本，避免贴图被拉糊。
        // 约定：assets/knob.png 的高分版叫 assets/knob@2x.png；没有就照常缩放。
        juce::String resolved = assetPath;

        if (juce::Desktop::getInstance().getGlobalScaleFactor() > 1.4f)
        {
            const auto dot   = assetPath.lastIndexOfChar ('.');
            const auto hiRes = dot > 0 ? assetPath.substring (0, dot) + "@2x"
                                             + assetPath.substring (dot)
                                       : assetPath + "@2x";

            if (assets->contains (hiRes))
                resolved = hiRes;
        }

        if (const auto it = imageCache.find (resolved); it != imageCache.end())
            return it->second;

        juce::Image image;

        if (assets->contains (resolved))
        {
            const auto data = assets->read (resolved);

            if (data.getSize() > 0)
            {
                image = juce::ImageFileFormat::loadFrom (data.getData(), data.getSize());

                // SVG 不是位图，ImageFileFormat 读不了：走 Drawable 解析再光栅化。
                // 缓存机制与位图共用，所以每帧绘制不会重复解析。
                if (image.isNull() && resolved.endsWithIgnoreCase (".svg"))
                    if (auto drawable = juce::Drawable::createFromImageData (data.getData(),
                                                                            data.getSize()))
                    {
                        const auto w = static_cast<float> (drawable->getWidth());
                        const auto h = static_cast<float> (drawable->getHeight());
                        const auto scale = juce::jlimit (1.0f, 4.0f,
                                                         2048.0f / juce::jmax (1.0f, juce::jmax (w, h)));

                        image = juce::Image (juce::Image::ARGB,
                                             juce::roundToInt (w * scale),
                                             juce::roundToInt (h * scale), true);
                        juce::Graphics g (image);
                        drawable->draw (g, 1.0f, juce::AffineTransform::scale (scale));
                    }
            }
        }

        imageCache[resolved] = image;   // 失败也缓存，避免每帧重复读盘
        return image;
    }

    juce::Image Skin::shapeMask() const
    {
        return shapeMaskPath.isEmpty() ? juce::Image() : imageFor (shapeMaskPath);
    }

    juce::String Skin::describe() const
    {
        return name + U8 ("（") + juce::String (canvasWidth) + U8 ("×") + juce::String (canvasHeight)
             + U8 ("，") + juce::String (static_cast<int> (elements.size())) + U8 (" 个元素）");
    }

    // ===================================================================
    // SkinBundle
    // ===================================================================

    SkinBundle& SkinBundle::getInstance()
    {
        static SkinBundle instance;
        return instance;
    }

    SkinBundle::SkinBundle()
    {
        currentAssets = makeBuiltInSource();

        juce::String error;

        if (auto builtIn = makeSkin (currentAssets, error); builtIn != nullptr)
            current = std::move (builtIn);
        else
            status = U8 ("内置皮肤不可用：") + error;

        restoreLastUsed();
    }

    std::shared_ptr<AssetSource> SkinBundle::makeBuiltInSource()
    {
        return std::make_shared<BuiltInAssetSource>();
    }

    std::unique_ptr<Skin> SkinBundle::makeSkin (std::shared_ptr<AssetSource> assets, juce::String& error)
    {
        if (assets == nullptr || ! assets->contains ("skin.json"))
        {
            error = U8 ("皮肤目录里没有 skin.json");
            return nullptr;
        }

        const auto data = assets->read ("skin.json");

        return Skin::create (juce::String::fromUTF8 (static_cast<const char*> (data.getData()),
                                                      static_cast<int> (data.getSize())),
                             std::move (assets), error);
    }

    void SkinBundle::setCurrent (std::unique_ptr<Skin> newSkin, std::shared_ptr<AssetSource> newAssets,
                                const juce::String& newExternalFolder, const juce::String& newStatus)
    {
        current        = std::move (newSkin);
        currentAssets  = std::move (newAssets);
        externalFolder = newExternalFolder;
        status         = newStatus;

        sendChangeMessage();
    }

    juce::String SkinBundle::getStatus() const
    {
        return status;
    }

    juce::String SkinBundle::getExternalFolder() const
    {
        return externalFolder;
    }

    bool SkinBundle::loadExternalFolder (const juce::File& folder, juce::String& error)
    {
        error.clear();

        if (! folder.isDirectory())
        {
            error = U8 ("目录不存在：") + folder.getFullPathName();
            return false;
        }

        auto assets = std::make_shared<FolderAssetSource> (folder);
        auto loaded = makeSkin (assets, error);

        if (loaded == nullptr)
            return false;   // 保持当前皮肤不动

        const auto description = loaded->describe();

        setCurrent (std::move (loaded), std::move (assets), folder.getFullPathName(),
                    U8 ("已加载外部皮肤：") + description);

        writeSettingToDisk ("skinFolder", folder.getFullPathName());

        return true;
    }

    void SkinBundle::loadBuiltIn()
    {
        juce::String error;
        auto assets = makeBuiltInSource();
        auto loaded = makeSkin (assets, error);

        if (loaded == nullptr)
        {
            status = U8 ("内置皮肤不可用：") + error;
            return;
        }

        const auto description = loaded->describe();

        setCurrent (std::move (loaded), std::move (assets), {}, U8 ("已回到内置皮肤：") + description);

        writeSettingToDisk ("skinFolder", {});
    }

    bool SkinBundle::reload (juce::String& error)
    {
        if (externalFolder.isEmpty())
        {
            error = U8 ("当前是内置皮肤；按 Ctrl+Shift+O 先选一个外部皮肤目录");
            return false;
        }

        return loadExternalFolder (juce::File (externalFolder), error);
    }

    void SkinBundle::restoreLastUsed()
    {
        const auto folder = readSettingFromDisk ("skinFolder");

        if (folder.isEmpty())
            return;

        juce::String error;

        if (! loadExternalFolder (juce::File (folder), error))
            status = U8 ("外部皮肤加载失败，已退回内置皮肤：") + error;
    }

    // ---- 皮肤设置：对外只有这两个入口；实现与文件规则都在匿名命名空间里 ----
    juce::String readSkinSetting (const juce::String& key)
    {
        return readSettingFromDisk (key);
    }

    void writeSkinSetting (const juce::String& key, const juce::String& value)
    {
        writeSettingToDisk (key, value);
    }
}