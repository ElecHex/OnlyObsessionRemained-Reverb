#include "ui/SkinDesigner.h"

#include "SkinBinaryData.h"
#include "ui/UiText.h"

#include <algorithm>
#include <cmath>

namespace skin
{
    namespace
    {
        /** 可编辑元素清单（与 docs/skin-guide.md 第 4 节一致）。 */
        struct ElementInfo { const char* id; const char* type; const char* label; };

        const ElementInfo kElements[] =
        {
            { "title",         "label",      "标题" },
            { "presetPrev",    "button",     "预设 <" },
            { "presetNext",    "button",     "预设 >" },
            { "preset",        "combo",      "预设下拉" },
            { "presetName",    "textEditor", "预设名输入框" },
            { "presetSave",    "button",     "保存按钮" },
            { "presetDelete",  "button",     "删除按钮" },
            { "typeLabel",     "label",      "类型标签" },
            { "type",          "combo",      "类型下拉" },
            { "mixLabel",      "label",      "Mix 标签" },
            { "mix",           "knob",       "Mix" },
            { "decayLabel",    "label",      "Decay 标签" },
            { "decay",         "knob",       "Decay" },
            { "preDelayLabel", "label",      "Pre-Delay 标签" },
            { "preDelay",      "knob",       "Pre-Delay" },
            { "modLabel",      "label",      "Mod 标签" },
            { "mod",           "knob",       "Mod" },
            { "highDampLabel", "label",      "High Damp 标签" },
            { "highDamp",      "knob",       "High Damp" },
            { "lowDampLabel",  "label",      "Low Damp 标签" },
            { "lowDamp",       "knob",       "Low Damp" },
            { "shimmerLabel",  "label",      "Shimmer 标签" },
            { "shimmer",       "knob",       "Shimmer" }
        };

        const ElementInfo* findInfo (const juce::String& id)
        {
            for (const auto& info : kElements)
                if (id == info.id)
                    return &info;

            return nullptr;
        }

        constexpr int kNumElements = static_cast<int> (sizeof (kElements) / sizeof (kElements[0]));

        /** 两条轨迹是否画出同一条线（启用状态 + 四个控制点都一致）。 */
        bool travelMatches (const Travel& a, const Travel& b)
        {
            constexpr float eps = 0.0005f;

            auto near = [eps] (juce::Point<float> p, juce::Point<float> q)
            {
                return std::abs (p.x - q.x) <= eps && std::abs (p.y - q.y) <= eps;
            };

            return a.enabled == b.enabled && near (a.p0, b.p0) && near (a.p1, b.p1)
                && near (a.p2, b.p2) && near (a.p3, b.p3);
        }

        juce::StringArray slotsForType (const juce::String& type)
        {
            if (type == "knob")   return { "filmstrip", "track", "trackFill", "thumb", "thumbOver", "thumbDown", "overlay" };
            if (type == "button") return { "normal", "over", "down", "disabled", "toggled" };
            if (type == "combo")  return { "background", "arrow", "arrowDown" };
            if (type == "textEditor") return { "background", "outline" };

            return {};
        }

        juce::StringArray styleChoices()
        {
            return { "rotary", "vfader", "hfader", "vbar", "hbar" };
        }

        juce::StringArray dragModeChoices()
        {
            return { "auto", "vertical", "horizontal", "absolute" };
        }

        juce::StringArray valueBoxChoices()
        {
            return { "below", "above", "left", "right", "none" };
        }

        /** 着色器分区的标题。折叠状态以标题为键，所以建分区和设默认折叠必须用同一份字面量。 */
        const char* const kShaderSectionTitle = "着色器（动态背景）";

        /** 变换分区的标题（同上，折叠状态以它为键）。 */
        const char* const kTransformSectionTitle = "图层与变换";

        /** 运动轨迹分区的标题。 */
        const char* const kTravelSectionTitle = "运动轨迹（贝塞尔）";

        /** 把点过一遍仿射变换（JUCE 只提供"传引用改坐标"的 transformPoint）。 */
        juce::Point<float> applyTransform (const juce::AffineTransform& t, juce::Point<float> p)
        {
            t.transformPoint (p.x, p.y);
            return p;
        }

        /** 把鼠标点换算回"未变换"的坐标系，好让命中判定仍是一次普通的矩形包含测试。 */
        bool hitTestRect (juce::Rectangle<int> rect, const Transform& t, juce::Point<int> position)
        {
            if (t.isIdentity() || t.scale.x == 0.0f || t.scale.y == 0.0f)
                return rect.contains (position);

            const auto back = t.toAffine (rect.toFloat()).inverted();

            return rect.contains (applyTransform (back, position.toFloat()).roundToInt());
        }

        /** 把两个数合成 `[a, b]`：pivot / offset / scale 都是成对字段，改一个要连另一个一起写回。 */
        juce::var pairVar (double first, double second)
        {
            juce::Array<juce::var> values;
            values.add (first);
            values.add (second);
            return juce::var (values);
        }

        /** 缩放模式下拉里的"默认"项 = 清单里不写 fit。 */
        juce::String fitDefaultValue() { return U8 ("（默认）"); }

        /** 缩放模式下拉。第一项是"不写"，其余与清单里的 fit 取值一一对应。 */
        juce::StringArray fitChoices()
        {
            juce::StringArray items;
            items.add (fitDefaultValue());
            items.add ("stretch");
            items.add ("contain");
            items.add ("cover");
            items.add ("native");
            return items;
        }

        /** 预设名会直接当目录名用：去掉路径分隔符与 Windows 非法字符。 */
        juce::String sanitisePresetName (const juce::String& name)
        {
            auto trimmed = name.trim();

            for (auto* invalid : { "/", "\\", ":", "*", "?", "\"", "<", ">", "|" })
                trimmed = trimmed.replace (invalid, "_");

            return trimmed;
        }

        /** 递归复制目录内容；skipTopLevel 里的顶层名字不复制（清单由调用方自己写）。 */
        bool copyDirectoryContents (const juce::File& source, const juce::File& target,
                                    const juce::StringArray& skipTopLevel, juce::String& error)
        {
            target.createDirectory();

            for (const auto& entry : juce::RangedDirectoryIterator (source, true, "*",
                                                                    juce::File::findFilesAndDirectories))
            {
                const auto file = entry.getFile();
                const auto relative = file.getRelativePathFrom (source);

                if (skipTopLevel.contains (relative))
                    continue;

                const auto destination = target.getChildFile (relative);

                if (entry.isDirectory())
                {
                    destination.createDirectory();
                }
                else
                {
                    destination.getParentDirectory().createDirectory();
                    destination.deleteFile();

                    if (! file.copyFileTo (destination))
                    {
                        error = U8 ("复制失败：") + file.getFullPathName();
                        return false;
                    }
                }
            }

            return true;
        }

        /** 加载预设前先清掉清单与资源目录，免得上一套方案的贴图残留下来。 */
        void clearSkinContent (const juce::File& folder)
        {
            folder.getChildFile ("skin.json").deleteFile();
            folder.getChildFile ("assets").deleteRecursively();
            folder.getChildFile ("shaders").deleteRecursively();
        }
    }

    // ===================================================================
    // SkinDocument
    // ===================================================================

    bool SkinDocument::attach (juce::String& error)
    {
        const auto folder = SkinBundle::getInstance().getExternalFolder();

        if (folder.isEmpty())
            return false;   // 内置皮肤：不可编辑，先U8 ("另存为可编辑副本")

        skinFolder = juce::File (folder);

        return readManifestFromDisk (error);
    }

    bool SkinDocument::readManifestFromDisk (juce::String& error)
    {
        const auto file = skinFolder.getChildFile ("skin.json");

        if (! file.existsAsFile())
        {
            error = U8 ("找不到 ") + file.getFullPathName();
            return false;
        }

        manifest = juce::JSON::parse (stripJsonComments (file.loadFileAsString()));

        if (! manifest.isObject())
        {
            error = U8 ("skin.json 解析失败");
            manifest = juce::var();
            return false;
        }

        return true;
    }

    bool SkinDocument::createEditableCopy (juce::String& error)
    {
        error.clear();

        const auto target = juce::File::getSpecialLocation (juce::File::userDocumentsDirectory)
                                .getChildFile ("OnlyObsessionRemained")
                                .getChildFile ("skin-work");

        target.createDirectory();

        const auto file = target.getChildFile ("skin.json");

        // 复制当前皮肤的清单（含手写注释，方便你在设计器里接着用）
        file.replaceWithText (SkinBundle::getInstance().getCurrent().getManifestText());

        if (! SkinBundle::getInstance().loadExternalFolder (target, error))
            return false;

        return attach (error);
    }

    juce::var& SkinDocument::elementNode (const juce::String& elementId, bool createIfMissing)
    {
        static juce::var nullVar;

        if (! manifest.isObject())
            return nullVar;

        auto* elements = manifest.getDynamicObject();

        if (elements == nullptr)
            return nullVar;

        if (! elements->hasProperty ("elements"))
            elements->setProperty ("elements", juce::Array<juce::var>());

        auto& list = elements->getProperty ("elements");

        if (auto* array = list.getArray())
        {
            for (auto& node : *array)
            {
                if (node.getProperty ("id", juce::var()).toString() == elementId)
                    return node;
            }

            if (createIfMissing)
            {
                juce::DynamicObject::Ptr node = new juce::DynamicObject();
                node->setProperty ("id", elementId);

                if (const auto* info = findInfo (elementId))
                    node->setProperty ("type", info->type);

                array->add (juce::var (node.get()));
                return array->getReference (array->size() - 1);
            }
        }

        return nullVar;
    }

    void SkinDocument::setRect (const juce::String& elementId, juce::Rectangle<int> rect)
    {
        auto& node = elementNode (elementId, true);

        if (node.isVoid())
            return;

        juce::Array<juce::var> values;
        values.add (rect.getX());
        values.add (rect.getY());
        values.add (rect.getWidth());
        values.add (rect.getHeight());

        node.getDynamicObject()->setProperty ("rect", values);
    }

    void SkinDocument::setParam (const juce::String& elementId, const juce::String& key, const juce::var& value)
    {
        auto& node = elementNode (elementId, true);

        if (node.getDynamicObject() == nullptr)
            return;

        auto params = node.getProperty ("params", juce::var());

        if (! params.isObject())
        {
            params = juce::var (new juce::DynamicObject());
            node.getDynamicObject()->setProperty ("params", params);
        }

        params.getDynamicObject()->setProperty (key, value);
    }

    void SkinDocument::setText (const juce::String& elementId, const juce::String& text)
    {
        auto& node = elementNode (elementId, true);

        if (node.getDynamicObject() != nullptr)
            node.getDynamicObject()->setProperty ("text", text);
    }

    void SkinDocument::setAsset (const juce::String& elementId, const juce::String& slot,
                                const juce::String& relativePath)
    {
        auto& node = elementNode (elementId, true);

        if (node.getDynamicObject() == nullptr)
            return;

        auto assetsVar = node.getProperty ("assets", juce::var());

        if (! assetsVar.isObject())
        {
            assetsVar = juce::var (new juce::DynamicObject());
            node.getDynamicObject()->setProperty ("assets", assetsVar);
        }

        assetsVar.getDynamicObject()->setProperty (slot, relativePath);
    }

    juce::String SkinDocument::importAsset (const juce::File& source, juce::String& error)
    {
        error.clear();

        if (! skinFolder.isDirectory())
        {
            error = U8 ("当前皮肤不是可编辑的外部目录");
            return {};
        }

        if (! source.existsAsFile())
        {
            error = U8 ("文件不存在：") + source.getFullPathName();
            return {};
        }

        const auto assetsDir = skinFolder.getChildFile ("assets");
        assetsDir.createDirectory();

        const auto target = assetsDir.getChildFile (source.getFileName());
        target.deleteFile();

        if (! source.copyFileTo (target))
        {
            error = U8 ("拷贝失败：") + target.getFullPathName();
            return {};
        }

        return "assets/" + source.getFileName();
    }

    juce::String SkinDocument::importVectorAsset (const juce::String& svgText,
                                                  const juce::String& preferredName,
                                                  juce::String& error)
    {
        error.clear();

        if (! skinFolder.isDirectory())
        {
            error = U8 ("当前皮肤不是可编辑的外部目录");
            return {};
        }

        auto name = preferredName.trim().toLowerCase();

        for (const auto c : juce::String ("\\/:*?\"<>| "))
            name = name.replaceCharacter (c, '-');

        if (name.isEmpty())
            name = "vector";

        const auto assetsDir = skinFolder.getChildFile ("assets");
        assetsDir.createDirectory();

        auto file = assetsDir.getChildFile (name + ".svg");

        for (int i = 2; file.existsAsFile(); ++i)
            file = assetsDir.getChildFile (name + "-" + juce::String (i) + ".svg");

        if (! file.replaceWithText (svgText))
        {
            error = U8 ("写入失败：") + file.getFullPathName();
            return {};
        }

        return "assets/" + file.getFileName();
    }

    // ---- 装饰图层 ----

    /** 清单里的 layers 数组（create = true 时缺失就建一个空数组）。 */
    static juce::Array<juce::var>* layerArray (juce::var& manifest, bool create)
    {
        if (! manifest.isObject())
            return nullptr;

        if (auto* array = manifest.getProperty ("layers", juce::var()).getArray())
            return array;

        if (! create)
            return nullptr;

        manifest.getDynamicObject()->setProperty ("layers", juce::var (juce::Array<juce::var>()));
        return manifest.getProperty ("layers", juce::var()).getArray();
    }

    static int layerIndex (const juce::var& manifest, const juce::String& layerId)
    {
        if (auto* array = manifest.getProperty ("layers", juce::var()).getArray())
            for (int i = 0; i < array->size(); ++i)
                if ((*array)[i].getProperty ("id", juce::var()).toString() == layerId)
                    return i;

        return -1;
    }

    static SkinDocument::LayerInfo layerInfoFrom (const juce::var& node)
    {
        SkinDocument::LayerInfo info;
        info.id        = node.getProperty ("id", juce::var()).toString();
        info.name      = node.getProperty ("name", juce::var()).toString();
        info.imagePath = node.getProperty ("image", juce::var()).toString();
        info.visible   = static_cast<bool> (node.getProperty ("visible", juce::var (true)));
        info.opacity   = static_cast<float> (node.getProperty ("opacity", juce::var (1.0))
                                                 .toString().getDoubleValue());
        info.transform = elementTransformFrom ({}, node);

        if (const auto rect = node.getProperty ("rect", juce::var()); rect.isArray())
            if (auto* r = rect.getArray(); r != nullptr && r->size() >= 4)
            {
                info.rect = { static_cast<int> ((*r)[0]), static_cast<int> ((*r)[1]),
                              static_cast<int> ((*r)[2]), static_cast<int> ((*r)[3]) };
                info.hasRect = true;
            }

        return info;
    }

    std::vector<SkinDocument::LayerInfo> SkinDocument::layers() const
    {
        std::vector<LayerInfo> result;

        if (auto* array = manifest.getProperty ("layers", juce::var()).getArray())
            for (const auto& node : *array)
                result.push_back (layerInfoFrom (node));

        return result;
    }

    SkinDocument::LayerInfo SkinDocument::layerFor (const juce::String& layerId) const
    {
        if (auto* array = manifest.getProperty ("layers", juce::var()).getArray())
            for (const auto& node : *array)
                if (node.getProperty ("id", juce::var()).toString() == layerId)
                    return layerInfoFrom (node);

        return {};
    }

    juce::String SkinDocument::addLayer()
    {
        auto* array = layerArray (manifest, true);

        if (array == nullptr)
            return {};

        // id 不能和控件撞：面板/编辑层都用同一个选择键
        juce::StringArray used;

        if (auto* elements = manifest.getProperty ("elements", juce::var()).getArray())
            for (const auto& node : *elements)
                used.add (node.getProperty ("id", juce::var()).toString());

        for (const auto& node : *array)
            used.add (node.getProperty ("id", juce::var()).toString());

        int n = array->size() + 1;
        juce::String id;

        do { id = "layer-" + juce::String (n++); } while (used.contains (id));

        const int canvasW = static_cast<int> (manifest.getProperty ("canvas", juce::var())
                                                 .getProperty ("width", juce::var (720))
                                                 .toString().getIntValue());
        const int canvasH = static_cast<int> (manifest.getProperty ("canvas", juce::var())
                                                 .getProperty ("height", juce::var (392))
                                                 .toString().getIntValue());

        auto* obj = new juce::DynamicObject();
        obj->setProperty ("id", id);
        obj->setProperty ("name", U8 ("图层 ") + juce::String (array->size() + 1));
        obj->setProperty ("visible", true);
        obj->setProperty ("opacity", 1.0);
        obj->setProperty ("rect", juce::var (juce::Array<juce::var>
                                             { juce::jmax (0, (canvasW - 120) / 2),
                                               juce::jmax (0, (canvasH - 120) / 2), 120, 120 }));

        array->add (juce::var (obj));

        return id;
    }

    void SkinDocument::removeLayer (const juce::String& layerId)
    {
        auto* array = layerArray (manifest, false);

        if (array == nullptr)
            return;

        const int index = layerIndex (manifest, layerId);

        if (index >= 0)
            array->remove (index);
    }

    bool SkinDocument::moveLayer (const juce::String& layerId, int delta)
    {
        auto* array = layerArray (manifest, false);

        if (array == nullptr)
            return false;

        const int index = layerIndex (manifest, layerId);
        const int target = index + delta;

        if (index < 0 || target < 0 || target >= array->size() || delta == 0)
            return false;

        array->swap (index, target);
        return true;
    }

    void SkinDocument::setLayerValue (const juce::String& layerId, const juce::String& key,
                                      const juce::var& value)
    {
        auto* array = layerArray (manifest, false);

        if (array == nullptr)
            return;

        const int index = layerIndex (manifest, layerId);

        if (index < 0)
            return;

        auto& node = (*array).getReference (index);

        if (node.getDynamicObject() == nullptr)
            return;

        if (value.isVoid())
            node.getDynamicObject()->removeProperty (key);
        else
            node.getDynamicObject()->setProperty (key, value);
    }

    bool SkinDocument::writeThrough (juce::String& error)
    {
        error.clear();

        if (! skinFolder.isDirectory())
        {
            error = U8 ("当前皮肤不是可编辑的外部目录");
            return false;
        }

        const auto file = skinFolder.getChildFile ("skin.json");

        // 手写注释无法在改动后保留，所以每次保存前把上一版留一份
        if (file.existsAsFile())
            file.copyFileTo (skinFolder.getChildFile ("skin.json.bak"));

        file.replaceWithText (juce::JSON::toString (manifest));

        return SkinBundle::getInstance().reload (error);
    }

    juce::Rectangle<int> SkinDocument::rectFor (const juce::String& elementId,
                                               juce::Rectangle<int> fallback) const
    {
        if (manifest.isObject())
            if (auto* array = manifest.getProperty ("elements", juce::var()).getArray())
                for (const auto& node : *array)
                    if (node.getProperty ("id", juce::var()).toString() == elementId)
                        if (auto* rect = node.getProperty ("rect", juce::var()).getArray();
                            rect != nullptr && rect->size() >= 4)
                            return { static_cast<int> ((*rect)[0]), static_cast<int> ((*rect)[1]),
                                     static_cast<int> ((*rect)[2]), static_cast<int> ((*rect)[3]) };

        return fallback;
    }

    juce::var SkinDocument::paramFor (const juce::String& elementId, const juce::String& key,
                                      const juce::var& fallback) const
    {
        if (manifest.isObject())
            if (auto* array = manifest.getProperty ("elements", juce::var()).getArray())
                for (const auto& node : *array)
                    if (node.getProperty ("id", juce::var()).toString() == elementId)
                    {
                        const auto params = node.getProperty ("params", juce::var());

                        if (auto* obj = params.getDynamicObject(); obj != nullptr && obj->hasProperty (key))
                            return obj->getProperty (key);
                    }

        return fallback;
    }

    /** 清单里的元素节点；找不到返回 void。 */
    static juce::var findElementNode (const juce::var& manifest, const juce::String& elementId)
    {
        if (auto* array = manifest.getProperty ("elements", juce::var()).getArray())
            for (const auto& node : *array)
                if (node.getProperty ("id", juce::var()).toString() == elementId)
                    return node;

        return {};
    }

    // ---- 控件图层（elements[].layers[]）----

    /** 元素节点里的 layers 数组（create = true 时缺失就建一个空数组）。 */
    static juce::Array<juce::var>* controlLayerArray (juce::var& elementNode, bool create)
    {
        if (elementNode.getDynamicObject() == nullptr)
            return nullptr;

        if (auto* array = elementNode.getProperty ("layers", juce::var()).getArray())
            return array;

        if (! create)
            return nullptr;

        elementNode.getDynamicObject()->setProperty ("layers", juce::var (juce::Array<juce::var>()));
        return elementNode.getProperty ("layers", juce::var()).getArray();
    }

    static int controlLayerIndex (const juce::var& elementNode, const juce::String& layerId)
    {
        if (auto* array = elementNode.getProperty ("layers", juce::var()).getArray())
            for (int i = 0; i < array->size(); ++i)
                if ((*array)[i].getProperty ("id", juce::var()).toString() == layerId)
                    return i;

        return -1;
    }

    static SkinDocument::ControlLayerInfo controlLayerInfoFrom (const juce::var& node)
    {
        SkinDocument::ControlLayerInfo info;
        info.id        = node.getProperty ("id", juce::var()).toString();
        info.name      = node.getProperty ("name", juce::var()).toString();
        info.imagePath = node.getProperty ("image", juce::var()).toString();
        info.visible   = static_cast<bool> (node.getProperty ("visible", juce::var (true)));
        info.opacity   = static_cast<float> (node.getProperty ("opacity", juce::var (1.0))
                                                 .toString().getDoubleValue());
        info.transform = parseTransform (node);

        if (const auto rect = node.getProperty ("rect", juce::var()); rect.isArray())
            if (auto* r = rect.getArray(); r != nullptr && r->size() >= 4)
            {
                info.rect = { static_cast<float> ((*r)[0].toString().getDoubleValue()),
                              static_cast<float> ((*r)[1].toString().getDoubleValue()),
                              static_cast<float> ((*r)[2].toString().getDoubleValue()),
                              static_cast<float> ((*r)[3].toString().getDoubleValue()) };
                info.hasRect = true;
            }

        return info;
    }

    std::vector<SkinDocument::ControlLayerInfo>
        SkinDocument::controlLayers (const juce::String& elementId) const
    {
        std::vector<ControlLayerInfo> result;

        const auto node = findElementNode (manifest, elementId);

        if (auto* array = node.getProperty ("layers", juce::var()).getArray())
            for (const auto& layerNode : *array)
                result.push_back (controlLayerInfoFrom (layerNode));

        return result;
    }

    SkinDocument::ControlLayerInfo SkinDocument::controlLayerFor (const juce::String& elementId,
                                                                  const juce::String& layerId) const
    {
        const auto node = findElementNode (manifest, elementId);

        if (auto* array = node.getProperty ("layers", juce::var()).getArray())
            for (const auto& layerNode : *array)
                if (layerNode.getProperty ("id", juce::var()).toString() == layerId)
                    return controlLayerInfoFrom (layerNode);

        return {};
    }

    juce::String SkinDocument::addControlLayer (const juce::String& elementId)
    {
        auto& node = elementNode (elementId, true);
        auto* array = controlLayerArray (node, true);

        if (array == nullptr)
            return {};

        juce::StringArray used;

        for (const auto& layerNode : *array)
            used.add (layerNode.getProperty ("id", juce::var()).toString());

        int n = array->size() + 1;
        juce::String id;

        do { id = "clayer-" + juce::String (n++); } while (used.contains (id));

        auto* obj = new juce::DynamicObject();
        obj->setProperty ("id", id);
        obj->setProperty ("name", U8 ("图层 ") + juce::String (array->size() + 1));
        obj->setProperty ("visible", true);
        obj->setProperty ("opacity", 1.0);

        // 缺省铺满控件：先看到占位框，再导入图片 / 矢量图形
        obj->setProperty ("rect", juce::var (juce::Array<juce::var> { 0.0, 0.0, 1.0, 1.0 }));

        array->add (juce::var (obj));

        return id;
    }

    void SkinDocument::removeControlLayer (const juce::String& elementId, const juce::String& layerId)
    {
        auto& node = elementNode (elementId, false);
        auto* array = controlLayerArray (node, false);

        if (array == nullptr)
            return;

        const int index = controlLayerIndex (node, layerId);

        if (index >= 0)
            array->remove (index);
    }

    bool SkinDocument::moveControlLayer (const juce::String& elementId, const juce::String& layerId,
                                         int delta)
    {
        auto& node = elementNode (elementId, false);
        auto* array = controlLayerArray (node, false);

        if (array == nullptr)
            return false;

        const int index = controlLayerIndex (node, layerId);
        const int target = index + delta;

        if (index < 0 || target < 0 || target >= array->size() || delta == 0)
            return false;

        array->swap (index, target);
        return true;
    }

    void SkinDocument::setControlLayerValue (const juce::String& elementId, const juce::String& layerId,
                                             const juce::String& key, const juce::var& value)
    {
        auto& node = elementNode (elementId, false);
        auto* array = controlLayerArray (node, false);

        if (array == nullptr)
            return;

        const int index = controlLayerIndex (node, layerId);

        if (index < 0)
            return;

        auto& layerNode = (*array).getReference (index);

        if (layerNode.getDynamicObject() == nullptr)
            return;

        if (value.isVoid())
            layerNode.getDynamicObject()->removeProperty (key);
        else
            layerNode.getDynamicObject()->setProperty (key, value);
    }

    void SkinDocument::setControlLayerRect (const juce::String& elementId, const juce::String& layerId,
                                            juce::Rectangle<float> normalizedRect)
    {
        setControlLayerValue (elementId, layerId, "rect",
                              juce::var (juce::Array<juce::var> { normalizedRect.getX(),
                                                                  normalizedRect.getY(),
                                                                  normalizedRect.getWidth(),
                                                                  normalizedRect.getHeight() }));
    }

    Transform SkinDocument::transformFor (const juce::String& elementId) const
    {
        const auto node = findElementNode (manifest, elementId);

        if (node.isVoid())
            return {};

        const auto type = node.getProperty ("type", juce::var()).toString();
        const auto typeDefault = manifest.getProperty ("defaults", juce::var())
                                     .getProperty (type, juce::var());

        return elementTransformFrom (typeDefault, node);
    }

    void SkinDocument::setTransformValue (const juce::String& elementId, const juce::String& key,
                                          const juce::var& value)
    {
        auto& node = elementNode (elementId, true);

        if (node.getDynamicObject() == nullptr)
            return;

        if (value.isVoid())
            node.getDynamicObject()->removeProperty (key);
        else
            node.getDynamicObject()->setProperty (key, value);
    }

    // ---- 运动轨迹 ----

    Travel SkinDocument::travelFor (const juce::String& elementId) const
    {
        const auto node = findElementNode (manifest, elementId);

        if (node.isVoid())
            return {};

        const auto type = node.getProperty ("type", juce::var()).toString();
        const auto typeDefault = manifest.getProperty ("defaults", juce::var())
                                     .getProperty (type, juce::var());

        auto t = travelFrom (typeDefault, node);

        // 与运行时 Skin::travelFor 保持一致：没写控制点时按形态给默认直线方向。
        if (t.hasDefaultPoints())
        {
            const auto def = defaultTravelForStyle (paramFor (elementId, "style",
                                                              type == "knob" ? juce::var ("rotary")
                                                                             : juce::var())
                                                        .toString());

            t.p0 = def.p0;
            t.p1 = def.p1;
            t.p2 = def.p2;
            t.p3 = def.p3;
        }

        return t;
    }

    Travel SkinDocument::travelForPart (const juce::String& elementId, const juce::String& part) const
    {
        auto t = travelFor (elementId);

        const auto node = findElementNode (manifest, elementId);

        if (node.isVoid() || part.isEmpty())
            return t;

        auto applyPart = [&t, &part] (const juce::var& travelNode)
        {
            const auto partNode = travelNode.getProperty ("parts", juce::var())
                                             .getProperty (part, juce::var());

            auto* obj = partNode.getDynamicObject();

            if (obj == nullptr)
                return;

            if (obj->hasProperty ("enabled"))
            {
                t.enabled = static_cast<bool> (obj->getProperty ("enabled"));
                t.partDisabled = ! t.enabled;
            }

            auto readPoint = [&partNode] (const juce::String& key, juce::Point<float> fallback)
            {
                const auto array = partNode.getProperty (key, juce::var());

                if (auto* a = array.getArray(); a != nullptr && a->size() >= 2)
                    return juce::Point<float> (static_cast<float> ((*a)[0].toString().getDoubleValue()),
                                               static_cast<float> ((*a)[1].toString().getDoubleValue()));

                return fallback;
            };

            t.p0 = readPoint ("p0", t.p0);
            t.p1 = readPoint ("p1", t.p1);
            t.p2 = readPoint ("p2", t.p2);
            t.p3 = readPoint ("p3", t.p3);
        };

        const auto type = node.getProperty ("type", juce::var()).toString();
        const auto typeDefault = manifest.getProperty ("defaults", juce::var())
                                     .getProperty (type, juce::var());

        applyPart (typeDefault.getProperty ("travel", juce::var()));
        applyPart (node.getProperty ("travel", juce::var()));

        return t;
    }

    juce::StringArray SkinDocument::travelPartsFor (const juce::String& elementId) const
    {
        juce::StringArray names;

        auto collect = [&names] (const juce::var& travelNode)
        {
            const auto parts = travelNode.getProperty ("parts", juce::var());

            if (auto* obj = parts.getDynamicObject())
                for (const auto& pair : obj->getProperties())
                    names.addIfNotAlreadyThere (pair.name.toString());
        };

        const auto node = findElementNode (manifest, elementId);

        if (node.isVoid())
            return names;

        const auto type = node.getProperty ("type", juce::var()).toString();
        const auto typeDefault = manifest.getProperty ("defaults", juce::var())
                                     .getProperty (type, juce::var());

        collect (typeDefault.getProperty ("travel", juce::var()));
        collect (node.getProperty ("travel", juce::var()));

        // 运行时真正会套轨迹的部位：即使清单还没写，也让它们出现在面板与编辑层，
        // 否则新皮肤只有「（整体）」一条曲线，看不到"各部位"。
        const auto style = paramFor (elementId, "style",
                                     type == "knob" ? juce::var ("rotary") : juce::var())
                               .toString().trim().toLowerCase();

        if (type == "knob")
        {
            if (style == "vfader" || style == "vertical" || style == "hfader"
                || style == "horizontal" || style == "vbar" || style == "hbar")
            {
                for (const char* part : { "track", "trackFill", "thumb", "thumbOver", "thumbDown", "overlay" })
                    names.addIfNotAlreadyThere (part);
            }
            else
            {
                for (const char* part : { "filmstrip", "overlay" })
                    names.addIfNotAlreadyThere (part);
            }
        }

        names.removeDuplicates (false);

        return names;
    }

    void SkinDocument::setTravelEnabled (const juce::String& elementId, const juce::String& part,
                                         bool enabled)
    {
        auto& node = elementNode (elementId, true);

        if (node.getDynamicObject() == nullptr)
            return;

        auto travel = node.getProperty ("travel", juce::var());

        if (! travel.isObject())
        {
            travel = juce::var (new juce::DynamicObject());
            node.getDynamicObject()->setProperty ("travel", travel);
        }

        auto* travelObj = travel.getDynamicObject();

        if (part.isEmpty())
        {
            travelObj->setProperty ("enabled", enabled);
            return;
        }

        auto parts = travelObj->getProperty ("parts");

        if (! parts.isObject())
        {
            parts = juce::var (new juce::DynamicObject());
            travelObj->setProperty ("parts", parts);
        }

        auto partNode = parts.getProperty (juce::Identifier (part), juce::var());

        if (! partNode.isObject())
        {
            partNode = juce::var (new juce::DynamicObject());
            parts.getDynamicObject()->setProperty (part, partNode);
        }

        partNode.getDynamicObject()->setProperty ("enabled", enabled);
    }

    void SkinDocument::setTravelPoint (const juce::String& elementId, const juce::String& part,
                                       int index, juce::Point<float> point)
    {
        if (index < 0 || index > 3)
            return;

        auto& node = elementNode (elementId, true);

        if (node.getDynamicObject() == nullptr)
            return;

        auto travel = node.getProperty ("travel", juce::var());

        if (! travel.isObject())
        {
            travel = juce::var (new juce::DynamicObject());
            node.getDynamicObject()->setProperty ("travel", travel);
        }

        auto* travelObj = travel.getDynamicObject();

        // 目标对象：元素级就是 travel 自己；部位级是 travel.parts.<part>。
        auto* target = travelObj;

        if (part.isNotEmpty())
        {
            auto parts = travelObj->getProperty ("parts");

            if (! parts.isObject())
            {
                parts = juce::var (new juce::DynamicObject());
                travelObj->setProperty ("parts", parts);
            }

            auto partNode = parts.getProperty (juce::Identifier (part), juce::var());

            if (! partNode.isObject())
            {
                partNode = juce::var (new juce::DynamicObject());
                parts.getDynamicObject()->setProperty (part, partNode);
            }

            target = partNode.getDynamicObject();
        }

        juce::Array<juce::var> values;
        values.add (point.x);
        values.add (point.y);

        target->setProperty ("p" + juce::String (index), juce::var (values));

        // 写点即视为启用：否则拖了半天却看不到运行时变化。
        if (! target->hasProperty ("enabled") && part.isNotEmpty())
            target->setProperty ("enabled", true);
    }

    void SkinDocument::resetTravel (const juce::String& elementId, const juce::String& part)
    {
        auto& node = elementNode (elementId, false);

        auto* obj = node.getDynamicObject();

        if (obj == nullptr)
            return;

        if (part.isEmpty())
        {
            // 元素级：只清掉自己的启用位与控制点，保留各部位的自定义轨迹
            // （否则重置"整体"会把辛苦调好的部位曲线一起删掉）。
            const auto travel = obj->getProperty ("travel");

            if (auto* travelObj = travel.getDynamicObject())
            {
                travelObj->removeProperty ("enabled");
                travelObj->removeProperty ("p0");
                travelObj->removeProperty ("p1");
                travelObj->removeProperty ("p2");
                travelObj->removeProperty ("p3");

                if (travelObj->getProperties().isEmpty())
                    obj->removeProperty ("travel");
            }

            return;
        }

        auto travel = obj->getProperty ("travel");

        auto* travelObj = travel.getDynamicObject();

        if (travelObj == nullptr)
            return;

        auto parts = travelObj->getProperty ("parts");

        if (auto* partsObj = parts.getDynamicObject())
            partsObj->removeProperty (part);
    }

    juce::String SkinDocument::textFor (const juce::String& elementId) const
    {
        const auto node = findElementNode (manifest, elementId);

        return node.isVoid() ? juce::String() : node.getProperty ("text", juce::var()).toString();
    }

    juce::String SkinDocument::assetFor (const juce::String& elementId, const juce::String& slot) const
    {
        if (manifest.isObject())
            if (auto* array = manifest.getProperty ("elements", juce::var()).getArray())
                for (const auto& node : *array)
                    if (node.getProperty ("id", juce::var()).toString() == elementId)
                    {
                        const auto assets = node.getProperty ("assets", juce::var());

                        if (auto* obj = assets.getDynamicObject(); obj != nullptr && obj->hasProperty (slot))
                            return obj->getProperty (slot).toString();
                    }

        return {};
    }

    // ---- 动态着色器（阶段三 3c）----

    juce::DynamicObject* SkinDocument::shaderObject (bool createIfMissing)
    {
        auto* root = manifest.getDynamicObject();

        if (root == nullptr)
            return nullptr;

        if (! root->hasProperty ("shader"))
        {
            if (! createIfMissing)
                return nullptr;

            root->setProperty ("shader", juce::var (new juce::DynamicObject()));
        }

        auto node = root->getProperty ("shader");   // 副本，内部对象是共享的

        // 清单里若把 shader 写成了非对象，按需要替换成对象
        if (! node.isObject() && createIfMissing)
        {
            node = juce::var (new juce::DynamicObject());
            root->setProperty ("shader", node);
        }

        return node.getDynamicObject();
    }

    juce::String SkinDocument::shaderSourcePath() const
    {
        if (! manifest.isObject())
            return {};

        const auto node = manifest.getProperty ("shader", juce::var());

        if (auto* obj = node.getDynamicObject())
            return obj->getProperty ("source").toString();

        return {};
    }

    void SkinDocument::setShaderSourcePath (const juce::String& relativePath)
    {
        if (auto* obj = shaderObject (true); obj != nullptr)
        {
            if (relativePath.isEmpty())
                obj->removeProperty ("source");
            else
                obj->setProperty ("source", relativePath);
        }
    }

    juce::var SkinDocument::shaderParam (const juce::String& name, const juce::var& fallback) const
    {
        if (! manifest.isObject())
            return fallback;

        const auto node = manifest.getProperty ("shader", juce::var());

        if (auto* obj = node.getDynamicObject())
        {
            const auto params = obj->getProperty ("params");

            if (auto* p = params.getDynamicObject(); p != nullptr && p->hasProperty (name))
                return p->getProperty (name);
        }

        return fallback;
    }

    void SkinDocument::setShaderParam (const juce::String& name, const juce::var& value)
    {
        if (auto* obj = shaderObject (true); obj != nullptr)
        {
            auto params = obj->getProperty ("params");

            if (! params.isObject())
            {
                params = juce::var (new juce::DynamicObject());
                obj->setProperty ("params", params);
            }

            params.getDynamicObject()->setProperty (name, value);
        }
    }

    // ---- 窗口与异形（阶段三 3e）----

    juce::DynamicObject* SkinDocument::windowObject (bool createIfMissing)
    {
        auto* root = manifest.getDynamicObject();

        if (root == nullptr)
            return nullptr;

        if (! root->hasProperty ("window"))
        {
            if (! createIfMissing)
                return nullptr;

            root->setProperty ("window", juce::var (new juce::DynamicObject()));
        }

        auto node = root->getProperty ("window");   // 副本，内部对象是共享的

        // 清单里若把 window 写成了非对象，按需要替换成对象
        if (! node.isObject() && createIfMissing)
        {
            node = juce::var (new juce::DynamicObject());
            root->setProperty ("window", node);
        }

        return node.getDynamicObject();
    }

    juce::Rectangle<int> SkinDocument::windowExpand() const
    {
        if (! manifest.isObject())
            return {};

        const auto node = manifest.getProperty ("window", juce::var());

        if (auto* obj = node.getDynamicObject())
        {
            const auto expand = obj->getProperty ("expand");

            if (const auto* a = expand.getArray(); a != nullptr && a->size() >= 4)
                return { static_cast<int> ((*a)[0]), static_cast<int> ((*a)[1]),
                         static_cast<int> ((*a)[2]), static_cast<int> ((*a)[3]) };
        }

        return {};
    }

    void SkinDocument::setWindowExpand (int side, int pixels)
    {
        auto* obj = windowObject (true);

        if (obj == nullptr)
            return;

        auto expand = windowExpand();

        switch (side)
        {
            case 0: expand.setX (pixels); break;
            case 1: expand.setY (pixels); break;
            case 2: expand.setWidth (pixels); break;
            case 3: expand.setHeight (pixels); break;
            default: return;
        }

        obj->setProperty ("expand", juce::var (juce::Array<juce::var> { expand.getX(), expand.getY(),
                                                                        expand.getWidth(), expand.getHeight() }));
    }

    bool SkinDocument::frameless() const
    {
        if (! manifest.isObject())
            return false;

        const auto node = manifest.getProperty ("window", juce::var());

        if (auto* obj = node.getDynamicObject())
            return static_cast<bool> (obj->getProperty ("frameless"));

        return false;
    }

    void SkinDocument::setFrameless (bool shouldBeFrameless)
    {
        if (auto* obj = windowObject (true); obj != nullptr)
            obj->setProperty ("frameless", shouldBeFrameless);
    }

    juce::String SkinDocument::dragAreaKind() const
    {
        if (! manifest.isObject())
            return "auto";

        const auto node = manifest.getProperty ("window", juce::var());

        if (auto* obj = node.getDynamicObject())
        {
            const auto value = obj->getProperty ("dragArea");

            if (value.isArray())
                return "rect";

            const auto text = value.toString();
            return text.isEmpty() ? "auto" : text;
        }

        return "auto";
    }

    juce::Rectangle<int> SkinDocument::dragAreaRect() const
    {
        if (! manifest.isObject())
            return {};

        const auto node = manifest.getProperty ("window", juce::var());

        if (auto* obj = node.getDynamicObject())
        {
            const auto value = obj->getProperty ("dragArea");

            if (const auto* a = value.getArray(); a != nullptr && a->size() >= 4)
                return { static_cast<int> ((*a)[0]), static_cast<int> ((*a)[1]),
                         static_cast<int> ((*a)[2]), static_cast<int> ((*a)[3]) };
        }

        return {};
    }

    void SkinDocument::setDragAreaAuto()
    {
        if (auto* obj = windowObject (true); obj != nullptr)
            obj->setProperty ("dragArea", "auto");
    }

    void SkinDocument::setDragAreaRect (juce::Rectangle<int> rect)
    {
        if (auto* obj = windowObject (true); obj != nullptr)
            obj->setProperty ("dragArea", juce::var (juce::Array<juce::var> {
                                  rect.getX(), rect.getY(), rect.getWidth(), rect.getHeight() }));
    }

    juce::String SkinDocument::shapeMode() const
    {
        if (! manifest.isObject())
            return "auto";

        const auto node = manifest.getProperty ("shape", juce::var());

        if (auto* obj = node.getDynamicObject())
        {
            const auto text = obj->getProperty ("mode").toString();
            return text.isEmpty() ? "auto" : text;
        }

        return "auto";
    }

    void SkinDocument::setShapeMode (const juce::String& mode)
    {
        auto* root = manifest.getDynamicObject();

        if (root == nullptr)
            return;

        auto node = root->getProperty ("shape");

        if (! node.isObject())
        {
            node = juce::var (new juce::DynamicObject());
            root->setProperty ("shape", node);
        }

        node.getDynamicObject()->setProperty ("mode", mode);
    }

    juce::String SkinDocument::shapeMaskPath() const
    {
        if (! manifest.isObject())
            return {};

        const auto node = manifest.getProperty ("shape", juce::var());

        if (auto* obj = node.getDynamicObject())
            return obj->getProperty ("mask").toString();

        return {};
    }

    void SkinDocument::setShapeMaskPath (const juce::String& relativePath)
    {
        auto* root = manifest.getDynamicObject();

        if (root == nullptr)
            return;

        auto node = root->getProperty ("shape");

        if (! node.isObject())
        {
            node = juce::var (new juce::DynamicObject());
            root->setProperty ("shape", node);
        }

        auto* obj = node.getDynamicObject();

        if (relativePath.isEmpty())
            obj->removeProperty ("mask");
        else
            obj->setProperty ("mask", relativePath);
    }

    juce::String SkinDocument::readSkinFile (const juce::String& relativePath) const
    {
        if (relativePath.isEmpty() || ! skinFolder.isDirectory())
            return {};

        const auto file = skinFolder.getChildFile (relativePath);

        return file.existsAsFile() ? file.loadFileAsString() : juce::String();
    }

    bool SkinDocument::writeSkinFile (const juce::String& relativePath, const juce::String& text,
                                      juce::String& error)
    {
        error.clear();

        if (! skinFolder.isDirectory())
        {
            error = U8 ("当前皮肤不是可编辑的外部目录");
            return false;
        }

        if (relativePath.isEmpty())
        {
            error = U8 ("清单里没有指定着色器文件路径");
            return false;
        }

        const auto file = skinFolder.getChildFile (relativePath);

        if (! file.replaceWithText (text))
        {
            error = U8 ("写入失败：") + file.getFullPathName();
            return false;
        }

        return true;
    }

    juce::String SkinDocument::importShader (const juce::File& source, juce::String& error)
    {
        error.clear();

        if (! skinFolder.isDirectory())
        {
            error = U8 ("当前皮肤不是可编辑的外部目录");
            return {};
        }

        if (! source.existsAsFile())
        {
            error = U8 ("文件不存在：") + source.getFullPathName();
            return {};
        }

        const auto dir = skinFolder.getChildFile ("shaders");
        dir.createDirectory();

        const auto target = dir.getChildFile (source.getFileName());
        target.deleteFile();

        if (! source.copyFileTo (target))
        {
            error = U8 ("拷贝失败：") + target.getFullPathName();
            return {};
        }

        return "shaders/" + source.getFileName();
    }

    // ---- 方案预设（整份设计的完整快照）----

    juce::File SkinDocument::presetsFolder()
    {
        return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                   .getChildFile ("OnlyObsessionRemained")
                   .getChildFile ("designer-presets");
    }

    juce::StringArray SkinDocument::listPresets() const
    {
        juce::StringArray names;

        const auto folder = presetsFolder();

        if (! folder.isDirectory())
            return names;

        for (const auto& entry : juce::RangedDirectoryIterator (folder, false, "*",
                                                                juce::File::findDirectories))
            if (entry.getFile().getChildFile ("skin.json").existsAsFile())
                names.add (entry.getFile().getFileName());

        names.sort (true);
        return names;
    }

    bool SkinDocument::savePreset (const juce::String& name, juce::String& error)
    {
        error.clear();

        if (! skinFolder.isDirectory())
        {
            error = U8 ("当前皮肤不是可编辑的外部目录");
            return false;
        }

        const auto clean = sanitisePresetName (name);

        if (clean.isEmpty())
        {
            error = U8 ("预设名不能为空");
            return false;
        }

        const auto target = presetsFolder().getChildFile (clean);

        // 整份覆盖：先删掉旧预设，避免上一次的贴图残留在快照里
        if (target.isDirectory() && ! target.deleteRecursively())
        {
            error = U8 ("无法覆盖旧预设：") + target.getFullPathName();
            return false;
        }

        if (! target.createDirectory())
        {
            error = U8 ("无法创建预设目录：") + target.getFullPathName();
            return false;
        }

        // 清单用内存里的版本：用户可能改了还没保存，预设必须包含这些改动
        if (! target.getChildFile ("skin.json").replaceWithText (juce::JSON::toString (manifest)))
        {
            error = U8 ("写入预设清单失败");
            return false;
        }

        // 其余资源（assets/ shaders/ 以及清单里引用的其它相对路径）整份复制
        if (! copyDirectoryContents (skinFolder, target, { "skin.json", "skin.json.bak" }, error))
            return false;

        return true;
    }

    bool SkinDocument::loadPreset (const juce::String& name, juce::String& error)
    {
        error.clear();

        if (! skinFolder.isDirectory())
        {
            error = U8 ("当前皮肤不是可编辑的外部目录");
            return false;
        }

        const auto source = presetsFolder().getChildFile (sanitisePresetName (name));

        if (! source.getChildFile ("skin.json").existsAsFile())
        {
            error = U8 ("预设不存在：") + name;
            return false;
        }

        clearSkinContent (skinFolder);

        if (! copyDirectoryContents (source, skinFolder, {}, error))
            return false;

        if (! readManifestFromDisk (error))
            return false;

        return SkinBundle::getInstance().reload (error);
    }

    bool SkinDocument::deletePreset (const juce::String& name, juce::String& error)
    {
        error.clear();

        const auto target = presetsFolder().getChildFile (sanitisePresetName (name));

        if (! target.isDirectory())
        {
            error = U8 ("预设不存在：") + name;
            return false;
        }

        if (! target.deleteRecursively())
        {
            error = U8 ("删除失败：") + target.getFullPathName();
            return false;
        }

        return true;
    }

    // ---- 内置预设（编译进二进制的默认布局）----

    juce::String SkinDocument::builtInPresetName()
    {
        return U8 ("默认布局（内置）");
    }

    bool SkinDocument::isBuiltInPreset (const juce::String& name)
    {
        return name == builtInPresetName();
    }

    bool SkinDocument::restoreBuiltIn (juce::String& error)
    {
        error.clear();

        if (! skinFolder.isDirectory())
        {
            error = U8 ("当前皮肤不是可编辑的外部目录");
            return false;
        }

        // 和 loadPreset 一样先清干净：内置那份里没有的资源（用户自己导入的贴图、
        // 着色器）留着的话，清单已经不引用它们了，留着只会让目录越滚越乱
        clearSkinContent (skinFolder);

        // 内置皮肤就是构建期打包进 SkinBinaryData 的那份（skins/<SKIN_NAME>/ 全部文件），
        // 所以这里不需要任何外部文件，逐个路径写回磁盘即可
        for (int i = 0; i < skin_binary::numFiles; ++i)
        {
            const auto file = skinFolder.getChildFile (skin_binary::files[i].path);

            file.getParentDirectory().createDirectory();

            if (! file.replaceWithData (skin_binary::files[i].data,
                                        static_cast<size_t> (skin_binary::files[i].size)))
            {
                error = U8 ("写回内置皮肤失败：") + file.getFullPathName();
                return false;
            }
        }

        if (! readManifestFromDisk (error))
            return false;

        return SkinBundle::getInstance().reload (error);
    }

    // ===================================================================
    // DesignerOverlay
    // ===================================================================

    DesignerOverlay::DesignerOverlay()
    {
        setWantsKeyboardFocus (true);
        setInterceptsMouseClicks (true, false);
    }

    void DesignerOverlay::setItems (std::vector<Item> newItems)
    {
        items = std::move (newItems);
        repaint();
    }

    void DesignerOverlay::setSelected (const juce::String& elementId)
    {
        if (selected == elementId)
            return;

        selected = elementId;
        repaint();
    }

    void DesignerOverlay::setCanvasInfo (juce::Rectangle<int> area, juce::Image maskImage)
    {
        canvasArea     = area;
        shapeMaskImage = std::move (maskImage);
        repaint();
    }

    void DesignerOverlay::setActiveTravelPart (const juce::String& part)
    {
        if (activeTravelPart == part)
            return;

        activeTravelPart = part;
        repaint();
    }

    const DesignerOverlay::Item* DesignerOverlay::findItem (juce::Point<int> position) const
    {
        // 0) 选中控件（或它下面某个图层）的图层虚线框先参与命中：
        //    面板提示“直接点紫色虚线框”必须真的可用，但只认框线那一圈，
        //    铺满控件的图层不会把控件内部的点击和拖动全部抢走。
        //    铺满整个控件的图层（0,0,1,1）不参与：它的框和控件外框完全重合，
        //    否则控件边缘就再也拖不动了，这种图层从面板列表里选。
        {
            const auto owner = selected.upToFirstOccurrenceOf ("/", false, false);

            if (owner.isNotEmpty())
                for (auto it = items.rbegin(); it != items.rend(); ++it)
                {
                    if (! it->isControlLayer || ! it->hasRect || it->ownerId != owner)
                        continue;

                    const auto& n = it->normalizedRect;

                    if (n.getX() <= 0.0001f && n.getY() <= 0.0001f
                        && n.getRight() >= 0.9999f && n.getBottom() >= 0.9999f)
                        continue;

                    if (itemFrameHitTest (*it, position))
                        return &*it;
                }
        }

        // 1) 已经选中的控件图层优先：它可能就是铺满控件的，不能让它被下面的控件抢走鼠标。
        for (auto it = items.rbegin(); it != items.rend(); ++it)
            if (it->isControlLayer && it->hasRect && it->id == selected
                && itemHitTest (*it, position))
                return &*it;

        // 2) 控件与画布图层：后声明的元素在上面（与清单顺序一致，方便点选压在一起的控件）
        for (auto it = items.rbegin(); it != items.rend(); ++it)
            if (! it->isControlLayer && it->hasRect
                && hitTestRect (it->rect, it->transform, position))
                return &*it;

        // 3) 当前选中控件的其它图层：只在所属控件被选中时才参与命中，
        //    否则新建的整幅图层会把控件本身完全挡住。
        const auto owner = selected.upToFirstOccurrenceOf ("/", false, false);

        if (owner.isNotEmpty())
            for (auto it = items.rbegin(); it != items.rend(); ++it)
                if (it->isControlLayer && it->hasRect && it->ownerId == owner
                    && itemHitTest (*it, position))
                    return &*it;

        return nullptr;
    }

    const DesignerOverlay::Item* DesignerOverlay::currentItem() const
    {
        for (const auto& item : items)
            if (item.id == selected)
                return &item;

        return nullptr;
    }

    juce::Rectangle<float> DesignerOverlay::itemArea (const Item& item) const
    {
        if (! item.isControlLayer)
            return item.rect.toFloat();

        const auto owner = item.rect.toFloat();
        const auto n = item.normalizedRect;

        return { owner.getX() + n.getX() * owner.getWidth(),
                 owner.getY() + n.getY() * owner.getHeight(),
                 n.getWidth() * owner.getWidth(),
                 n.getHeight() * owner.getHeight() };
    }

    juce::AffineTransform DesignerOverlay::itemAffine (const Item& item) const
    {
        if (! item.isControlLayer)
            return item.transform.isIdentity() ? juce::AffineTransform()
                                               : item.transform.toAffine (item.rect.toFloat());

        // 与运行时 ControlLayerOverlay 完全一致：先在图层自己的区域内做变换，
        // 再套控件元素的变换（控件旋转时图层跟着转）。
        juce::AffineTransform affine;

        if (! item.layerTransform.isIdentity())
            affine = item.layerTransform.toAffine (itemArea (item));

        if (! item.transform.isIdentity())
            affine = affine.followedBy (item.transform.toAffine (item.rect.toFloat()));

        return affine;
    }

    bool DesignerOverlay::itemHitTest (const Item& item, juce::Point<int> position) const
    {
        const auto affine = itemAffine (item);
        const auto local = affine.isIdentity()
                               ? position.toFloat()
                               : applyTransform (affine.inverted(), position.toFloat());

        return itemArea (item).contains (local);
    }

    bool DesignerOverlay::itemFrameHitTest (const Item& item, juce::Point<int> position) const
    {
        const auto affine = itemAffine (item);
        const auto local = affine.isIdentity()
                               ? position.toFloat()
                               : applyTransform (affine.inverted(), position.toFloat());

        const auto area  = itemArea (item);
        const auto inner = area.reduced (kFrameHitTolerance);

        // 外扩一圈算“够近”，内缩一圈后仍包含则说明落在框内部，不属于框线。
        return area.expanded (kFrameHitTolerance).contains (local)
               && (inner.isEmpty() || ! inner.contains (local));
    }

    bool DesignerOverlay::itemIsVisibleInOverlay (const Item& item) const
    {
        if (! item.isControlLayer)
            return true;

        // 只在它所属的控件（或该控件下的某个图层）被选中时显示，
        // 否则每个控件都铺着一层虚线，画面没法看。
        return selected == item.ownerId || selected.startsWith (item.ownerId + "/");
    }

    juce::Rectangle<float> DesignerOverlay::handleArea (const Item& item) const
    {
        const auto area = itemArea (item);
        const auto affine = itemAffine (item);
        const auto corner = juce::Point<float> (area.getRight(), area.getBottom());

        return juce::Rectangle<float> (static_cast<float> (kHandleSize), static_cast<float> (kHandleSize))
                   .withCentre (affine.isIdentity() ? corner : applyTransform (affine, corner));
    }

    juce::AffineTransform DesignerOverlay::overlayTransform (const Item& item) const
    {
        return itemAffine (item);
    }

    juce::Point<float> DesignerOverlay::travelHandlePosition (const Item& item, const Travel& travel,
                                                              int index) const
    {
        const auto area = item.rect.toFloat();

        const juce::Point<float> points[4] { travel.p0, travel.p1, travel.p2, travel.p3 };
        const auto& p = points[juce::jlimit (0, 3, index)];

        // 归一化控制点先落回控件矩形，再套元素变换——与运行时 Travel::pointIn 同一套坐标。
        const auto local = juce::Point<float> (area.getX() + p.x * area.getWidth(),
                                               area.getY() + p.y * area.getHeight());
        const auto affine = overlayTransform (item);

        return affine.isIdentity() ? local : applyTransform (affine, local);
    }

    void DesignerOverlay::paint (juce::Graphics& g)
    {
        // 3e：先把"画布 / 窗口"的关系画清楚 —— 出血区（画布之外、窗口之内的那圈）
        // 在这里是**可以放东西的**，所以用淡色铺一层 + 虚线勾出画布边界。
        if (! canvasArea.isEmpty() && canvasArea != getLocalBounds())
        {
            g.setColour (juce::Colour (0xff0fdc78).withAlpha (0.06f));
            g.fillRect (getLocalBounds());

            g.setColour (juce::Colour (0xff0fdc78).withAlpha (0.35f));
            g.fillRect (canvasArea);
        }

        if (! canvasArea.isEmpty())
        {
            juce::Path canvasFrame;
            canvasFrame.addRectangle (canvasArea.toFloat());

            juce::Path dashed;
            const float dashes[] = { 4.0f, 4.0f };
            juce::PathStrokeType (1.0f).createDashedStroke (dashed, canvasFrame, dashes, 2);

            g.setColour (juce::Colour (0xff0fdc78).withAlpha (0.45f));
            g.strokePath (dashed, juce::PathStrokeType (1.0f));
        }

        // 形状蒙版：以低不透明度把"窗口形状"叠出来（不做等值线，够看轮廓了）
        if (! shapeMaskImage.isNull())
        {
            const auto scaleX = static_cast<float> (getWidth())  / static_cast<float> (shapeMaskImage.getWidth());
            const auto scaleY = static_cast<float> (getHeight()) / static_cast<float> (shapeMaskImage.getHeight());

            const juce::Graphics::ScopedSaveState save (g);
            g.setOpacity (0.18f);
            g.drawImageTransformed (shapeMaskImage,
                                    juce::AffineTransform::scale (scaleX, scaleY));
        }

        for (const auto& item : items)
        {
            if (! item.hasRect || ! itemIsVisibleInOverlay (item))
                continue;

            const bool isSelected = item.id == selected;
            const bool isLayer    = item.isLayer;
            const bool isControl  = item.isControlLayer;
            const auto area       = itemArea (item);

            g.setColour (isSelected ? juce::Colours::orange.withAlpha (0.95f)
                                    : isControl ? juce::Colour (0xffbf5af2).withAlpha (0.72f)
                                    : isLayer ? juce::Colour (0xff3a86ff).withAlpha (0.62f)
                                              : juce::Colours::white.withAlpha (0.28f));

            // 画"变换后"的框：清单里写了旋转，选框就跟着转，否则看不出控件到底转了多少。
            // 控件图层会先把归一化矩形换算成控件里的实际区域，再套图层变换和控件变换。
            juce::Path frame;
            frame.addRectangle (area);

            const auto affine = itemAffine (item);

            if (! affine.isIdentity())
                frame.applyTransform (affine);

            if (isLayer || isControl)
            {
                // 图层可能没有可见内容：虚线框表示"这是可拖的装饰层"
                juce::Path dashed;
                const float dashes[] = { 6.0f, 4.0f };
                juce::PathStrokeType (isSelected ? 1.8f : 1.0f)
                    .createDashedStroke (dashed, frame, dashes, 2);
                g.strokePath (dashed, juce::PathStrokeType (isSelected ? 1.8f : 1.0f));
            }
            else
            {
                g.strokePath (frame, juce::PathStrokeType (isSelected ? 2.0f : 1.0f));
            }

            if (isSelected)
            {
                // 元素名 + 尺寸，方便边拖边看数字（写在未变换的位置，保证读得到）
                juce::String detail;

                if (isControl)
                {
                    const auto owner = item.rect.toFloat();
                    const auto& n = item.normalizedRect;

                    detail = juce::String (juce::roundToInt (n.getX() * owner.getWidth())) + ","
                           + juce::String (juce::roundToInt (n.getY() * owner.getHeight())) + "  "
                           + juce::String (juce::roundToInt (n.getWidth() * owner.getWidth())) + U8 ("×")
                           + juce::String (juce::roundToInt (n.getHeight() * owner.getHeight()));
                }
                else
                {
                    detail = juce::String (item.rect.getX()) + ","
                           + juce::String (item.rect.getY()) + "  "
                           + juce::String (item.rect.getWidth()) + U8 ("×")
                           + juce::String (item.rect.getHeight());
                }

                const auto labelPrefix = isControl ? U8 ("[控件图层] ")
                                        : isLayer   ? U8 ("[图层] ")
                                                    : juce::String();

                auto labelBounds = area.toNearestInt().withY (area.toNearestInt().getY() - 16)
                                       .withHeight (14);

                // 控件图层可能很窄，名字 + 尺寸放不下时会被省略号吃掉后半段，
                // 给一个最小宽度，保证 X/Y/宽/高 始终读得到。
                if (isControl)
                    labelBounds = labelBounds.withWidth (juce::jmax (labelBounds.getWidth(), 220));

                // 靠近画布下缘 / 右缘时标签会被窗口裁掉（控件图层尤其容易落到边上），
                // 夹取回画布内，保证选中时始终读得到名字和尺寸。
                labelBounds.setY (juce::jlimit (0, juce::jmax (0, getHeight() - labelBounds.getHeight()),
                                                labelBounds.getY()));
                labelBounds.setX (juce::jlimit (0, juce::jmax (0, getWidth() - labelBounds.getWidth()),
                                                labelBounds.getX()));

                g.setFont (juce::FontOptions (11.0f));
                g.drawText (labelPrefix + item.label + "  " + detail, labelBounds,
                            juce::Justification::centredLeft, true);

                // ---- 运动轨迹 ----
                // 图层是静态装饰，没有运行时轨迹，跳过这一段。
                // 元素级一条、每个部位各一条；部位用不同颜色，方便分辨是哪一块在动。
                // 坐标换算与运行时 Travel::pointIn 完全一致，所以这里看到的曲线就是控件实际走的路径。
                auto drawTravel = [&] (const Travel& travel, const juce::String& part, juce::Colour colour)
                {
                    const auto area = item.rect.toFloat();
                    const auto affine = overlayTransform (item);
                    const bool active = travel.enabled;

                    auto path = travel.pathIn (area);

                    if (! affine.isIdentity())
                        path.applyTransform (affine);

                    g.setColour (colour.withAlpha (active ? 0.95f : 0.42f));

                    if (active)
                    {
                        g.strokePath (path, juce::PathStrokeType (part.isEmpty() ? 2.0f : 1.6f,
                                                                  juce::PathStrokeType::curved,
                                                                  juce::PathStrokeType::rounded));
                    }
                    else
                    {
                        // 未启用：虚线。路径照样显示，方便先摆好再决定要不要让它生效。
                        juce::Path dashed;
                        const float dashes[] = { 5.0f, 4.0f };
                        juce::PathStrokeType (1.2f).createDashedStroke (dashed, path, dashes, 2);
                        g.strokePath (dashed, juce::PathStrokeType (1.2f));
                    }

                    // 控制多边形 + 控制点手柄（P0…P3；P1 / P2 是贝塞尔的"拉力"点）
                    juce::Point<float> screen[4];

                    for (int i = 0; i < 4; ++i)
                        screen[i] = travelHandlePosition (item, travel, i);

                    g.setColour (colour.withAlpha (active ? 0.45f : 0.28f));

                    for (int i = 0; i < 3; ++i)
                        g.drawLine (juce::Line<float> (screen[i], screen[i + 1]), 1.0f);

                    for (int i = 0; i < 4; ++i)
                    {
                        const float radius = (i == 1 || i == 2) ? 4.5f : 3.5f;
                        const auto dot = juce::Rectangle<float> (radius * 2.0f, radius * 2.0f)
                                             .withCentre (screen[i]);

                        g.setColour (colour.withAlpha (active ? 0.95f : 0.6f));
                        g.fillEllipse (dot);
                        g.setColour (juce::Colours::black.withAlpha (0.55f));
                        g.drawEllipse (dot, 1.0f);
                    }

                    if (part.isNotEmpty())
                    {
                        g.setColour (colour.withAlpha (0.9f));
                        g.setFont (juce::FontOptions (10.0f));
                        g.drawText (part,
                                    juce::Rectangle<float> (screen[0].x + 7.0f, screen[0].y - 8.0f, 90.0f, 14.0f),
                                    juce::Justification::centredLeft, true);
                    }
                };

                const juce::Colour partColours[]
                {
                    juce::Colour (0xffff9f0a), juce::Colour (0xff3a86ff), juce::Colour (0xffff6ec7),
                    juce::Colour (0xffffd60a), juce::Colour (0xffbf5af2)
                };

                if (! isLayer && ! isControl)
                {
                    drawTravel (item.travel, {}, juce::Colour (0xff0fdc78));

                    // 没有单独配置的部位，运行时走的可能是元素级、也可能是别的部位
                    // （例如 thumbOver 未启用时回落 thumb）：同一路径只画一次，
                    // 否则曲线叠在一起既看不清，控制点也会互相抢鼠标。
                    juce::StringArray sharedParts;
                    std::vector<Travel> drawnTravels { item.travel };
                    int partIndex = 0;

                    for (const auto& entry : item.travelParts)
                    {
                        const bool active = entry.first == activeTravelPart;
                        const bool shared = std::any_of (drawnTravels.begin(), drawnTravels.end(),
                                                         [&entry] (const Travel& t)
                                                         { return travelMatches (entry.second, t); });

                        if (! active && shared)
                        {
                            sharedParts.add (entry.first);
                            continue;
                        }

                        drawnTravels.push_back (entry.second);
                        drawTravel (entry.second, entry.first, partColours[(partIndex++) % 5]);
                    }

                    if (! sharedParts.isEmpty())
                    {
                        g.setColour (juce::Colours::white.withAlpha (0.55f));
                        g.setFont (juce::FontOptions (10.0f));
                        g.drawText (U8 ("共用路径：") + sharedParts.joinIntoString (", "),
                                    item.rect.withY (item.rect.getBottom() + 2).withHeight (13),
                                    juce::Justification::centredLeft, true);
                    }
                }

                g.setColour (juce::Colours::orange);
                g.fillRect (handleArea (item));
            }
        }
    }

    void DesignerOverlay::mouseDown (const juce::MouseEvent& e)
    {
        grabKeyboardFocus();

        travelDragPoint = -1;
        travelDragPart.clear();

        if (const auto* item = currentItem(); item != nullptr && item->hasRect
            && ! item->isLayer && ! item->isControlLayer)
        {
            const auto mouse = e.getPosition().toFloat();

            auto hitTravel = [&] (const Travel& travel, const juce::String& part)
            {
                for (int i = 0; i < 4; ++i)
                {
                    if (travelHandlePosition (*item, travel, i).getDistanceFrom (mouse) <= kTravelHandleRadius)
                    {
                        travelDragPart = part;
                        travelDragPoint = i;
                        dragging = false;
                        resizing = false;
                        repaint();
                        return true;
                    }
                }

                return false;
            };

            // 与绘制共用同一套去重规则：正在编辑的部位优先，其次只测"确实画出来"的曲线，
            // 最后才是元素级 —— 重合的控制点只有一条曲线能接管鼠标，不再随机抢。
            if (activeTravelPart.isNotEmpty())
            {
                for (const auto& entry : item->travelParts)
                    if (entry.first == activeTravelPart && hitTravel (entry.second, entry.first))
                        return;
            }

            std::vector<Travel> drawnTravels { item->travel };

            for (const auto& entry : item->travelParts)
            {
                if (entry.first == activeTravelPart)
                    continue;

                const bool shared = std::any_of (drawnTravels.begin(), drawnTravels.end(),
                                                 [&entry] (const Travel& t)
                                                 { return travelMatches (entry.second, t); });

                if (shared)
                    continue;

                drawnTravels.push_back (entry.second);

                if (hitTravel (entry.second, entry.first))
                    return;
            }

            if (hitTravel (item->travel, {}))
                return;
        }

        if (const auto* item = currentItem())
        {
            if (handleArea (*item).contains (e.getPosition().toFloat()))
            {
                resizing = true;
                dragging = false;
                dragStart = e.getPosition();
                rectAtDragStart = item->rect;
                normalizedAtDragStart = item->normalizedRect;
                return;
            }
        }

        if (const auto* hit = findItem (e.getPosition()))
        {
            selected = hit->id;
            dragging = true;
            resizing = false;
            dragStart = e.getPosition();
            rectAtDragStart = hit->rect;
            normalizedAtDragStart = hit->normalizedRect;

            if (hit->isControlLayer)
            {
                if (onControlLayerSelected)
                    onControlLayerSelected (hit->ownerId, hit->layerId);
            }
            else if (onSelectionChanged && ! hit->isLayer)
            {
                onSelectionChanged (selected);
            }

            if (onLayerSelected && hit->isLayer)
                onLayerSelected (selected);

            repaint();
            return;
        }

        // 空白处按下（没命中任何条目）：编辑层自己没别的事可做，把它交给编辑器 ——
        // 无标题栏的窗口要靠这些"落在空白处"的点击来拖动，右键菜单也只有那里能弹。
        // 不转发的话，只要设计器开着，异形窗口就拖不动、右键菜单也打不开。
        // （选中状态保持不动：原来点空白就是不改变选中的）
        if (onEmptyAreaPressed)
            onEmptyAreaPressed (e);
    }

    void DesignerOverlay::mouseDrag (const juce::MouseEvent& e)
    {
        if (travelDragPoint >= 0)
        {
            auto* item = const_cast<Item*> (currentItem());

            if (item == nullptr)
            {
                travelDragPoint = -1;
                return;
            }

            Travel* travel = nullptr;

            if (travelDragPart.isEmpty())
            {
                travel = &item->travel;
            }
            else
            {
                for (auto& entry : item->travelParts)
                {
                    if (entry.first == travelDragPart)
                    {
                        travel = &entry.second;
                        break;
                    }
                }
            }

            if (travel == nullptr)
            {
                travelDragPoint = -1;
                return;
            }

            // 鼠标位置先逆变换回控件自己的坐标系，再归一化成 0…1 —— 清单里存的就是这个值。
            const auto area = item->rect.toFloat();
            const auto local = applyTransform (overlayTransform (*item).inverted(),
                                               e.getPosition().toFloat());

            const auto normalised = juce::Point<float> (
                juce::jlimit (0.0f, 1.0f, (local.x - area.getX()) / juce::jmax (1.0f, area.getWidth())),
                juce::jlimit (0.0f, 1.0f, (local.y - area.getY()) / juce::jmax (1.0f, area.getHeight())));

            switch (travelDragPoint)
            {
                case 0: travel->p0 = normalised; break;
                case 1: travel->p1 = normalised; break;
                case 2: travel->p2 = normalised; break;
                case 3: travel->p3 = normalised; break;
                default: break;
            }

            // 拖控制点即视为启用：否则改完曲线运行时却没反应，看起来就像"自定义轨迹没绑定"。
            travel->enabled = true;

            repaint();   // 拖动过程不写盘（与矩形拖动同理），松手再落到清单
            return;
        }

        auto* item = const_cast<Item*> (currentItem());

        if (item == nullptr || (! dragging && ! resizing))
        {
            // 空白处起来的拖动也一样交给编辑器（无标题栏的窗口靠它移动）
            if (onEmptyAreaDragged)
                onEmptyAreaDragged (e);

            return;
        }

        auto delta = e.getPosition() - dragStart;
        auto rect  = rectAtDragStart;

        if (item->isControlLayer)
        {
            const auto owner  = item->rect.toFloat();
            const auto ownerW = juce::jmax (1.0f, owner.getWidth());
            const auto ownerH = juce::jmax (1.0f, owner.getHeight());

            const auto startArea = juce::Rectangle<float> (
                owner.getX() + normalizedAtDragStart.getX() * ownerW,
                owner.getY() + normalizedAtDragStart.getY() * ownerH,
                normalizedAtDragStart.getWidth() * ownerW,
                normalizedAtDragStart.getHeight() * ownerH);

            // 与运行时同一套变换：图层变换 → 控件元素变换。
            juce::AffineTransform affine;

            if (! item->layerTransform.isIdentity())
                affine = item->layerTransform.toAffine (startArea);

            if (! item->transform.isIdentity())
                affine = affine.followedBy (item->transform.toAffine (owner));

            const auto back = affine.inverted();

            // 屏幕增量是方向量：只过仿射的线性部分，不能把平移也算进去。
            // JUCE 8 的矩阵布局是 [mat00 mat01 mat02; mat10 mat11 mat12]，手写乘法必须
            // 按这个顺序取元素，否则得到的是转置矩阵 —— 旋转过的控件上拖拽会往斜对角跑。
            const auto localDelta = juce::Point<float> (
                back.mat00 * delta.x + back.mat01 * delta.y,
                back.mat10 * delta.x + back.mat11 * delta.y);

            auto n = normalizedAtDragStart;

            if (resizing)
            {
                // 最小 4 px，折算成归一化尺寸。
                n.setSize (juce::jmax (4.0f / ownerW, n.getWidth()  + localDelta.x / ownerW),
                           juce::jmax (4.0f / ownerH, n.getHeight() + localDelta.y / ownerH));
            }
            else
            {
                n.setPosition (n.getX() + localDelta.x / ownerW,
                               n.getY() + localDelta.y / ownerH);
            }

            item->normalizedRect = n;

            if (onControlLayerRectPreview)
                onControlLayerRectPreview (item->ownerId, item->layerId, n);

            repaint();
            return;
        }

        if (resizing)
        {
            // 鼠标增量在屏幕坐标里；把它反向旋转回元素自己的坐标系再改宽高，
            // 免得"框转了 45° 之后往右拖却是斜着变宽"这种别扭的手感。
            if (item->transform.rotation != 0.0f)
            {
                const auto back = juce::AffineTransform::rotation (
                    juce::degreesToRadians (-item->transform.rotation));
                const auto d = applyTransform (back, delta.toFloat());
                delta = { juce::roundToInt (d.x), juce::roundToInt (d.y) };
            }

            rect.setSize (juce::jmax (8, rectAtDragStart.getWidth()  + delta.x),
                          juce::jmax (8, rectAtDragStart.getHeight() + delta.y));
        }
        else
        {
            rect.setPosition (rectAtDragStart.getX() + delta.x,
                              rectAtDragStart.getY() + delta.y);
        }

        item->rect = rect;
        item->hasRect = true;

        // 拖动过程只走"预览"回调（编辑器直接改控件 bounds），松手才写文件——
        // 否则每个鼠标移动事件都会写一次 skin.json 并重载皮肤（含贴图解码），卡得没法用。
        if (item->isLayer)
        {
            if (onLayerRectPreview)
                onLayerRectPreview (item->id, rect);
        }
        else if (onRectPreview)
        {
            onRectPreview (item->id, rect);
        }

        repaint();
    }

    void DesignerOverlay::mouseUp (const juce::MouseEvent&)
    {
        if (travelDragPoint >= 0)
        {
            if (const auto* item = currentItem())
            {
                const Travel* travel = nullptr;

                if (travelDragPart.isEmpty())
                {
                    travel = &item->travel;
                }
                else
                {
                    for (const auto& entry : item->travelParts)
                    {
                        if (entry.first == travelDragPart)
                        {
                            travel = &entry.second;
                            break;
                        }
                    }
                }

                if (travel != nullptr && onTravelPointChanged)
                {
                    const juce::Point<float> points[4] { travel->p0, travel->p1, travel->p2, travel->p3 };

                    onTravelPointChanged (item->id, travelDragPart, travelDragPoint,
                                          points[juce::jlimit (0, 3, travelDragPoint)]);
                }
            }

            travelDragPoint = -1;
            travelDragPart.clear();
            return;
        }

        if (const auto* item = currentItem(); (dragging || resizing) && item != nullptr)
        {
            if (item->isControlLayer)
            {
                if (onControlLayerRectChanged)
                    onControlLayerRectChanged (item->ownerId, item->layerId, item->normalizedRect);
            }
            else if (item->isLayer)
            {
                if (onLayerRectChanged)
                    onLayerRectChanged (item->id, item->rect);
            }
            else if (onRectChanged)
            {
                onRectChanged (item->id, item->rect);
            }
        }

        dragging = false;
        resizing = false;
    }

    bool DesignerOverlay::keyPressed (const juce::KeyPress& key)
    {
        const auto* item = currentItem();

        if (item == nullptr)
            return false;

        // Esc：从控件图层退回它所属的控件，方便继续调整控件本身。
        if (item->isControlLayer && key.getKeyCode() == juce::KeyPress::escapeKey)
        {
            selected = item->ownerId;
            repaint();

            if (onSelectionChanged)
                onSelectionChanged (selected);

            return true;
        }

        const int step = key.getModifiers().isShiftDown() ? 10 : 1;

        if (item->isControlLayer)
        {
            const auto owner  = item->rect.toFloat();
            const auto ownerW = juce::jmax (1.0f, owner.getWidth());
            const auto ownerH = juce::jmax (1.0f, owner.getHeight());

            auto n = item->normalizedRect;

            if      (key.getKeyCode() == juce::KeyPress::leftKey)  n.setX (n.getX() - step / ownerW);
            else if (key.getKeyCode() == juce::KeyPress::rightKey) n.setX (n.getX() + step / ownerW);
            else if (key.getKeyCode() == juce::KeyPress::upKey)    n.setY (n.getY() - step / ownerH);
            else if (key.getKeyCode() == juce::KeyPress::downKey)  n.setY (n.getY() + step / ownerH);
            else return false;

            const_cast<Item*> (item)->normalizedRect = n;

            if (onControlLayerRectPreview)
                onControlLayerRectPreview (item->ownerId, item->layerId, n);

            if (onControlLayerRectChanged)
                onControlLayerRectChanged (item->ownerId, item->layerId, n);

            repaint();
            return true;
        }

        auto rect = item->rect;

        if      (key.getKeyCode() == juce::KeyPress::leftKey)  rect.setX (rect.getX() - step);
        else if (key.getKeyCode() == juce::KeyPress::rightKey) rect.setX (rect.getX() + step);
        else if (key.getKeyCode() == juce::KeyPress::upKey)    rect.setY (rect.getY() - step);
        else if (key.getKeyCode() == juce::KeyPress::downKey)  rect.setY (rect.getY() + step);
        else return false;

        const_cast<Item*> (item)->rect = rect;

        if (item->isLayer)
        {
            if (onLayerRectPreview)
                onLayerRectPreview (item->id, rect);

            if (onLayerRectChanged)
                onLayerRectChanged (item->id, rect);
        }
        else
        {
            if (onRectPreview)
                onRectPreview (item->id, rect);

            if (onRectChanged)
                onRectChanged (item->id, rect);
        }

        repaint();
        return true;
    }

    // ===================================================================
    // 矢量绘图（导入图片旁边的入口）
    // ===================================================================

    namespace
    {
        /**
            内置矢量绘图工具：矩形 / 圆角矩形 / 椭圆 / 直线 / 正多边形。

            输出是一段 SVG 文本，由 SkinDocument::importVectorAsset 写成皮肤里的
            assets/<name>.svg；Skin::imageFor 会在解码时把它光栅化，因此贴图槽位、
            装饰图层、背景都能直接使用，不需要调用处区分位图和矢量。
        */
        class VectorShapeEditor final : public juce::Component
        {
        public:
            explicit VectorShapeEditor (std::function<void (const juce::String&)> exportSvg)
                : onExport (std::move (exportSvg))
            {
                juce::StringArray shapes;
                shapes.add (U8 ("矩形"));
                shapes.add (U8 ("圆角矩形"));
                shapes.add (U8 ("椭圆"));
                shapes.add (U8 ("直线"));
                shapes.add (U8 ("多边形"));
                shapeBox.addItemList (shapes, 1);
                shapeBox.setSelectedId (2, juce::dontSendNotification);

                for (auto* slider : { &widthSlider, &heightSlider, &strokeSlider,
                                      &radiusSlider, &sidesSlider })
                {
                    slider->setSliderStyle (juce::Slider::LinearHorizontal);
                    slider->setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
                    slider->setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff141416));
                    slider->setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0xff4a4a4e));
                    slider->setTextBoxStyle (juce::Slider::TextBoxRight, false, 56, 18);
                    slider->setColour (juce::Slider::trackColourId, juce::Colour (0xff3a86ff));
                    slider->setColour (juce::Slider::backgroundColourId, juce::Colour (0xff2c2c2e));

                    addAndMakeVisible (*slider);
                }

                widthSlider .setRange (8.0, 1024.0, 1.0);
                heightSlider.setRange (8.0, 1024.0, 1.0);
                strokeSlider.setRange (0.0, 40.0, 0.5);
                radiusSlider.setRange (0.0, 256.0, 1.0);
                sidesSlider .setRange (3.0, 16.0, 1.0);

                widthSlider .setValue (96.0,  juce::dontSendNotification);
                heightSlider.setValue (96.0,  juce::dontSendNotification);
                strokeSlider.setValue (0.0,   juce::dontSendNotification);
                radiusSlider.setValue (18.0,  juce::dontSendNotification);
                sidesSlider .setValue (3.0,   juce::dontSendNotification);

                fillEditor  .setText ("#3a86ff", false);
                strokeEditor.setText ("#ffffff", false);

                auto setupLabel = [this] (juce::Label& label, const char* text)
                {
                    label.setText (U8 (text), juce::dontSendNotification);
                    label.setFont (juce::FontOptions (12.0f));
                    label.setColour (juce::Label::textColourId, juce::Colours::lightgrey);
                    label.setJustificationType (juce::Justification::centredRight);
                    addAndMakeVisible (label);
                };

                setupLabel (shapeLabel,  "形状");
                setupLabel (widthLabel,  "宽");
                setupLabel (heightLabel, "高");
                setupLabel (radiusLabel, "圆角");
                setupLabel (sidesLabel,  "边数");
                setupLabel (strokeLabel, "描边宽");
                setupLabel (fillLabel,   "填充色");
                setupLabel (strokeColourLabel, "描边色");

                for (auto* editor : { &fillEditor, &strokeEditor })
                {
                    editor->setMultiLine (false);
                    editor->setReturnKeyStartsNewLine (false);
                    editor->setInputRestrictions (9, "#0123456789abcdefABCDEF");
                    addAndMakeVisible (*editor);
                }

                fillEditor  .setTooltip (U8 ("#rrggbb；留空 = 不填充"));
                strokeEditor.setTooltip (U8 ("#rrggbb；留空 = 不描边"));

                addAndMakeVisible (shapeBox);

                createButton.setButtonText (U8 ("创建"));
                cancelButton.setButtonText (U8 ("取消"));
                addAndMakeVisible (createButton);
                addAndMakeVisible (cancelButton);

                shapeBox.onChange = [this] { updateEnabled(); repaint(); };
                fillEditor.onTextChange = [this] { repaint(); };
                strokeEditor.onTextChange = [this] { repaint(); };

                createButton.onClick = [this]
                {
                    if (onExport != nullptr)
                        onExport (buildSvg());

                    if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
                        dialog->closeButtonPressed();
                };

                cancelButton.onClick = [this]
                {
                    if (auto* dialog = findParentComponentOfClass<juce::DialogWindow>())
                        dialog->closeButtonPressed();
                };

                updateEnabled();
                setSize (430, 440);
            }

            void paint (juce::Graphics& g) override
            {
                g.fillAll (juce::Colour (0xff232326));

                if (previewBounds.isEmpty())
                    return;

                g.setColour (juce::Colour (0xff1a1a1c));
                g.fillRect (previewBounds);

                // 棋盘底：透明区域一眼可辨
                g.setColour (juce::Colours::white.withAlpha (0.035f));

                constexpr int cell = 12;
                for (int y = 0; y < previewBounds.getHeight(); y += cell)
                    for (int x = 0; x < previewBounds.getWidth(); x += cell)
                        if (((x / cell) + (y / cell)) % 2 == 0)
                            g.fillRect (previewBounds.getX() + x, previewBounds.getY() + y,
                                        juce::jmin (cell, previewBounds.getWidth() - x),
                                        juce::jmin (cell, previewBounds.getHeight() - y));

                g.setColour (juce::Colours::white.withAlpha (0.18f));
                g.drawRect (previewBounds, 1);

                const auto box = fitPreviewBox();

                if (box.isEmpty())
                    return;

                const auto path = buildPath (box);
                const auto fill = parseColour (fillEditor.getText());

                if (fill.getAlpha() > 0)
                {
                    g.setColour (fill);
                    g.fillPath (path);
                }

                const auto stroke = parseColour (strokeEditor.getText());
                const auto strokeWidth = static_cast<float> (strokeSlider.getValue());
                const auto scale = box.getWidth() / juce::jmax (1.0, widthSlider.getValue());

                if (strokeWidth > 0.0f && stroke.getAlpha() > 0)
                {
                    g.setColour (stroke);
                    g.strokePath (path, juce::PathStrokeType (juce::jmax (0.5f, strokeWidth * (float) scale)));
                }
            }

            void resized() override
            {
                auto area = getLocalBounds().reduced (10);

                auto row = [&area] (int height)
                {
                    auto r = area.removeFromTop (height);
                    area.removeFromTop (4);
                    return r;
                };

                auto place = [] (juce::Rectangle<int>& r, juce::Label& label, juce::Component& control)
                {
                    label.setBounds (r.removeFromLeft (72));
                    r.removeFromLeft (6);
                    control.setBounds (r);
                };

                auto shapeRow = row (24);
                place (shapeRow, shapeLabel, shapeBox);

                auto widthRow = row (24);
                place (widthRow, widthLabel, widthSlider);

                auto heightRow = row (24);
                place (heightRow, heightLabel, heightSlider);

                auto radiusRow = row (24);
                place (radiusRow, radiusLabel, radiusSlider);

                auto sidesRow = row (24);
                place (sidesRow, sidesLabel, sidesSlider);

                auto strokeRow = row (24);
                place (strokeRow, strokeLabel, strokeSlider);

                auto fillRow = row (24);
                place (fillRow, fillLabel, fillEditor);

                auto strokeColourRow = row (24);
                place (strokeColourRow, strokeColourLabel, strokeEditor);

                previewBounds = area.removeFromTop (juce::jmax (80, area.getHeight() - 34));
                area.removeFromTop (8);

                auto buttons = area.removeFromTop (26);
                cancelButton.setBounds (buttons.removeFromRight (76));
                buttons.removeFromRight (6);
                createButton.setBounds (buttons.removeFromRight (76));
            }

        private:
            void updateEnabled()
            {
                const int shape = shapeBox.getSelectedId();
                radiusSlider.setEnabled (shape == 2);
                sidesSlider .setEnabled (shape == 5);
                fillEditor  .setEnabled (shape != 4);
            }

            juce::Rectangle<float> fitPreviewBox() const
            {
                auto area = previewBounds.reduced (14).toFloat();

                if (area.isEmpty())
                    return {};

                const auto w = juce::jmax (1.0f, (float) widthSlider.getValue());
                const auto h = juce::jmax (1.0f, (float) heightSlider.getValue());
                const auto scale = juce::jmin (area.getWidth() / w, area.getHeight() / h);

                return juce::Rectangle<float> (w * scale, h * scale).withCentre (area.getCentre());
            }

            static juce::Colour parseColour (const juce::String& text)
            {
                auto trimmed = text.trim();

                if (trimmed.isEmpty())
                    return juce::Colours::transparentBlack;

                auto hex = trimmed.startsWith ("#") ? trimmed.substring (1) : trimmed;

                // Colour::fromString 的 6 位写法会被当成 0x00RRGGBB（alpha = 0，全透明），
                // 这里补上不透明 alpha，预览才不会"什么都没有"。
                if (hex.length() == 6)
                    hex = "ff" + hex;
                else if (hex.length() == 3)
                    hex = "ff" + juce::String::charToString (hex[0]) + hex[0]
                              + juce::String::charToString (hex[1]) + hex[1]
                              + juce::String::charToString (hex[2]) + hex[2];

                return juce::Colour::fromString ("#" + hex);
            }

            juce::Path buildPath (juce::Rectangle<float> box) const
            {
                const int shape = shapeBox.getSelectedId();
                juce::Path path;

                if (shape == 1)
                {
                    path.addRectangle (box);
                }
                else if (shape == 2)
                {
                    const auto scale = box.getWidth() / juce::jmax (1.0f, (float) widthSlider.getValue());
                    path.addRoundedRectangle (box, juce::jmax (0.0f, (float) radiusSlider.getValue()) * scale);
                }
                else if (shape == 3)
                {
                    path.addEllipse (box);
                }
                else if (shape == 4)
                {
                    path.startNewSubPath (box.getBottomLeft());
                    path.lineTo (box.getTopRight());
                }
                else
                {
                    const auto sides = juce::jmax (3, (int) sidesSlider.getValue());
                    const auto centre = box.getCentre();
                    const auto rx = box.getWidth() * 0.5f;
                    const auto ry = box.getHeight() * 0.5f;

                    for (int i = 0; i < sides; ++i)
                    {
                        const auto angle = -juce::MathConstants<float>::halfPi
                                           + juce::MathConstants<float>::twoPi * (float) i / (float) sides;
                        const juce::Point<float> p (centre.x + rx * std::cos (angle),
                                                    centre.y + ry * std::sin (angle));

                        if (i == 0)
                            path.startNewSubPath (p);
                        else
                            path.lineTo (p);
                    }

                    path.closeSubPath();
                }

                return path;
            }

            juce::String buildSvg() const
            {
                const auto w = juce::jmax (8, (int) widthSlider.getValue());
                const auto h = juce::jmax (8, (int) heightSlider.getValue());
                const auto strokeWidth = strokeSlider.getValue();
                const auto fillText = fillEditor.getText().trim();
                const auto strokeText = strokeEditor.getText().trim();

                const auto fill = fillText.isEmpty() ? juce::String ("none")
                                                     : (fillText.startsWith ("#") ? fillText : "#" + fillText);
                const auto stroke = strokeText.isEmpty() ? juce::String ("none")
                                                         : (strokeText.startsWith ("#") ? strokeText : "#" + strokeText);

                const auto common = " fill=\"" + fill + "\" stroke=\"" + stroke
                                    + "\" stroke-width=\"" + juce::String (strokeWidth, 2) + "\"";

                juce::String shape;
                const int type = shapeBox.getSelectedId();

                if (type == 1)
                {
                    shape = "<rect x=\"0\" y=\"0\" width=\"" + juce::String (w)
                            + "\" height=\"" + juce::String (h) + "\"" + common + "/>";
                }
                else if (type == 2)
                {
                    const auto radius = juce::jlimit (0.0, juce::jmin (w, h) / 2.0,
                                                      radiusSlider.getValue());
                    shape = "<rect x=\"0\" y=\"0\" width=\"" + juce::String (w)
                            + "\" height=\"" + juce::String (h)
                            + "\" rx=\"" + juce::String (radius, 2)
                            + "\" ry=\"" + juce::String (radius, 2) + "\"" + common + "/>";
                }
                else if (type == 3)
                {
                    shape = "<ellipse cx=\"" + juce::String (w / 2.0, 2)
                            + "\" cy=\"" + juce::String (h / 2.0, 2)
                            + "\" rx=\"" + juce::String (w / 2.0, 2)
                            + "\" ry=\"" + juce::String (h / 2.0, 2) + "\"" + common + "/>";
                }
                else if (type == 4)
                {
                    shape = "<line x1=\"0\" y1=\"" + juce::String (h)
                            + "\" x2=\"" + juce::String (w) + "\" y2=\"0\""
                            + " stroke=\"" + stroke + "\" stroke-width=\""
                            + juce::String (juce::jmax (0.5, strokeWidth), 2) + "\""
                            + " stroke-linecap=\"round\"/>";
                }
                else
                {
                    const auto sides = juce::jmax (3, (int) sidesSlider.getValue());
                    juce::String points;

                    for (int i = 0; i < sides; ++i)
                    {
                        const auto angle = -juce::MathConstants<double>::halfPi
                                           + juce::MathConstants<double>::twoPi * (double) i / (double) sides;
                        const auto x = (double) w * 0.5 + (double) w * 0.5 * std::cos (angle);
                        const auto y = (double) h * 0.5 + (double) h * 0.5 * std::sin (angle);

                        if (i > 0)
                            points << " ";

                        points << juce::String (x, 2) << "," << juce::String (y, 2);
                    }

                    shape = "<polygon points=\"" + points + "\"" + common + "/>";
                }

                return "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + juce::String (w)
                       + "\" height=\"" + juce::String (h) + "\" viewBox=\"0 0 "
                       + juce::String (w) + " " + juce::String (h) + "\">" + shape + "</svg>";
            }

            std::function<void (const juce::String&)> onExport;

            juce::ComboBox   shapeBox;
            juce::Slider     widthSlider, heightSlider, strokeSlider, radiusSlider, sidesSlider;
            juce::TextEditor fillEditor, strokeEditor;
            juce::TextButton createButton, cancelButton;

            juce::Label shapeLabel, widthLabel, heightLabel, radiusLabel, sidesLabel,
                        strokeLabel, fillLabel, strokeColourLabel;

            juce::Rectangle<int> previewBounds;
        };
    }

    // ===================================================================
    // DesignerWindow（浮动属性面板）
    // ===================================================================

    class DesignerWindow::Panel final : public juce::Component,
                                        private juce::Timer
    {
    public:
        Panel (SkinDocument& doc, DesignerWindow::Callbacks callbacks)
            : document (doc),
              editorBounds (std::move (callbacks.editorBoundsFor)),
              notifyChanged (std::move (callbacks.documentChanged)),
              notifySelected (std::move (callbacks.elementSelected)),
              notifyTravelPart (std::move (callbacks.travelPartSelected)),
              shaderCompileError (std::move (callbacks.shaderCompileError)),
              shaderRuntimeStatus (std::move (callbacks.shaderRuntimeStatus))
        {
            addAndMakeVisible (viewport);
            viewport.setViewedComponent (&content, false);
            viewport.setScrollBarsShown (true, false);

            for (int i = 0; i < kNumElements; ++i)
                elementBox.addItem (U8 (kElements[i].label) + "   " + kElements[i].id, i + 1);

            elementBox.onChange = [this]
            {
                const int index = elementBox.getSelectedId() - 1;

                if (index < 0 || index >= kNumElements)
                    return;

                // 从下拉框挑控件 = 退出图层模式
                layerMode = false;
                selectedControlLayer.clear();
                updateModeButtons();

                selected = kElements[index].id;
                rebuildRows();

                if (notifySelected)
                    notifySelected (selected);
            };

            // ---- 控件 / 图层 两种面板模式 ----
            elementModeButton.setButtonText (U8 ("控件"));
            layerModeButton  .setButtonText (U8 ("图层"));

            elementModeButton.setTooltip (U8 ("编辑控件的布局、变换、轨迹与贴图"));
            layerModeButton  .setTooltip (U8 ("图层管理：显示 / 隐藏、顺序、独立编辑"));

            elementModeButton.onClick = [this]
            {
                if (! layerMode)
                    return;

                layerMode = false;
                updateModeButtons();
                rebuildRows();
            };

            layerModeButton.onClick = [this]
            {
                if (layerMode)
                    return;

                enterLayerMode();
            };

            addAndMakeVisible (elementModeButton);
            addAndMakeVisible (layerModeButton);
            updateModeButtons();

            prevButton.onClick   = [this] { stepElement (-1); };
            nextButton.onClick   = [this] { stepElement (1); };
            saveButton.onClick   = [this] { flush(); };
            folderButton.onClick = [this] { document.getFolder().revealToUser(); };

            // ---- 方案预设 ----
            presetSaveButton  .onClick = [this] { saveCurrentPreset(); };
            presetLoadButton  .onClick = [this] { loadSelectedPreset(); };
            presetDeleteButton.onClick = [this] { deleteSelectedPreset(); };

            // 选到内置那条时「删除」自动变灰：内置预设从代码里来，删不掉
            presetBox.onChange = [this] { presetDeleteButton.setEnabled (canDeleteSelectedPreset()); };

            presetSaveButton  .setButtonText (U8 ("存为预设"));
            presetLoadButton  .setButtonText (U8 ("加载"));
            presetDeleteButton.setButtonText (U8 ("删除"));

            presetSaveButton.setTooltip (U8 ("把当前整套设计（清单 + 全部贴图 + 着色器）存成可复用的预设"));
            presetLoadButton.setTooltip (U8 ("用选中的预设整体替换当前皮肤方案；"
                                             "选第一条「默认布局（内置）」则回到内置默认布局（会先自动存一条备份预设）"));
            presetDeleteButton.setTooltip (U8 ("删除选中的预设（不会影响当前皮肤；内置那条删不掉）"));

            presetNameEditor.setTextToShowWhenEmpty (U8 ("输入新预设名…"),
                                                     juce::Colours::white.withAlpha (0.35f));
            presetBox.setTooltip (U8 ("方案预设：第一条是内置的默认布局，随时可以退回；下面是自己存的"));

            for (auto* b : { &prevButton, &nextButton, &saveButton, &folderButton,
                             &presetSaveButton, &presetLoadButton, &presetDeleteButton })
                addAndMakeVisible (*b);

            addAndMakeVisible (elementBox);
            addAndMakeVisible (presetBox);
            addAndMakeVisible (presetNameEditor);

            refreshPresetList();

            statusLabel.setJustificationType (juce::Justification::centredLeft);
            statusLabel.setFont (juce::FontOptions (12.0f));
            addAndMakeVisible (statusLabel);

            saveButton.setTooltip (U8 ("把改动写回 skin.json（旧版自动备份为 skin.json.bak）"));
            folderButton.setTooltip (U8 ("在资源管理器里打开皮肤目录"));

            saveButton.setButtonText (U8 ("保存"));
            folderButton.setButtonText (U8 ("打开目录"));

            // 着色器分区默认收起：它在面板最后，展开着会让元素自己的参数被挤到很下面
            collapsedSections.add (U8 (kShaderSectionTitle));

            setSelected ("mix");

            if (document.isWritable())
                setStatus (U8 ("皮肤目录：") + document.getFolder().getFileName(), false);
            else
                setStatus (U8 ("当前是内置皮肤；在 OnlyObsessionRemained 窗口里按 Ctrl+Shift+O 选一个目录，"
                           "或用 Ctrl+Shift+D 回内置"), true);
        }

        /** 外部（overlay）选中变化时同步。 */
        void setSelected (const juce::String& elementId)
        {
            if (elementId.isEmpty())
                return;   // 空选择不改变面板内容（否则会把行列表清空）

            // 编辑层里点中的是某个控件的图层：id 约定为 "控件id/图层id"，面板留在控件页。
            const auto owner = elementId.upToFirstOccurrenceOf ("/", false, false);

            if (owner.isNotEmpty() && owner != elementId)
            {
                const auto layerId = elementId.fromFirstOccurrenceOf ("/", false, false);

                if (document.controlLayerFor (owner, layerId).id.isNotEmpty())
                {
                    setSelectedControlLayer (owner, layerId, false);
                    return;
                }
            }

            // 编辑层里点中的是画布图层时，面板要切到图层页（重新打开设计器时也会走这里）
            if (document.layerFor (elementId).id.isNotEmpty())
            {
                setSelectedLayer (elementId);
                return;
            }

            layerMode = false;
            selectedControlLayer.clear();
            updateModeButtons();

            selected = elementId;

            for (int i = 0; i < kNumElements; ++i)
                if (elementId == kElements[i].id)
                    elementBox.setSelectedId (i + 1, juce::dontSendNotification);

            rebuildRows();
        }

        /** 选中某个控件图层：面板留在控件页，并滚到「控件图层」分区。 */
        void setSelectedControlLayer (const juce::String& ownerId, const juce::String& layerId,
                                      bool notify)
        {
            if (document.controlLayerFor (ownerId, layerId).id.isEmpty())
                return;

            layerMode = false;
            updateModeButtons();

            selected = ownerId;
            selectedControlLayer = layerId;

            for (int i = 0; i < kNumElements; ++i)
                if (ownerId == kElements[i].id)
                    elementBox.setSelectedId (i + 1, juce::dontSendNotification);

            if (notify && notifySelected)
                notifySelected (ownerId + "/" + layerId);

            rebuildRows();
        }

        /** 选中某个装饰图层：面板切到图层页并重建。 */
        void setSelectedLayer (const juce::String& layerId)
        {
            if (layerId.isEmpty())
                return;

            layerMode = true;
            selectedLayer = layerId;
            selectedControlLayer.clear();
            updateModeButtons();

            if (notifySelected)
                notifySelected (layerId);

            rebuildRows();
        }

        void refreshRows()
        {
            for (auto& row : rows)
                if (row.refresh)
                    row.refresh();
        }

        /** 皮肤换了：重读着色器行的文件（否则会把上一套皮肤的源码写到新皮肤上）。 */
        void reloadShaderSource()
        {
            if (shaderRow != nullptr)
                shaderRow->reloadFromDisk();
        }

        void resized() override
        {
            auto area = getLocalBounds().reduced (8);
            auto header = area.removeFromTop (26);
            prevButton.setBounds (header.removeFromLeft (24));
            header.removeFromLeft (3);
            nextButton.setBounds (header.removeFromLeft (24));
            header.removeFromLeft (6);
            elementBox.setBounds (header);

            area.removeFromTop (5);

            // 控件 / 图层：图层模式下元素下拉框仍然可用（选控件即退出图层模式）
            auto modeRow = area.removeFromTop (24);
            layerModeButton.setBounds (modeRow.removeFromRight (modeRow.getWidth() / 2));
            modeRow.removeFromRight (4);
            elementModeButton.setBounds (modeRow);

            area.removeFromTop (5);

            // 预设：选中一个已有方案即可加载 / 删除
            auto presetRow = area.removeFromTop (24);
            presetLoadButton  .setBounds (presetRow.removeFromRight (52));
            presetRow.removeFromRight (4);
            presetDeleteButton.setBounds (presetRow.removeFromRight (52));
            presetRow.removeFromRight (4);
            presetBox.setBounds (presetRow);

            area.removeFromTop (4);

            // 保存：输入名字后整份快照
            auto nameRow = area.removeFromTop (24);
            presetSaveButton.setBounds (nameRow.removeFromRight (86));
            nameRow.removeFromRight (4);
            presetNameEditor.setBounds (nameRow);

            area.removeFromTop (5);

            auto buttons = area.removeFromTop (24);
            saveButton  .setBounds (buttons.removeFromLeft (78));
            buttons.removeFromLeft (5);
            folderButton.setBounds (buttons.removeFromLeft (78));

            area.removeFromTop (5);
            statusLabel.setBounds (area.removeFromTop (32));
            area.removeFromTop (3);

            viewport.setBounds (area);
            layoutRows();
        }

        void paint (juce::Graphics& g) override
        {
            g.fillAll (juce::Colour (0xff232326));
        }

    private:
        struct Row
        {
            std::unique_ptr<juce::Label> name;
            std::unique_ptr<juce::Component> component;
            std::function<void()> refresh;
            bool isSection = false;
            int  height    = 0;   // 0 = 默认行高；高行（如着色器编辑区）在此显式给出
            int  section   = -1;  // 所属分区行的下标（分区行自己为 -1）
            juce::String title;   // 仅分区行使用：折叠状态以标题为键，这样换个元素重建后状态还在
            bool wide      = false;   // 没有左侧名称标签、组件占满整行（图层列表用）
        };

        /** 分区标题：整行可点击，用来折叠 / 展开该分区下面的行。 */
        class SectionHeader final : public juce::Component
        {
        public:
            SectionHeader (juce::String titleToDraw,
                           std::function<bool()> collapsedProvider,
                           std::function<void()> onToggle)
                : title (std::move (titleToDraw)),
                  isCollapsed (std::move (collapsedProvider)),
                  toggle (std::move (onToggle))
            {
                setMouseCursor (juce::MouseCursor::PointingHandCursor);
            }

            void paint (juce::Graphics& g) override
            {
                const auto green = juce::Colour (0xff0fdc78);
                const bool collapsed = isCollapsed != nullptr && isCollapsed();

                auto area = getLocalBounds();
                auto arrow = area.removeFromLeft (14);

                // 收起时 ▸（朝右），展开时 ▾（朝下）
                const float cx = static_cast<float> (arrow.getCentreX());
                const float cy = static_cast<float> (arrow.getCentreY());
                const float r  = 4.0f;

                juce::Path triangle;

                if (collapsed)
                    triangle.addTriangle (cx - r * 0.6f, cy - r,
                                          cx - r * 0.6f, cy + r,
                                          cx + r * 0.8f, cy);
                else
                    triangle.addTriangle (cx - r,        cy - r * 0.6f,
                                          cx + r,        cy - r * 0.6f,
                                          cx,            cy + r * 0.8f);

                g.setColour (green.withAlpha (collapsed ? 0.72f : 1.0f));
                g.fillPath (triangle);

                g.setFont (juce::FontOptions (13.0f, juce::Font::bold));
                g.drawText (title, area, juce::Justification::centredLeft);
            }

            void mouseDown (const juce::MouseEvent&) override
            {
                if (toggle != nullptr)
                    toggle();
            }

        private:
            juce::String title;
            std::function<bool()> isCollapsed;
            std::function<void()> toggle;
        };

        // ---- 行构建 ----

        void addSection (const juce::String& text)
        {
            auto header = std::make_unique<SectionHeader> (
                text,
                [this, text] { return collapsedSections.contains (text); },
                [this, text] { toggleSection (text); });

            auto* raw = header.get();
            content.addAndMakeVisible (raw);

            // 记住自己这一行的下标：后面的行要按它回查"我属于哪个分区、它收起了没"
            currentSection = static_cast<int> (rows.size());
            rows.push_back ({ nullptr, std::move (header), {}, true, 0, -1, text });
        }

        /** 折叠 / 展开一个分区。状态以标题为键，所以换个元素重建行列表后仍然保持。 */
        void toggleSection (const juce::String& title)
        {
            if (collapsedSections.contains (title))
                collapsedSections.removeString (title);
            else
                collapsedSections.add (title);

            layoutRows();

            // 三角的方向要跟着变
            for (auto& row : rows)
                if (row.isSection && row.title == title && row.component != nullptr)
                    row.component->repaint();
        }

        /** 该行是否因为所属分区被收起而应当隐藏。分区行自己永远显示。 */
        bool rowIsHidden (const Row& row) const
        {
            if (row.isSection || row.section < 0)
                return false;

            return collapsedSections.contains (rows[static_cast<size_t> (row.section)].title);
        }

        // 中文名一律从这里过一遍 UTF-8（调用处仍可直接写中文）
        void addRow (const char* name, std::unique_ptr<juce::Component> component,
                     std::function<void()> refresh)
        {
            addRow (U8 (name), std::move (component), std::move (refresh));
        }

        void addSlider (const char* name, double min, double max, double interval,
                        std::function<double()> get, std::function<void (double)> set)
        {
            addSlider (U8 (name), min, max, interval, std::move (get), std::move (set));
        }

        void addCombo (const char* name, const juce::StringArray& items,
                       std::function<juce::String()> get, std::function<void (const juce::String&)> set)
        {
            addCombo (U8 (name), items, std::move (get), std::move (set));
        }

        void addToggle (const char* name, std::function<bool()> get, std::function<void (bool)> set)
        {
            addToggle (U8 (name), std::move (get), std::move (set));
        }

        void addText (const char* name, std::function<juce::String()> get,
                      std::function<void (const juce::String&)> set)
        {
            addText (U8 (name), std::move (get), std::move (set));
        }

        /** 分区标题：调用处直接写中文，这里统一转 UTF-8。 */
        void section (const char* text) { addSection (U8 (text)); }

        /** 没有左侧名称标签的整行（图层列表这类内容需要完整的水平空间）。 */
        void addWideRow (std::unique_ptr<juce::Component> component,
                         std::function<void()> refresh, int height = 0)
        {
            content.addAndMakeVisible (component.get());

            rows.push_back ({ nullptr, std::move (component), std::move (refresh),
                              false, height, currentSection, {}, true });
        }

        /** 建一行；返回左侧名称标签，便于把当前值也写进去。height > 0 时为高行。 */
        juce::Label* addRow (const juce::String& name, std::unique_ptr<juce::Component> component,
                             std::function<void()> refresh, int height = 0)
        {
            auto label = std::make_unique<juce::Label>();
            label->setText (name, juce::dontSendNotification);
            label->setFont (juce::FontOptions (12.0f));
            label->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
            label->setJustificationType (juce::Justification::centredRight);

            content.addAndMakeVisible (label.get());
            content.addAndMakeVisible (component.get());

            auto* rawLabel = label.get();

            rows.push_back ({ std::move (label), std::move (component), std::move (refresh),
                              false, height, currentSection, {} });

            return rawLabel;
        }

        void addSlider (const juce::String& name, double min, double max, double interval,
                        std::function<double()> get, std::function<void (double)> set)
        {
            auto slider = std::make_unique<juce::Slider> (juce::Slider::LinearHorizontal,
                                                          juce::Slider::TextBoxRight);
            slider->setRange (min, max, interval);

            // 颜色必须在 setTextBoxStyle 之前设：JUCE 创建文本框时会把颜色烤进内部 Label
            // （见 LookAndFeel_V2::createSliderTextBox），之后再改就不生效了。
            slider->setColour (juce::Slider::textBoxTextColourId, juce::Colours::white);
            slider->setColour (juce::Slider::textBoxBackgroundColourId, juce::Colour (0xff141416));
            slider->setColour (juce::Slider::textBoxOutlineColourId, juce::Colour (0xff4a4a4e));

            slider->setTextBoxStyle (juce::Slider::TextBoxRight, false, 58, 18);
            slider->setColour (juce::Slider::trackColourId, juce::Colour (0xff3a86ff));
            slider->setColour (juce::Slider::backgroundColourId, juce::Colour (0xff2c2c2e));

            auto* raw = slider.get();
            raw->setValue (get(), juce::dontSendNotification);

            raw->onValueChange = [this, raw, set]
            {
                set (raw->getValue());
                markDirty();
            };

            // 数值直接写在行名上（例如「X   236」）：JUCE 滑块的数值框在深色主题下太不起眼，
            // 而设计器最需要的就是随时看到确切数字。
            auto* label = addRow (name,
                                  std::move (slider),
                                  [raw, get] { raw->setValue (get(), juce::dontSendNotification); });

            raw->onValueChange = [this, raw, set, label, name]
            {
                set (raw->getValue());
                label->setText (name + "   " + raw->getTextFromValue (raw->getValue()),
                                juce::dontSendNotification);
                markDirty();
            };

            // 外部（如清单被改、切元素）刷新时，数值框与行名都要跟着走
            rows.back().refresh = [raw, get, label, name]
            {
                raw->setValue (get(), juce::dontSendNotification);
                label->setText (name + "   " + raw->getTextFromValue (raw->getValue()),
                                juce::dontSendNotification);
            };

            label->setText (name + "   " + raw->getTextFromValue (raw->getValue()),
                            juce::dontSendNotification);
        }

        void addCombo (const juce::String& name, const juce::StringArray& items,
                       std::function<juce::String()> get, std::function<void (const juce::String&)> set)
        {
            auto box = std::make_unique<juce::ComboBox>();
            auto* raw = box.get();

            raw->addItemList (items, 1);
            raw->setText (get(), juce::dontSendNotification);

            raw->onChange = [this, raw, set]
            {
                set (raw->getText());
                markDirty();
            };

            addRow (name, std::move (box), [raw, get] { raw->setText (get(), juce::dontSendNotification); });
        }

        void addToggle (const juce::String& name, std::function<bool()> get, std::function<void (bool)> set)
        {
            auto button = std::make_unique<juce::TextButton> (U8 ("切换"));
            auto* raw = button.get();

            raw->setClickingTogglesState (true);
            raw->setToggleState (get(), juce::dontSendNotification);

            raw->onClick = [this, raw, set]
            {
                set (raw->getToggleState());
                markDirty();
            };

            addRow (name, std::move (button),
                    [raw, get] { raw->setToggleState (get(), juce::dontSendNotification); });
        }

        void addText (const juce::String& name, std::function<juce::String()> get,
                      std::function<void (const juce::String&)> set)
        {
            auto editor = std::make_unique<juce::TextEditor>();
            auto* raw = editor.get();

            raw->setText (get(), juce::dontSendNotification);
            raw->setMultiLine (false);
            raw->setReturnKeyStartsNewLine (false);
            raw->onTextChange = [this, raw, set] { set (raw->getText()); markDirty(); };

            addRow (name, std::move (editor), [raw, get] { raw->setText (get(), false); });
        }

        /** 贴图槽位行：显示当前文件 + 导入/清除。 */
        class AssetRow final : public juce::Component
        {
        public:
            AssetRow (const juce::String& slotName,
                      juce::String emptyHintText = U8 ("（内置矢量画法）"))
                : slot (slotName), emptyHint (std::move (emptyHintText))
            {
                addAndMakeVisible (importButton);
                addAndMakeVisible (vectorButton);
                addAndMakeVisible (clearButton);

                importButton.onClick = [this] { if (onImport) onImport(); };
                vectorButton.onClick = [this] { if (onVector) onVector(); };
                clearButton.onClick  = [this] { if (onClear) onClear(); };

                vectorButton.setTooltip (U8 ("用内置矢量绘图创建图形（存成皮肤内的 SVG 资源）"));
            }

            void setPath (const juce::String& newPath)
            {
                path = newPath;
                clearButton.setEnabled (path.isNotEmpty());
                repaint();
            }

            std::function<void()> onImport, onVector, onClear;

            void resized() override
            {
                auto area = getLocalBounds();
                clearButton .setBounds (area.removeFromRight (42));
                area.removeFromRight (3);
                vectorButton.setBounds (area.removeFromRight (54));
                area.removeFromRight (3);
                importButton.setBounds (area.removeFromRight (52));
            }

            void paint (juce::Graphics& g) override
            {
                g.setColour (path.isNotEmpty() ? juce::Colours::white.withAlpha (0.75f)
                                               : juce::Colours::white.withAlpha (0.3f));
                g.setFont (juce::FontOptions (11.0f));
                g.drawText (path.isEmpty() ? emptyHint : path,
                            getLocalBounds().withTrimmedRight (160), juce::Justification::centredLeft, true);
            }

        private:
            juce::String slot, path, emptyHint;
            // 注意：构造函数初始化列表里的字面量同样要过 U8()——这里曾经漏了，
            // 于是「导入…」「清除」按 Latin-1 解释成了乱码。
            juce::TextButton importButton { U8 ("导入…") }, vectorButton { U8 ("矢量…") },
                             clearButton { U8 ("清除") };
        };

        /**
            图层面板里的一行：整行可点（选中），右侧三个按钮管显示与顺序。
            点按钮时不能再依赖"行被选中"这种隐式状态，所以每个按钮都把图层 id
            绑在回调里——否则用户在 A 行点「↑」，动的可能是 B 层。
        */
        class LayerRow final : public juce::Component
        {
        public:
            LayerRow()
            {
                addAndMakeVisible (visibleButton);
                addAndMakeVisible (upButton);
                addAndMakeVisible (downButton);

                visibleButton.onClick = [this] { if (onToggleVisible) onToggleVisible(); };
                upButton.onClick      = [this] { if (onMoveUp) onMoveUp(); };
                downButton.onClick    = [this] { if (onMoveDown) onMoveDown(); };

                visibleButton.setTooltip (U8 ("显示 / 隐藏该图层"));
                upButton.setTooltip (U8 ("向前移动（列表上方 = 更靠前）"));
                downButton.setTooltip (U8 ("向后移动（列表下方 = 更靠后）"));

                setMouseCursor (juce::MouseCursor::PointingHandCursor);
            }

            void setInfo (const juce::String& displayName, bool isVisible, bool isSelected)
            {
                name = displayName;
                visible = isVisible;
                selected = isSelected;

                // 按钮文案描述"点下去会发生什么"，而不是当前状态：
                // 状态由行首的 ●/○ 表达，按钮写反最容易被误读。
                visibleButton.setButtonText (visible ? U8 ("隐藏") : U8 ("显示"));
                repaint();
            }

            std::function<void()> onSelect, onToggleVisible, onMoveUp, onMoveDown;

            void mouseDown (const juce::MouseEvent&) override
            {
                if (onSelect)
                    onSelect();
            }

            void resized() override
            {
                auto area = getLocalBounds().reduced (2);
                visibleButton.setBounds (area.removeFromRight (44));
                area.removeFromRight (4);
                upButton.setBounds (area.removeFromRight (24));
                area.removeFromRight (3);
                downButton.setBounds (area.removeFromRight (24));
            }

            void paint (juce::Graphics& g) override
            {
                const auto area = getLocalBounds().toFloat();

                if (selected)
                {
                    g.setColour (juce::Colour (0xff3a86ff).withAlpha (0.26f));
                    g.fillRoundedRectangle (area, 3.0f);

                    g.setColour (juce::Colour (0xff3a86ff));
                    g.drawRoundedRectangle (area.reduced (0.5f), 3.0f, 1.0f);
                }

                const auto right = visibleButton.getWidth() + upButton.getWidth()
                                   + downButton.getWidth() + 20;

                g.setColour (visible ? juce::Colours::white.withAlpha (0.85f)
                                     : juce::Colours::white.withAlpha (0.35f));
                g.setFont (juce::FontOptions (12.0f));
                g.drawText ((visible ? juce::String (U8 ("● ")) : juce::String (U8 ("○ "))) + name,
                            getLocalBounds().withTrimmedRight (right),
                            juce::Justification::centredLeft, true);
            }

        private:
            juce::String name;
            bool visible = true, selected = false;

            juce::TextButton visibleButton, upButton { U8 ("↑") }, downButton { U8 ("↓") };
        };

        /** 图层页顶部的动作：新建 / 导入图片 /（控件图层）矢量图形 / 删除。 */
        class LayerActionsRow final : public juce::Component
        {
        public:
            LayerActionsRow (bool forControlLayer = false)
            {
                addButton    .setButtonText (U8 ("新建图层"));
                importButton .setButtonText (U8 ("导入图片…"));
                vectorButton .setButtonText (U8 ("矢量图形…"));
                deleteButton .setButtonText (U8 ("删除图层"));

                addButton   .setTooltip (forControlLayer ? U8 ("在这个控件的范围内新建一个空图层")
                                                         : U8 ("在画布中部新建一个空图层"));
                importButton.setTooltip (forControlLayer ? U8 ("没有选中图层时会自动新建一个，再把图片作为它的贴图")
                                                         : U8 ("把图片拷进皮肤资源并作为当前图层的贴图"));
                vectorButton.setTooltip (U8 ("没有选中图层时会自动新建一个，再画一个矢量图形作为它的贴图"));
                deleteButton.setTooltip (forControlLayer ? U8 ("删除当前选中的控件图层")
                                                         : U8 ("删除当前选中的图层"));

                for (auto* b : { &addButton, &importButton, &vectorButton, &deleteButton })
                    addAndMakeVisible (*b);

                // 矢量图形只服务控件自定义（画布图层仍走属性区的兼容入口）
                vectorButton.setVisible (forControlLayer);

                addButton.onClick    = [this] { if (onAdd) onAdd(); };
                importButton.onClick = [this] { if (onImport) onImport(); };
                vectorButton.onClick = [this] { if (onVector) onVector(); };
                deleteButton.onClick = [this] { if (onDelete) onDelete(); };
            }

            void setCanDelete (bool canDelete) { deleteButton.setEnabled (canDelete); }

            std::function<void()> onAdd, onImport, onVector, onDelete;

            void resized() override
            {
                auto area = getLocalBounds();

                const int count = vectorButton.isVisible() ? 4 : 3;
                const int gap   = 4;
                const int width = juce::jmax (0, (area.getWidth() - gap * (count - 1)) / count);

                addButton.setBounds (area.removeFromLeft (width));

                area.removeFromLeft (gap);
                importButton.setBounds (area.removeFromLeft (width));

                if (vectorButton.isVisible())
                {
                    area.removeFromLeft (gap);
                    vectorButton.setBounds (area.removeFromLeft (width));
                }

                area.removeFromLeft (gap);
                deleteButton.setBounds (area);
            }

        private:
            juce::TextButton addButton, importButton, vectorButton, deleteButton;
        };

        void addAssetRow (const juce::String& slot)
        {
            auto row = std::make_unique<AssetRow> (slot);
            auto* raw = row.get();

            raw->onImport = [this, slot] { importAsset (slot); };
            raw->onVector = [this, slot]
            {
                openVectorEditor (slot, [this, slot] (const juce::String& relative)
                {
                    document.setAsset (selected, slot, relative);
                });
            };
            raw->onClear  = [this, slot]
            {
                document.setAsset (selected, slot, {});
                markDirty();
                refreshRows();
                setStatus (U8 ("已清除 ") + slot, false);
            };

            addRow (slot, std::move (row), [this, raw, slot]
            {
                raw->setPath (document.assetFor (selected, slot));
            });
        }

        /**
            着色器编辑区：路径条 + 导入 / 应用 / 还原 + 多行编辑器 + 编译状态。

            编译是异步的——真正的编译发生在"下一次绘制"里（见 ShaderBackground），
            所以这里用定时器轮询编译结果，而不是应用后立刻断言成功。

            注意：本行的 refresh 刻意为空。刷新时会重读整行控件，若顺手把编辑器内容也
            从磁盘重载，用户在编辑器里改了但还没点"应用"的内容就会被静默丢掉。
            只有"皮肤换了"这种确实该重置的场合才走 reloadFromDisk()。
        */
        class ShaderSourceRow final : public juce::Component,
                                     private juce::Timer
        {
        public:
            ShaderSourceRow (SkinDocument& doc,
                             std::function<juce::String()> errorProvider,
                             std::function<juce::String()> runtimeStatusProvider,
                             std::function<void()> onManifestChanged)
                : document (doc),
                  getCompileError (std::move (errorProvider)),
                  getRuntimeStatus (std::move (runtimeStatusProvider)),
                  notifyManifestChanged (std::move (onManifestChanged))
            {
                editor.setMultiLine (true, true);
                editor.setReturnKeyStartsNewLine (true);
                editor.setFont (juce::Font (juce::FontOptions ("Consolas", 12.5f, juce::Font::plain)));
                editor.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff141416));
                editor.setColour (juce::TextEditor::textColourId, juce::Colour (0xffe5e5e7));
                editor.setColour (juce::TextEditor::outlineColourId, juce::Colour (0xff2c2c2e));
                addAndMakeVisible (editor);

                for (auto* b : { &importButton, &applyButton, &revertButton })
                {
                    b->setButtonText (b == &importButton ? U8 ("导入 .glsl…")
                                                         : b == &applyButton ? U8 ("应用")
                                                                             : U8 ("还原"));
                    addAndMakeVisible (*b);
                }

                importButton.onClick = [this] { chooseShaderFile(); };
                applyButton .onClick = [this] { apply(); };
                revertButton.onClick = [this] { reloadFromDisk(); };

                pathLabel.setFont (juce::FontOptions (11.0f));
                pathLabel.setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.72f));
                addAndMakeVisible (pathLabel);

                statusLabel.setFont (juce::FontOptions (11.5f));
                statusLabel.setJustificationType (juce::Justification::centredLeft);
                addAndMakeVisible (statusLabel);

                reloadFromDisk();
                startTimer (500);   // 轮询编译结果与运行状态
            }

            /** 从磁盘重读（导入后、或皮肤换了之后）。会丢弃编辑器里未"应用"的内容。 */
            void reloadFromDisk()
            {
                sourcePath = document.shaderSourcePath();

                pathLabel.setText (sourcePath.isEmpty()
                                       ? U8 ("（这套皮肤还没有着色器，点「导入 .glsl…」开始）")
                                       : U8 ("文件：") + sourcePath,
                                   juce::dontSendNotification);

                editor.setText (sourcePath.isEmpty() ? juce::String()
                                                     : document.readSkinFile (sourcePath), false);
                repaint();
            }

            void resized() override
            {
                auto area = getLocalBounds();

                auto top = area.removeFromTop (26);

                constexpr int bw = 74, gap = 4;

                revertButton.setBounds (top.removeFromRight (bw));
                top.removeFromRight (gap);
                applyButton .setBounds (top.removeFromRight (bw));
                top.removeFromRight (gap);
                importButton.setBounds (top.removeFromRight (bw));

                pathLabel.setBounds (top.withTrimmedRight (gap + 6));

                statusLabel.setBounds (area.removeFromBottom (20));
                area.removeFromBottom (3);
                editor.setBounds (area);
            }

        private:
            void chooseShaderFile()
            {
                fileChooser = std::make_unique<juce::FileChooser> (U8 ("选择着色器（.glsl）"),
                                                                   juce::File::getSpecialLocation (
                                                                       juce::File::userDocumentsDirectory),
                                                                   "*.glsl;*.frag;*.txt");

                fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                              | juce::FileBrowserComponent::canSelectFiles,
                                          [this] (const juce::FileChooser& chooser)
                {
                    const auto file = chooser.getResult();

                    if (! file.existsAsFile())
                        return;

                    juce::String error;
                    const auto relative = document.importShader (file, error);

                    if (relative.isEmpty())
                    {
                        setStatus (U8 ("导入失败：") + error, true);
                        return;
                    }

                    document.setShaderSourcePath (relative);

                    if (notifyManifestChanged)
                        notifyManifestChanged();

                    reloadFromDisk();
                    setStatus (U8 ("已导入 ") + relative, false);
                });
            }

            void apply()
            {
                juce::String error;

                if (sourcePath.isEmpty())
                {
                    setStatus (U8 ("先点「导入 .glsl…」选一个文件"), true);
                    return;
                }

                if (! document.writeSkinFile (sourcePath, editor.getText(), error))
                {
                    setStatus (error, true);
                    return;
                }

                // 让皮肤重新载入：ShaderBackground 会读到新源码并在下一次绘制里编译
                juce::String reloadError;

                if (! skin::SkinBundle::getInstance().reload (reloadError))
                {
                    setStatus (U8 ("皮肤重载失败：") + reloadError, true);
                    return;
                }

                setStatus (U8 ("已应用，正在重编译…"), false);
                ticks = 0;
                startTimer (400);
            }

            void timerCallback() override
            {
                // 编译错误优先显示；没有错误则显示运行状态（GPU 就绪 / 已降级 / 未启用）
                if (const auto error = getCompileError(); error.isNotEmpty())
                {
                    setStatus (U8 ("编译失败：") + error, true);
                    return;
                }

                const auto runtime = getRuntimeStatus ? getRuntimeStatus() : juce::String();

                if (ticks >= 0)
                {
                    // 应用后的短暂观察窗：连续几拍都没错误才算"编译通过"
                    if (++ticks >= 8)
                    {
                        ticks = -1;
                        setStatus (runtime.isEmpty() ? U8 ("已应用") : U8 ("已应用 · ") + runtime, false);
                    }
                    else
                    {
                        setStatus (U8 ("已应用，正在重编译…"), false);
                    }

                    return;
                }

                setStatus (runtime, false);
            }

            void setStatus (const juce::String& text, bool isError)
            {
                statusLabel.setText (text, juce::dontSendNotification);
                statusLabel.setColour (juce::Label::textColourId,
                                       isError ? juce::Colours::orange : juce::Colour (0xff0fdc78));
            }

            SkinDocument& document;
            std::function<juce::String()> getCompileError;
            std::function<juce::String()> getRuntimeStatus;
            std::function<void()> notifyManifestChanged;

            juce::TextEditor editor;
            juce::TextButton importButton, applyButton, revertButton;
            juce::Label      pathLabel, statusLabel;

            juce::String sourcePath;
            int          ticks = -1;   // -1 = 未处于"应用后观察窗"；>=0 = 剩余观察拍数

            std::unique_ptr<juce::FileChooser> fileChooser;
        };

        void addShaderEditor()
        {
            auto row = std::make_unique<ShaderSourceRow> (document, shaderCompileError,
                                                          shaderRuntimeStatus,
                                                          [this] { flush(); });

            shaderRow = row.get();

            // refresh 刻意留空：见 ShaderSourceRow 的说明（避免静默丢弃未应用的编辑）
            addRow (U8 ("源码"), std::move (row), {}, 320);
        }

        // ---- 内容刷新 ----

        juce::Rectangle<int> currentRect() const
        {
            const auto fallback = editorBounds ? editorBounds (selected) : juce::Rectangle<int>();
            return document.rectFor (selected, fallback);
        }

        void applyRect (juce::Rectangle<int> rect)
        {
            document.setRect (selected, rect);

            if (notifyChanged)
                notifyChanged();
        }

        void setParam (const juce::String& key, const juce::var& value)
        {
            document.setParam (selected, key, value);

            if (notifyChanged)
                notifyChanged();
        }

        /** 当前元素的变换（面板里几个成对的滑块改其中一半时，需要另一半的现值）。 */
        Transform transform() const { return document.transformFor (selected); }

        void setTransform (const juce::String& key, const juce::var& value)
        {
            document.setTransformValue (selected, key, value);

            if (notifyChanged)
                notifyChanged();
        }

        void rebuildRows()
        {
            // 顺序很重要：先把控件从 content 上摘下来，再释放 rows 持有的所有权。
            // 反过来写的话 rows.clear() 会先析构控件，而 content 仍持有它们的裸指针，
            // 随后的 removeAllChildren() 就会踩野指针（表现为面板空白）。
            content.removeAllChildren();
            rows.clear();

            // rows 里的控件已经全部析构：着色器行的裸指针必须同步作废。
            // 否则切到图层页（不再建着色器行）后，下一次换皮肤触发 onSkinChanged()，
            // reloadShaderSource() 就会通过野指针访问已释放的对象（崩溃在 SkinDocument::manifest）。
            shaderRow = nullptr;

            currentSection = -1;

            if (layerMode)
            {
                buildLayerRows();
                refreshRows();
                layoutRows();
                return;
            }

            const auto* info = findInfo (selected);

            if (info == nullptr)
                return;

            const auto type = juce::String (info->type);

            // ---- 布局 ----
            section ("布局（画布坐标，可用方向键微调；拖动预览里的元素同样改这里）");

            addSlider ("X", -3200.0, 3200.0, 1.0,
                       [this] { return static_cast<double> (currentRect().getX()); },
                       [this] (double v) { auto r = currentRect(); r.setX (static_cast<int> (v)); applyRect (r); });

            addSlider ("Y", -3200.0, 3200.0, 1.0,
                       [this] { return static_cast<double> (currentRect().getY()); },
                       [this] (double v) { auto r = currentRect(); r.setY (static_cast<int> (v)); applyRect (r); });

            addSlider (U8 ("宽"), 8.0, 1600.0, 1.0,
                       [this] { return static_cast<double> (currentRect().getWidth()); },
                       [this] (double v) { auto r = currentRect(); r.setWidth (static_cast<int> (v)); applyRect (r); });

            addSlider (U8 ("高"), 8.0, 1600.0, 1.0,
                       [this] { return static_cast<double> (currentRect().getHeight()); },
                       [this] (double v) { auto r = currentRect(); r.setHeight (static_cast<int> (v)); applyRect (r); });

            // ---- 图层与变换 ----
            // 对任何元素都适用：旋转 / 锚点 / 平移 / 缩放是画贴图时套的变换，
            // 所以"斜着摆一排旋钮"或"贴图转个角度"都只改这里。
            section (kTransformSectionTitle);

            addSlider (U8 ("旋转角"), -180.0, 180.0, 1.0,
                       [this] { return static_cast<double> (document.transformFor (selected).rotation); },
                       [this] (double v) { setTransform ("rotation", v); });

            addSlider (U8 ("锚点 X"), 0.0, 1.0, 0.05,
                       [this] { return static_cast<double> (document.transformFor (selected).pivot.x); },
                       [this] (double v) { setTransform ("pivot", pairVar (v, transform().pivot.y)); });

            addSlider (U8 ("锚点 Y"), 0.0, 1.0, 0.05,
                       [this] { return static_cast<double> (document.transformFor (selected).pivot.y); },
                       [this] (double v) { setTransform ("pivot", pairVar (transform().pivot.x, v)); });

            addSlider (U8 ("平移 X"), -400.0, 400.0, 1.0,
                       [this] { return static_cast<double> (document.transformFor (selected).offset.x); },
                       [this] (double v) { setTransform ("offset", pairVar (v, transform().offset.y)); });

            addSlider (U8 ("平移 Y"), -400.0, 400.0, 1.0,
                       [this] { return static_cast<double> (document.transformFor (selected).offset.y); },
                       [this] (double v) { setTransform ("offset", pairVar (transform().offset.x, v)); });

            addSlider (U8 ("缩放 X"), 0.05, 4.0, 0.05,
                       [this] { return static_cast<double> (document.transformFor (selected).scale.x); },
                       [this] (double v) { setTransform ("scale", pairVar (v, transform().scale.y)); });

            addSlider (U8 ("缩放 Y"), 0.05, 4.0, 0.05,
                       [this] { return static_cast<double> (document.transformFor (selected).scale.y); },
                       [this] (double v) { setTransform ("scale", pairVar (transform().scale.x, v)); });

            addCombo (U8 ("缩放模式"), fitChoices(),
                      [this] { const auto f = document.transformFor (selected).fit;
                               return f.isEmpty() ? fitDefaultValue() : f; },
                      [this] (const juce::String& v)
                      { setTransform ("fit", v == fitDefaultValue() ? juce::var() : juce::var (v)); });

            // ---- 运动轨迹 ----
            // 这里改的 P0…P3 会原样写进清单，运行时 Skin::travelForPart 读的就是同一份数据，
            // 所以面板/编辑层画的曲线 = 控件实际走的路径。
            section (kTravelSectionTitle);

            // 换了元素就回到「整体」，免得上一个元素的部位名套到新元素上
            if (travelPartOwner != selected)
            {
                travelPartOwner = selected;
                travelPart.clear();

                if (notifyTravelPart)
                    notifyTravelPart (travelPart);
            }

            juce::StringArray partChoices;
            partChoices.add (U8 ("（整体）"));
            partChoices.addArray (document.travelPartsFor (selected));

            for (const auto& slot : slotsForType (type))
                partChoices.addIfNotAlreadyThere (slot);

            addCombo (U8 ("部位"), partChoices,
                      [this] { return travelPart.isEmpty() ? juce::String (U8 ("（整体）")) : travelPart; },
                      [this] (const juce::String& v)
                      {
                          travelPart = (v == U8 ("（整体）")) ? juce::String() : v;

                          // 让编辑层知道现在编辑的是哪条轨迹：重合的控制点归它接管。
                          if (notifyTravelPart)
                              notifyTravelPart (travelPart);

                          // 只换数值，不重建行：这个回调正处在 ComboBox 自己的 onChange 里，
                          // 重建会把当前控件析构掉。
                          refreshRows();
                      });

            addToggle (U8 ("启用此轨迹"),
                       [this] { return document.travelForPart (selected, travelPart).enabled; },
                       [this] (bool v) { document.setTravelEnabled (selected, travelPart, v); });

            auto pointOf = [this] (int index)
            {
                const auto travel = document.travelForPart (selected, travelPart);
                const juce::Point<float> points[4] { travel.p0, travel.p1, travel.p2, travel.p3 };

                return points[juce::jlimit (0, 3, index)];
            };

            for (int i = 0; i < 4; ++i)
            {
                addSlider ("P" + juce::String (i) + " X", 0.0, 1.0, 0.01,
                           [pointOf, i] { return static_cast<double> (pointOf (i).x); },
                           [this, pointOf, i] (double v)
                           {
                               auto p = pointOf (i);
                               p.x = static_cast<float> (v);
                               document.setTravelPoint (selected, travelPart, i, p);
                           });

                addSlider ("P" + juce::String (i) + " Y", 0.0, 1.0, 0.01,
                           [pointOf, i] { return static_cast<double> (pointOf (i).y); },
                           [this, pointOf, i] (double v)
                           {
                               auto p = pointOf (i);
                               p.y = static_cast<float> (v);
                               document.setTravelPoint (selected, travelPart, i, p);
                           });
            }

            // 编辑层直接拖控制点：这里给一个回到默认直线的出口
            {
                auto button = std::make_unique<juce::TextButton> (U8 ("重置此轨迹"));
                auto* raw = button.get();

                raw->onClick = [this]
                {
                    document.resetTravel (selected, travelPart);
                    markDirty();
                    refreshRows();
                    setStatus (U8 ("已重置轨迹（回到默认直线）"), false);
                };

                addRow (U8 ("轨迹"), std::move (button), {});
            }

            // ---- 形态与交互 ----
            if (type == "knob")
            {
                section ("形态与操作逻辑");

                addCombo (U8 ("形态"), styleChoices(),
                          [this] { return document.paramFor (selected, "style", juce::var ("rotary")).toString(); },
                          [this] (const juce::String& v) { setParam ("style", v); });

                addCombo (U8 ("拖动方式"), dragModeChoices(),
                          [this] { return document.paramFor (selected, "dragMode", juce::var ("auto")).toString(); },
                          [this] (const juce::String& v) { setParam ("dragMode", v); });

                addSlider (U8 ("拖动灵敏度"), 10.0, 800.0, 5.0,
                           [this] { return document.paramFor (selected, "sensitivity", juce::var (200)).toString().getDoubleValue(); },
                           [this] (double v) { setParam ("sensitivity", static_cast<int> (v)); });

                addSlider (U8 ("行程内缩"), 0.0, 200.0, 1.0,
                           [this] { return document.paramFor (selected, "trackInset", juce::var (0)).toString().getDoubleValue(); },
                           [this] (double v) { setParam ("trackInset", static_cast<int> (v)); });

                addSlider (U8 ("旋钮起点角度"), 0.0, 720.0, 1.0,
                           [this] { return document.paramFor (selected, "angleStart", juce::var (216)).toString().getDoubleValue(); },
                           [this] (double v) { setParam ("angleStart", v); });

                addSlider (U8 ("旋钮终点角度"), 0.0, 720.0, 1.0,
                           [this] { return document.paramFor (selected, "angleEnd", juce::var (504)).toString().getDoubleValue(); },
                           [this] (double v) { setParam ("angleEnd", v); });

                addCombo (U8 ("数值框方位"), valueBoxChoices(),
                          [this] { return document.paramFor (selected, "valueBox", juce::var ("below")).toString(); },
                          [this] (const juce::String& v) { setParam ("valueBox", v); });

                addSlider (U8 ("数值框宽"), 0.0, 240.0, 1.0,
                           [this] { return document.paramFor (selected, "valueBoxWidth", juce::var (72)).toString().getDoubleValue(); },
                           [this] (double v) { setParam ("valueBoxWidth", static_cast<int> (v)); });

                addSlider (U8 ("数值框高"), 0.0, 80.0, 1.0,
                           [this] { return document.paramFor (selected, "valueBoxHeight", juce::var (18)).toString().getDoubleValue(); },
                           [this] (double v) { setParam ("valueBoxHeight", static_cast<int> (v)); });

                addToggle (U8 ("滚轮可调"), 
                           [this] { return static_cast<bool> (document.paramFor (selected, "scrollEnabled", juce::var (true))); },
                           [this] (bool v) { setParam ("scrollEnabled", v); });

                addToggle (U8 ("双击复位到默认值"),
                           [this] { return static_cast<bool> (document.paramFor (selected, "doubleClickReset", juce::var (false))); },
                           [this] (bool v) { setParam ("doubleClickReset", v); });
            }

            // ---- 文案 ----
            if (type == "label" || type == "button" || type == "textEditor")
            {
                section ("文案");

                addText (U8 ("文本"), [this] { return document.textFor (selected); },
                         [this] (const juce::String& v) { document.setText (selected, v); });
            }

            // ---- 贴图 ----
            const auto slots = slotsForType (type);

            if (! slots.isEmpty())
            {
                section ("贴图（导入的图片会拷进皮肤的 assets/ 目录）");

                for (const auto& slot : slots)
                    addAssetRow (slot);
            }

            // ---- 控件图层（叠加在控件之上，随控件移动 / 缩放 / 旋转）----
            buildControlLayerRows();

            // ---- 窗口与异形（阶段三 3e）----
            buildWindowRows();

            // ---- 动态着色器（阶段三 3c）----
            // 对任何元素都显示同一份着色器编辑区；放最后是因为它是个高块，
            // 放前面会把该元素自己的参数挤到下面去。默认收起（标题可点击展开）。
            section (kShaderSectionTitle);

            addSlider ("帧率", 5.0, 120.0, 1.0,
                       [this] { return document.shaderParam ("fps", juce::var (30))
                                            .toString().getDoubleValue(); },
                       [this] (double v) { document.setShaderParam ("fps", static_cast<int> (v)); });

            addShaderEditor();

            refreshRows();
            layoutRows();
        }

        void layoutRows()
        {
            // 内容宽度只能由"视口自己的宽度"算出来，不能问"内容现在多宽"：
            // JUCE 的 Viewport::getMaximumVisibleWidth() 返回的是内部 contentHolder 的宽度，
            // 而 contentHolder = max(内容宽, 可见宽)。一旦内容比可见区宽，这个值就被内容撑着，
            // 目标宽度变成"内容宽 - 4"，每轮布局只缩 4 px、收不回来。
            // 这里预留垂直滚动条（与 Viewport::updateVisibleArea 的算法一致），结果与内容当前宽度无关。
            const int width  = juce::jmax (240, viewport.getWidth()
                                                - viewport.getScrollBarThickness() - 4);
            const int column = 156;   // 够放「旋钮终点角度   504」这样的行名

            int y = 0;

            // 行高要够：滑块的数值框高度会被 JUCE 压到 min(设定高度, 行高-15)，
            // 行高 22 时数值框只剩 7 px、文字基本看不见。行高 30 后数值框是完整的 18 px。
            constexpr int rowHeight = 30;
            constexpr int rowStride = 34;

            for (auto& row : rows)
            {
                // 分区收起时它下面的行整段不占位置，也不接受点击
                const bool hidden = rowIsHidden (row);

                if (row.name != nullptr)      row.name->setVisible (! hidden);
                if (row.component != nullptr) row.component->setVisible (! hidden);

                if (hidden)
                    continue;

                if (row.isSection)
                {
                    row.component->setBounds (0, y + 8, width, 18);
                    y += 32;
                }
                else if (row.height > 0)
                {
                    // 高行（着色器编辑区）：名称只在顶部对齐，组件占满整行高度
                    if (row.name != nullptr)
                        row.name->setBounds (0, y, column - 8, rowHeight);

                    if (row.wide)
                        row.component->setBounds (0, y, width, row.height);
                    else
                        row.component->setBounds (column, y, width - column, row.height);

                    y += row.height + 4;
                }
                else
                {
                    if (row.name != nullptr)
                        row.name->setBounds (0, y, column - 8, rowHeight);

                    if (row.wide)
                        row.component->setBounds (0, y, width, rowHeight);
                    else
                        row.component->setBounds (column, y, width - column, rowHeight);

                    y += rowStride;
                }
            }

            content.setSize (width, y + 8);
        }

        void stepElement (int delta)
        {
            const int index = juce::jlimit (0, kNumElements - 1, elementBox.getSelectedId() - 1 + delta);
            elementBox.setSelectedId (index + 1, juce::sendNotification);
        }

        // ---- 面板模式 ----

        void updateModeButtons()
        {
            auto apply = [] (juce::TextButton& button, bool active)
            {
                button.setColour (juce::TextButton::buttonColourId,
                                  active ? juce::Colour (0xff3a86ff) : juce::Colour (0xff2c2c2e));
                button.setColour (juce::TextButton::textColourOffId,
                                  active ? juce::Colours::white
                                         : juce::Colours::lightgrey.withAlpha (0.75f));
                button.setColour (juce::TextButton::textColourOnId, juce::Colours::white);
            };

            apply (elementModeButton, ! layerMode);
            apply (layerModeButton, layerMode);
        }

        /** 从别的控件回调里重建行列表（会析构发起点击的控件）时必须延后一拍。 */
        void scheduleRebuild()
        {
            if (rebuildPending)
                return;

            rebuildPending = true;

            juce::Component::SafePointer<Panel> safeThis (this);

            juce::MessageManager::callAsync ([safeThis]
            {
                if (safeThis == nullptr)
                    return;

                safeThis->rebuildPending = false;
                safeThis->rebuildRows();
            });
        }

        void enterLayerMode()
        {
            layerMode = true;
            selectedControlLayer.clear();
            updateModeButtons();

            // 没选过图层（或原图层已被删）：默认选列表最上面一层，点开就能直接拖
            if (document.layerFor (selectedLayer).id.isEmpty())
            {
                const auto all = document.layers();
                selectedLayer = all.empty() ? juce::String() : all.back().id;
            }

            if (notifySelected)
                notifySelected (selectedLayer);

            rebuildRows();
        }

        void selectLayerFromList (const juce::String& layerId)
        {
            if (layerId.isEmpty() || layerId == selectedLayer)
                return;

            selectedLayer = layerId;

            if (notifySelected)
                notifySelected (layerId);

            scheduleRebuild();
        }

        void toggleLayerVisible (const juce::String& layerId)
        {
            const auto info = document.layerFor (layerId);

            if (info.id.isEmpty())
                return;

            document.setLayerValue (layerId, "visible", ! info.visible);
            markDirty();
            scheduleRebuild();

            setStatus (info.visible ? U8 ("已隐藏图层「") + info.name + U8 ("」")
                                    : U8 ("已显示图层「") + info.name + U8 ("」"), false);
        }

        void moveLayerInList (const juce::String& layerId, int delta)
        {
            if (! document.moveLayer (layerId, delta))
                return;

            markDirty();
            scheduleRebuild();
        }

        void addNewLayer()
        {
            const auto id = document.addLayer();

            if (id.isEmpty())
            {
                setStatus (U8 ("新建图层失败：清单里没有可写的 layers 数组"), true);
                return;
            }

            selectedLayer = id;
            markDirty();

            if (notifySelected)
                notifySelected (id);

            scheduleRebuild();
            setStatus (U8 ("已新建图层（可拖动编辑层里的虚线框）"), false);
        }

        void deleteSelectedLayer()
        {
            if (selectedLayer.isEmpty())
            {
                setStatus (U8 ("先选中一个图层"), true);
                return;
            }

            const auto info = document.layerFor (selectedLayer);

            if (info.id.isEmpty())
                return;

            const auto id   = selectedLayer;
            const auto name = info.name.isNotEmpty() ? info.name : info.id;

            const auto options = juce::MessageBoxOptions()
                                     .withIconType (juce::MessageBoxIconType::WarningIcon)
                                     .withTitle (U8 ("删除图层"))
                                     .withMessage (U8 ("确定删除图层「") + name + U8 ("」吗？"))
                                     .withButton (U8 ("删除"))
                                     .withButton (U8 ("取消"));

            juce::Component::SafePointer<Panel> safeThis (this);

            juce::AlertWindow::showAsync (options, [safeThis, id, name] (int result)
            {
                if (result != 1 || safeThis == nullptr)
                    return;

                safeThis->document.removeLayer (id);

                const auto remaining = safeThis->document.layers();
                safeThis->selectedLayer = remaining.empty() ? juce::String() : remaining.back().id;

                safeThis->markDirty();

                if (safeThis->notifySelected)
                    safeThis->notifySelected (safeThis->selectedLayer);

                safeThis->scheduleRebuild();
                safeThis->setStatus (U8 ("已删除图层「") + name + U8 ("」"), false);
            });
        }

        void importLayerImage()
        {
            if (selectedLayer.isEmpty())
            {
                setStatus (U8 ("先选中一个图层"), true);
                return;
            }

            fileChooser = std::make_unique<juce::FileChooser> (U8 ("选择图层图片（PNG / JPG / SVG）"),
                                                               juce::File::getSpecialLocation (
                                                                   juce::File::userPicturesDirectory),
                                                               "*.png;*.jpg;*.jpeg;*.svg");

            fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                          | juce::FileBrowserComponent::canSelectFiles,
                                      [this] (const juce::FileChooser& chooser)
            {
                const auto file = chooser.getResult();

                if (! file.existsAsFile() || selectedLayer.isEmpty())
                    return;

                juce::String error;
                const auto relative = document.importAsset (file, error);

                if (relative.isEmpty())
                {
                    setStatus (U8 ("导入失败：") + error, true);
                    return;
                }

                document.setLayerValue (selectedLayer, "image", relative);
                markDirty();
                refreshRows();
                setStatus (U8 ("已导入 ") + relative, false);
            });
        }

        /** 选中图层的矩形（清单没写时给一个与新建图层一致的默认值）。 */
        juce::Rectangle<int> selectedLayerRect() const
        {
            const auto info = document.layerFor (selectedLayer);
            return info.hasRect ? info.rect : juce::Rectangle<int> (0, 0, 120, 120);
        }

        void applyLayerRect (juce::Rectangle<int> rect)
        {
            if (selectedLayer.isEmpty())
                return;

            document.setLayerValue (selectedLayer, "rect",
                                    juce::var (juce::Array<juce::var> { rect.getX(), rect.getY(),
                                                                        rect.getWidth(), rect.getHeight() }));

            if (notifyChanged)
                notifyChanged();
        }

        Transform layerTransform() const
        {
            return document.layerFor (selectedLayer).transform;
        }

        void setLayerTransform (const juce::String& key, const juce::var& value)
        {
            if (selectedLayer.isEmpty())
                return;

            document.setLayerValue (selectedLayer, key, value);

            if (notifyChanged)
                notifyChanged();
        }

        void addLayerAssetRow()
        {
            auto row = std::make_unique<AssetRow> (U8 ("图层图片"),
                                                   U8 ("（没有图片；导入或画一个矢量图）"));
            auto* raw = row.get();

            raw->onImport = [this] { importLayerImage(); };

            raw->onVector = [this]
            {
                if (selectedLayer.isEmpty())
                {
                    setStatus (U8 ("先选中一个图层"), true);
                    return;
                }

                openVectorEditor (selectedLayer + "-image", [this] (const juce::String& relative)
                {
                    if (selectedLayer.isEmpty())
                        return;

                    document.setLayerValue (selectedLayer, "image", relative);
                });
            };

            raw->onClear = [this]
            {
                if (selectedLayer.isEmpty())
                    return;

                document.setLayerValue (selectedLayer, "image", juce::var());
                markDirty();
                refreshRows();
                setStatus (U8 ("已清除图层图片"), false);
            };

            addRow (U8 ("图片"), std::move (row), [this, raw]
            {
                raw->setPath (document.layerFor (selectedLayer).imagePath);
            });
        }

        /** 图层页：动作按钮 + 从"最上层"排到"最下层"的列表 + 选中图层的属性。 */
        void buildLayerRows()
        {
            const auto allLayers = document.layers();

            section ("图层");

            {
                auto actions = std::make_unique<LayerActionsRow>();
                auto* raw = actions.get();

                raw->onAdd    = [this] { addNewLayer(); };
                raw->onImport = [this] { importLayerImage(); };
                raw->onDelete = [this] { deleteSelectedLayer(); };
                raw->setCanDelete (! selectedLayer.isEmpty());

                addWideRow (std::move (actions), {}, 26);
            }

            if (allLayers.empty())
            {
                auto hint = std::make_unique<juce::Label>();
                hint->setText (U8 ("还没有图层。点「新建图层」建一个空层，或导入图片作为贴图。"),
                               juce::dontSendNotification);
                hint->setFont (juce::FontOptions (12.0f));
                hint->setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.45f));
                hint->setJustificationType (juce::Justification::centredLeft);
                addWideRow (std::move (hint), {}, 30);
                return;
            }

            section ("图层列表（上方 = 越靠前，圆点 = 显示 / 隐藏）");

            // 数组末尾最上层：列表要按"专业绘图软件"的习惯把最上层放最上面，所以倒序遍历
            for (int i = static_cast<int> (allLayers.size()) - 1; i >= 0; --i)
            {
                const auto& info = allLayers[static_cast<size_t> (i)];

                if (info.id.isEmpty())
                    continue;

                auto row = std::make_unique<LayerRow>();
                auto* raw = row.get();

                raw->setInfo (info.name.isNotEmpty() ? info.name : info.id,
                              info.visible, info.id == selectedLayer);

                const auto id = info.id;
                raw->onSelect        = [this, id] { selectLayerFromList (id); };
                raw->onToggleVisible = [this, id] { toggleLayerVisible (id); };
                raw->onMoveUp        = [this, id] { moveLayerInList (id, 1); };
                raw->onMoveDown      = [this, id] { moveLayerInList (id, -1); };

                addWideRow (std::move (row), {}, 26);
            }

            const auto info = document.layerFor (selectedLayer);

            if (info.id.isEmpty())
                return;

            section ("选中图层的属性");

            // 名称：回车后才重建列表（边打字边重建会把输入框析构掉）
            {
                auto editor = std::make_unique<juce::TextEditor>();
                auto* raw = editor.get();

                raw->setText (info.name, juce::dontSendNotification);
                raw->setMultiLine (false);
                raw->setReturnKeyStartsNewLine (false);

                raw->onTextChange = [this, raw]
                {
                    if (selectedLayer.isEmpty())
                        return;

                    document.setLayerValue (selectedLayer, "name", raw->getText());
                    markDirty();
                };

                juce::Component::SafePointer<Panel> safeThis (this);
                raw->onReturnKey = [safeThis]
                {
                    if (safeThis != nullptr)
                        safeThis->scheduleRebuild();

                    return true;
                };

                addRow (U8 ("名称"), std::move (editor), [this, raw]
                {
                    raw->setText (document.layerFor (selectedLayer).name,
                                  juce::dontSendNotification);
                });
            }

            addToggle (U8 ("可见"),
                       [this] { return document.layerFor (selectedLayer).visible; },
                       [this] (bool v)
                       {
                           if (selectedLayer.isEmpty())
                               return;

                           document.setLayerValue (selectedLayer, "visible", v);
                           markDirty();
                       });

            addSlider (U8 ("不透明度"), 0.0, 1.0, 0.01,
                       [this] { return static_cast<double> (document.layerFor (selectedLayer).opacity); },
                       [this] (double v)
                       { document.setLayerValue (selectedLayer, "opacity",
                                                 juce::jlimit (0.0, 1.0, v)); });

            addLayerAssetRow();

            // ---- 布局 ----
            section ("图层布局（画布坐标；也可直接拖编辑层里的虚线框）");

            addSlider ("X", -3200.0, 3200.0, 1.0,
                       [this] { return static_cast<double> (selectedLayerRect().getX()); },
                       [this] (double v)
                       { auto r = selectedLayerRect(); r.setX (static_cast<int> (v)); applyLayerRect (r); });

            addSlider ("Y", -3200.0, 3200.0, 1.0,
                       [this] { return static_cast<double> (selectedLayerRect().getY()); },
                       [this] (double v)
                       { auto r = selectedLayerRect(); r.setY (static_cast<int> (v)); applyLayerRect (r); });

            addSlider (U8 ("宽"), 1.0, 1600.0, 1.0,
                       [this] { return static_cast<double> (selectedLayerRect().getWidth()); },
                       [this] (double v)
                       { auto r = selectedLayerRect(); r.setWidth (juce::jmax (1, static_cast<int> (v))); applyLayerRect (r); });

            addSlider (U8 ("高"), 1.0, 1600.0, 1.0,
                       [this] { return static_cast<double> (selectedLayerRect().getHeight()); },
                       [this] (double v)
                       { auto r = selectedLayerRect(); r.setHeight (juce::jmax (1, static_cast<int> (v))); applyLayerRect (r); });

            // ---- 变换 ----
            section (kTransformSectionTitle);

            addSlider (U8 ("旋转角"), -180.0, 180.0, 1.0,
                       [this] { return static_cast<double> (layerTransform().rotation); },
                       [this] (double v) { setLayerTransform ("rotation", v); });

            addSlider (U8 ("锚点 X"), 0.0, 1.0, 0.05,
                       [this] { return static_cast<double> (layerTransform().pivot.x); },
                       [this] (double v) { setLayerTransform ("pivot", pairVar (v, layerTransform().pivot.y)); });

            addSlider (U8 ("锚点 Y"), 0.0, 1.0, 0.05,
                       [this] { return static_cast<double> (layerTransform().pivot.y); },
                       [this] (double v) { setLayerTransform ("pivot", pairVar (layerTransform().pivot.x, v)); });

            addSlider (U8 ("平移 X"), -400.0, 400.0, 1.0,
                       [this] { return static_cast<double> (layerTransform().offset.x); },
                       [this] (double v) { setLayerTransform ("offset", pairVar (v, layerTransform().offset.y)); });

            addSlider (U8 ("平移 Y"), -400.0, 400.0, 1.0,
                       [this] { return static_cast<double> (layerTransform().offset.y); },
                       [this] (double v) { setLayerTransform ("offset", pairVar (layerTransform().offset.x, v)); });

            addSlider (U8 ("缩放 X"), 0.05, 4.0, 0.05,
                       [this] { return static_cast<double> (layerTransform().scale.x); },
                       [this] (double v) { setLayerTransform ("scale", pairVar (v, layerTransform().scale.y)); });

            addSlider (U8 ("缩放 Y"), 0.05, 4.0, 0.05,
                       [this] { return static_cast<double> (layerTransform().scale.y); },
                       [this] (double v) { setLayerTransform ("scale", pairVar (layerTransform().scale.x, v)); });

            addCombo (U8 ("缩放模式"), fitChoices(),
                      [this] { const auto f = layerTransform().fit;
                               return f.isEmpty() ? fitDefaultValue() : f; },
                      [this] (const juce::String& v)
                      { setLayerTransform ("fit", v == fitDefaultValue() ? juce::var() : juce::var (v)); });
        }

        // ---- 控件图层（elements[].layers[]，叠加在控件之上）----

        SkinDocument::ControlLayerInfo selectedControlLayerInfo() const
        {
            if (selected.isEmpty() || selectedControlLayer.isEmpty())
                return {};

            return document.controlLayerFor (selected, selectedControlLayer);
        }

        /** 选中控件图层的像素矩形（相对控件左上角：比归一化数字直观）。 */
        juce::Rectangle<int> selectedControlLayerRectPx() const
        {
            const auto info  = selectedControlLayerInfo();
            const auto owner = currentRect();

            return { juce::roundToInt (info.rect.getX() * owner.getWidth()),
                     juce::roundToInt (info.rect.getY() * owner.getHeight()),
                     juce::roundToInt (info.rect.getWidth() * owner.getWidth()),
                     juce::roundToInt (info.rect.getHeight() * owner.getHeight()) };
        }

        void applyControlLayerRectPx (juce::Rectangle<int> px)
        {
            if (selectedControlLayer.isEmpty())
                return;

            // 归一化存储：控件挪动 / 缩放时图层比例不变，所以这里必须按当前控件尺寸折算
            const auto owner = currentRect();
            const auto w = static_cast<float> (juce::jmax (1, owner.getWidth()));
            const auto h = static_cast<float> (juce::jmax (1, owner.getHeight()));

            document.setControlLayerRect (selected, selectedControlLayer,
                                          { static_cast<float> (px.getX()) / w,
                                            static_cast<float> (px.getY()) / h,
                                            static_cast<float> (px.getWidth()) / w,
                                            static_cast<float> (px.getHeight()) / h });

            if (notifyChanged)
                notifyChanged();
        }

        Transform controlLayerTransform() const
        {
            return selectedControlLayerInfo().transform;
        }

        void setControlLayerTransform (const juce::String& key, const juce::var& value)
        {
            if (selectedControlLayer.isEmpty())
                return;

            document.setControlLayerValue (selected, selectedControlLayer, key, value);

            if (notifyChanged)
                notifyChanged();
        }

        void selectControlLayerFromList (const juce::String& layerId)
        {
            if (layerId.isEmpty() || layerId == selectedControlLayer)
                return;

            selectedControlLayer = layerId;

            // 让编辑层同步选中：紫色虚线框与拖拽目标都跟着走
            if (notifySelected)
                notifySelected (selected + "/" + layerId);

            scheduleRebuild();
        }

        void toggleControlLayerVisible (const juce::String& layerId)
        {
            const auto info = document.controlLayerFor (selected, layerId);

            if (info.id.isEmpty())
                return;

            document.setControlLayerValue (selected, layerId, "visible", ! info.visible);
            markDirty();
            scheduleRebuild();

            setStatus (info.visible ? U8 ("已隐藏控件图层「") + info.name + U8 ("」")
                                    : U8 ("已显示控件图层「") + info.name + U8 ("」"), false);
        }

        void moveControlLayerInList (const juce::String& layerId, int delta)
        {
            if (! document.moveControlLayer (selected, layerId, delta))
                return;

            markDirty();
            scheduleRebuild();
        }

        void addNewControlLayer()
        {
            const auto id = document.addControlLayer (selected);

            if (id.isEmpty())
            {
                setStatus (U8 ("新建控件图层失败：清单里没有可写的元素"), true);
                return;
            }

            selectedControlLayer = id;
            markDirty();

            if (notifySelected)
                notifySelected (selected + "/" + id);

            scheduleRebuild();
            setStatus (U8 ("已新建控件图层（默认铺满控件；导入图片或画个矢量图即可）"), false);
        }

        void deleteSelectedControlLayer()
        {
            if (selectedControlLayer.isEmpty())
            {
                setStatus (U8 ("先选中一个控件图层"), true);
                return;
            }

            const auto info = selectedControlLayerInfo();

            if (info.id.isEmpty())
                return;

            const auto ownerId = selected;
            const auto layerId = selectedControlLayer;
            const auto name    = info.name.isNotEmpty() ? info.name : info.id;

            const auto options = juce::MessageBoxOptions()
                                     .withIconType (juce::MessageBoxIconType::WarningIcon)
                                     .withTitle (U8 ("删除控件图层"))
                                     .withMessage (U8 ("确定删除控件图层「") + name + U8 ("」吗？"))
                                     .withButton (U8 ("删除"))
                                     .withButton (U8 ("取消"));

            juce::Component::SafePointer<Panel> safeThis (this);

            juce::AlertWindow::showAsync (options, [safeThis, ownerId, layerId, name] (int result)
            {
                if (result != 1 || safeThis == nullptr)
                    return;

                safeThis->document.removeControlLayer (ownerId, layerId);

                // 删除期间用户可能已经切到别的控件：只在自己仍停留在这个控件时改选中项
                if (safeThis->selected == ownerId)
                {
                    const auto remaining = safeThis->document.controlLayers (ownerId);
                    safeThis->selectedControlLayer = remaining.empty() ? juce::String()
                                                                       : remaining.back().id;

                    if (safeThis->notifySelected)
                        safeThis->notifySelected (safeThis->selectedControlLayer.isEmpty()
                                                      ? ownerId
                                                      : ownerId + "/" + safeThis->selectedControlLayer);
                }

                safeThis->markDirty();
                safeThis->scheduleRebuild();
                safeThis->setStatus (U8 ("已删除控件图层「") + name + U8 ("」"), false);
            });
        }

        /** 控件图层的便捷入口：没有可用选中图层时自动新建一个。
            返回可写入的图层 id（失败为空），并同步编辑层选中与面板列表。 */
        juce::String ensureControlLayerForEdit()
        {
            if (selected.isEmpty())
                return {};

            if (document.controlLayerFor (selected, selectedControlLayer).id.isNotEmpty())
                return selectedControlLayer;

            const auto id = document.addControlLayer (selected);

            if (id.isEmpty())
                return {};

            selectedControlLayer = id;
            markDirty();

            if (notifySelected)
                notifySelected (selected + "/" + id);

            scheduleRebuild();
            return id;
        }

        void importControlLayerImage()
        {
            if (selected.isEmpty())
            {
                setStatus (U8 ("先选中一个控件，再导入图片"), true);
                return;
            }

            const auto ownerId = selected;

            fileChooser = std::make_unique<juce::FileChooser> (U8 ("选择图层图片（PNG / JPG / SVG）"),
                                                               juce::File::getSpecialLocation (
                                                                   juce::File::userPicturesDirectory),
                                                               "*.png;*.jpg;*.jpeg;*.svg");

            fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                          | juce::FileBrowserComponent::canSelectFiles,
                                      [this, ownerId] (const juce::FileChooser& chooser)
            {
                const auto file = chooser.getResult();

                if (! file.existsAsFile())
                    return;

                // 选文件期间用户可能切走：不要把图片写进已经看不见的控件
                if (selected != ownerId)
                {
                    setStatus (U8 ("导入已取消：控件已切换"), true);
                    return;
                }

                const auto layerId = ensureControlLayerForEdit();

                if (layerId.isEmpty())
                {
                    setStatus (U8 ("导入失败：清单里没有可写的元素"), true);
                    return;
                }

                juce::String error;
                const auto relative = document.importAsset (file, error);

                if (relative.isEmpty())
                {
                    setStatus (U8 ("导入失败：") + error, true);
                    return;
                }

                document.setControlLayerValue (ownerId, layerId, "image", relative);
                markDirty();
                refreshRows();
                setStatus (U8 ("已导入 ") + relative, false);
            });
        }

        /** 动作行的「矢量图形…」：没有图层就顺手新建一个，画完直接成为它的贴图。 */
        void drawControlLayerVector()
        {
            if (selected.isEmpty())
            {
                setStatus (U8 ("先选中一个控件，再画矢量图形"), true);
                return;
            }

            const auto ownerId    = selected;
            const auto existingId = document.controlLayerFor (ownerId, selectedControlLayer).id;
            const auto preferred  = existingId.isNotEmpty() ? ownerId + "-" + existingId
                                                            : ownerId + "-vector";

            // 取消绘图不应该留下一个空图层：确认生成后再建层并写回图片。
            // openVectorEditor 成功后内部会 markDirty + refreshRows，这里只负责建层与写回。
            openVectorEditor (preferred, [this, ownerId] (const juce::String& relative)
            {
                if (selected != ownerId)
                {
                    setStatus (U8 ("矢量图形已生成，但控件已切换，未写入图层"), true);
                    return;
                }

                const auto layerId = ensureControlLayerForEdit();

                if (layerId.isEmpty())
                {
                    setStatus (U8 ("写入失败：清单里没有可写的元素"), true);
                    return;
                }

                document.setControlLayerValue (ownerId, layerId, "image", relative);
            });
        }

        void addControlLayerAssetRow()
        {
            auto row = std::make_unique<AssetRow> (U8 ("图层图片"),
                                                   U8 ("（没有图片；导入或画一个矢量图）"));
            auto* raw = row.get();

            raw->onImport = [this] { importControlLayerImage(); };

            raw->onVector = [this] { drawControlLayerVector(); };

            raw->onClear = [this]
            {
                if (selectedControlLayer.isEmpty())
                    return;

                document.setControlLayerValue (selected, selectedControlLayer, "image", juce::var());
                markDirty();
                refreshRows();
                setStatus (U8 ("已清除控件图层图片"), false);
            };

            addRow (U8 ("图片"), std::move (row), [this, raw]
            {
                raw->setPath (selectedControlLayerInfo().imagePath);
            });
        }

        // ---- 窗口与异形（阶段三 3e）----

        /** 出血某一侧的值（0=左 1=上 2=右 3=下）。 */
        double expandValue (int side) const
        {
            const auto e = document.windowExpand();

            switch (side)
            {
                case 0:  return e.getX();
                case 1:  return e.getY();
                case 2:  return e.getWidth();
                default: return e.getHeight();
            }
        }

        static juce::StringArray shapeModeChoices()
        {
            return { U8 ("关（矩形窗口）"),
                     U8 ("自动（独立运行真异形；插件里只做视觉）"),
                     U8 ("强制（插件里也裁窗口，实验性）") };
        }

        static juce::String shapeModeLabelFor (const juce::String& mode)
        {
            if (mode == "off")   return shapeModeChoices()[0];
            if (mode == "force") return shapeModeChoices()[2];

            return shapeModeChoices()[1];
        }

        static juce::String shapeModeFromLabel (const juce::String& label)
        {
            if (label == shapeModeChoices()[0]) return "off";
            if (label == shapeModeChoices()[2]) return "force";

            return "auto";
        }

        /** 蒙版尺寸 vs 窗口尺寸：不一致会被拉伸，直接写出来才不会白折腾。 */
        juce::String maskSizeHint() const
        {
            const auto& s = SkinBundle::getInstance().getCurrent();

            if (s.shapeMaskPath.isEmpty())
                return U8 ("没有蒙版 → 矩形窗口（形状外的绘制照常，窗口边界就是画布 + 出血）");

            const auto image = s.shapeMask();

            if (image.isNull())
                return U8 ("蒙版读不出来：") + s.shapeMaskPath;

            const auto text = U8 ("蒙版 ") + juce::String (image.getWidth()) + U8 ("×")
                            + juce::String (image.getHeight())
                            + U8 ("  →  窗口 ") + juce::String (s.windowWidth()) + U8 ("×")
                            + juce::String (s.windowHeight());

            if (image.getWidth() != s.windowWidth() || image.getHeight() != s.windowHeight())
                return text + U8 ("（尺寸不一致，会被拉伸；建议按窗口尺寸出图）");

            return text + U8 ("（尺寸一致）");
        }

        void importShapeMaskImage()
        {
            fileChooser = std::make_unique<juce::FileChooser> (U8 ("选择形状蒙版（PNG / JPG / SVG）"),
                                                               juce::File::getSpecialLocation (
                                                                   juce::File::userPicturesDirectory),
                                                               "*.png;*.jpg;*.jpeg;*.svg");

            fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                          | juce::FileBrowserComponent::canSelectFiles,
                                      [this] (const juce::FileChooser& chooser)
            {
                const auto file = chooser.getResult();

                if (! file.existsAsFile())
                    return;

                juce::String error;
                const auto relative = document.importAsset (file, error);

                if (relative.isEmpty())
                {
                    setStatus (U8 ("导入失败：") + error, true);
                    return;
                }

                document.setShapeMaskPath (relative);
                markDirty();
                refreshRows();
                setStatus (U8 ("形状蒙版：") + relative + U8 ("（alpha > 128 处属于窗口）"), false);
            });
        }

        /** 蒙版就是"皮肤里的一张图"，所以直接复用内置矢量绘图；画完写进 shape.mask。 */
        void drawShapeMaskVector()
        {
            openVectorEditor ("shape", [this] (const juce::String& relative)
            {
                document.setShapeMaskPath (relative);
                setStatus (U8 ("形状蒙版：") + relative + U8 ("（alpha > 128 处属于窗口）"), false);
            });
        }

        /** 「窗口与异形」分区：出血 / 形状模式 / 蒙版 / 无标题栏 / 拖动区。 */
        void buildWindowRows()
        {
            section ("窗口与异形（出血 = 窗口比画布大出来的四边；清单坐标仍相对画布）");

            const char* const sideNames[] = { "出血 左", "出血 上", "出血 右", "出血 下" };

            for (int side = 0; side < 4; ++side)
                addSlider (sideNames[side], 0.0, 400.0, 1.0,
                           [this, side] { return expandValue (side); },
                           [this, side] (double v)
                           {
                               document.setWindowExpand (side, static_cast<int> (v));
                               markDirty();
                           });

            addCombo ("形状模式", shapeModeChoices(),
                      [this] { return shapeModeLabelFor (document.shapeMode()); },
                      [this] (const juce::String& label)
                      {
                          document.setShapeMode (shapeModeFromLabel (label));
                          markDirty();
                          refreshRows();
                      });

            {
                auto row = std::make_unique<AssetRow> (U8 ("形状蒙版"), U8 ("（没有蒙版 = 矩形窗口）"));
                auto* raw = row.get();

                raw->onImport = [this] { importShapeMaskImage(); };
                raw->onVector = [this] { drawShapeMaskVector(); };
                raw->onClear  = [this]
                {
                    document.setShapeMaskPath ({});
                    markDirty();
                    refreshRows();
                    setStatus (U8 ("已清除形状蒙版（回到矩形窗口）"), false);
                };

                addRow (U8 ("蒙版"), std::move (row), [this, raw]
                {
                    raw->setPath (document.shapeMaskPath());
                });
            }

            {
                auto hint = std::make_unique<juce::Label>();
                hint->setFont (juce::FontOptions (11.0f));
                hint->setJustificationType (juce::Justification::centredLeft);
                hint->setColour (juce::Label::textColourId, juce::Colours::lightgrey);
                hint->setText (maskSizeHint(), juce::dontSendNotification);

                auto* raw = hint.get();

                addWideRow (std::move (hint), [this, raw]
                {
                    raw->setText (maskSizeHint(), juce::dontSendNotification);
                }, 24);
            }

            addToggle ("无标题栏（仅独立运行）",
                       [this] { return document.frameless(); },
                       [this] (bool v)
                       {
                           document.setFrameless (v);
                           markDirty();
                           setStatus (v ? U8 ("无标题栏：拖动窗口靠按住空白处，"
                                              "音频设置用右键菜单或 Ctrl+Shift+A")
                                        : U8 ("已恢复标题栏"), false);
                       });

            const juce::StringArray dragChoices { U8 ("背景任意处（自动）"), U8 ("自定义矩形") };

            addCombo ("拖动区（无标题栏时）", dragChoices,
                      [this, dragChoices] { return document.dragAreaKind() == "rect" ? dragChoices[1] : dragChoices[0]; },
                      [this, dragChoices] (const juce::String& label)
                      {
                          if (label == dragChoices[1])
                          {
                              // 没写过矩形时给个"顶部一条"的起点，比 0 尺寸的空框好用
                              auto rect = document.dragAreaRect();

                              if (rect.isEmpty())
                                  rect = juce::Rectangle<int> (0, 0, 160, 24).translated (
                                             0, -document.windowExpand().getY());

                              document.setDragAreaRect (rect);
                          }
                          else
                          {
                              document.setDragAreaAuto();
                          }

                          markDirty();
                          refreshRows();
                      });

            if (document.dragAreaKind() == "rect")
            {
                // 拖动区是设计坐标（相对画布），和元素的 rect 一套口径
                const char* const names[] = { "拖动区 X", "拖动区 Y", "拖动区 宽", "拖动区 高" };

                for (int part = 0; part < 4; ++part)
                    addSlider (names[part], part < 2 ? -400.0 : 0.0, part < 2 ? 1600.0 : 400.0, 1.0,
                               [this, part]
                               {
                                   const auto rect = document.dragAreaRect();

                                   switch (part)
                                   {
                                       case 0:  return static_cast<double> (rect.getX());
                                       case 1:  return static_cast<double> (rect.getY());
                                       case 2:  return static_cast<double> (rect.getWidth());
                                       default: return static_cast<double> (rect.getHeight());
                                   }
                               },
                               [this, part] (double v)
                               {
                                   auto rect = document.dragAreaRect();

                                   switch (part)
                                   {
                                       case 0:  rect.setX (static_cast<int> (v)); break;
                                       case 1:  rect.setY (static_cast<int> (v)); break;
                                       case 2:  rect.setWidth (static_cast<int> (v)); break;
                                       default: rect.setHeight (static_cast<int> (v)); break;
                                   }

                                   document.setDragAreaRect (rect);
                                   markDirty();
                               });
            }
        }

        /** 控件页的「控件图层」分区：图层栈 + 选中图层的属性（图片 / 布局 / 变换）。 */
        void buildControlLayerRows()
        {
            const auto allLayers = document.controlLayers (selected);

            section ("控件图层（叠加在此控件之上，随控件移动 / 缩放 / 旋转）");

            {
                auto actions = std::make_unique<LayerActionsRow> (true);
                auto* raw = actions.get();

                raw->onAdd    = [this] { addNewControlLayer(); };
                raw->onImport = [this] { importControlLayerImage(); };
                raw->onVector = [this] { drawControlLayerVector(); };
                raw->onDelete = [this] { deleteSelectedControlLayer(); };
                raw->setCanDelete (! selectedControlLayer.isEmpty());

                addWideRow (std::move (actions), {}, 26);
            }

            if (allLayers.empty())
            {
                auto hint = std::make_unique<juce::Label>();
                hint->setText (U8 ("这个控件还没有自己的图层。直接点「导入图片…」或「矢量图形…」会自动建好图层；"
                                   "图层叠在控件之上并跟着控件一起移动 / 缩放 / 旋转。"),
                               juce::dontSendNotification);
                hint->setFont (juce::FontOptions (12.0f));
                hint->setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.45f));
                hint->setJustificationType (juce::Justification::centredLeft);
                addWideRow (std::move (hint), {}, 46);
                return;
            }

            section ("控件图层列表（上方 = 越靠前，圆点 = 显示 / 隐藏）");

            // 数组末尾最上层：与画布图层一致，列表把最上层放最上面
            for (int i = static_cast<int> (allLayers.size()) - 1; i >= 0; --i)
            {
                const auto& layer = allLayers[static_cast<size_t> (i)];

                if (layer.id.isEmpty())
                    continue;

                auto row = std::make_unique<LayerRow>();
                auto* raw = row.get();

                raw->setInfo (layer.name.isNotEmpty() ? layer.name : layer.id,
                              layer.visible, layer.id == selectedControlLayer);

                const auto id = layer.id;
                raw->onSelect        = [this, id] { selectControlLayerFromList (id); };
                raw->onToggleVisible = [this, id] { toggleControlLayerVisible (id); };
                raw->onMoveUp        = [this, id] { moveControlLayerInList (id, 1); };
                raw->onMoveDown      = [this, id] { moveControlLayerInList (id, -1); };

                addWideRow (std::move (row), {}, 26);
            }

            const auto info = selectedControlLayerInfo();

            if (info.id.isEmpty())
            {
                auto hint = std::make_unique<juce::Label>();
                hint->setText (U8 ("点上面列表里的图层，或在编辑层里直接点它的紫色虚线框，就能编辑属性。"),
                               juce::dontSendNotification);
                hint->setFont (juce::FontOptions (12.0f));
                hint->setColour (juce::Label::textColourId, juce::Colours::white.withAlpha (0.45f));
                hint->setJustificationType (juce::Justification::centredLeft);
                addWideRow (std::move (hint), {}, 30);
                return;
            }

            section ("选中控件图层的属性");

            // 名称：回车后才重建列表（边打字边重建会把输入框析构掉）
            {
                auto editor = std::make_unique<juce::TextEditor>();
                auto* raw = editor.get();

                raw->setText (info.name, juce::dontSendNotification);
                raw->setMultiLine (false);
                raw->setReturnKeyStartsNewLine (false);

                raw->onTextChange = [this, raw]
                {
                    if (selectedControlLayer.isEmpty())
                        return;

                    document.setControlLayerValue (selected, selectedControlLayer, "name", raw->getText());
                    markDirty();
                };

                juce::Component::SafePointer<Panel> safeThis (this);
                raw->onReturnKey = [safeThis]
                {
                    if (safeThis != nullptr)
                        safeThis->scheduleRebuild();

                    return true;
                };

                addRow (U8 ("名称"), std::move (editor), [this, raw]
                {
                    raw->setText (selectedControlLayerInfo().name, juce::dontSendNotification);
                });
            }

            addToggle (U8 ("可见"),
                       [this] { return selectedControlLayerInfo().visible; },
                       [this] (bool v)
                       {
                           if (selectedControlLayer.isEmpty())
                               return;

                           document.setControlLayerValue (selected, selectedControlLayer, "visible", v);
                       });

            addSlider (U8 ("不透明度"), 0.0, 1.0, 0.01,
                       [this] { return static_cast<double> (selectedControlLayerInfo().opacity); },
                       [this] (double v)
                       {
                           if (selectedControlLayer.isEmpty())
                               return;

                           document.setControlLayerValue (selected, selectedControlLayer, "opacity",
                                                         juce::jlimit (0.0, 1.0, v));
                       });

            addControlLayerAssetRow();

            // ---- 布局（相对控件左上角）----
            section ("控件图层布局（相对控件左上角；也可直接拖编辑层里的紫色虚线框）");

            addSlider ("X", -1600.0, 1600.0, 1.0,
                       [this] { return static_cast<double> (selectedControlLayerRectPx().getX()); },
                       [this] (double v)
                       { auto r = selectedControlLayerRectPx(); r.setX (static_cast<int> (v)); applyControlLayerRectPx (r); });

            addSlider ("Y", -1600.0, 1600.0, 1.0,
                       [this] { return static_cast<double> (selectedControlLayerRectPx().getY()); },
                       [this] (double v)
                       { auto r = selectedControlLayerRectPx(); r.setY (static_cast<int> (v)); applyControlLayerRectPx (r); });

            addSlider (U8 ("宽"), 1.0, 1600.0, 1.0,
                       [this] { return static_cast<double> (selectedControlLayerRectPx().getWidth()); },
                       [this] (double v)
                       { auto r = selectedControlLayerRectPx(); r.setWidth (juce::jmax (1, static_cast<int> (v))); applyControlLayerRectPx (r); });

            addSlider (U8 ("高"), 1.0, 1600.0, 1.0,
                       [this] { return static_cast<double> (selectedControlLayerRectPx().getHeight()); },
                       [this] (double v)
                       { auto r = selectedControlLayerRectPx(); r.setHeight (juce::jmax (1, static_cast<int> (v))); applyControlLayerRectPx (r); });

            // ---- 变换 ----
            section (kTransformSectionTitle);

            addSlider (U8 ("旋转角"), -180.0, 180.0, 1.0,
                       [this] { return static_cast<double> (controlLayerTransform().rotation); },
                       [this] (double v) { setControlLayerTransform ("rotation", v); });

            addSlider (U8 ("锚点 X"), 0.0, 1.0, 0.05,
                       [this] { return static_cast<double> (controlLayerTransform().pivot.x); },
                       [this] (double v) { setControlLayerTransform ("pivot", pairVar (v, controlLayerTransform().pivot.y)); });

            addSlider (U8 ("锚点 Y"), 0.0, 1.0, 0.05,
                       [this] { return static_cast<double> (controlLayerTransform().pivot.y); },
                       [this] (double v) { setControlLayerTransform ("pivot", pairVar (controlLayerTransform().pivot.x, v)); });

            addSlider (U8 ("平移 X"), -400.0, 400.0, 1.0,
                       [this] { return static_cast<double> (controlLayerTransform().offset.x); },
                       [this] (double v) { setControlLayerTransform ("offset", pairVar (v, controlLayerTransform().offset.y)); });

            addSlider (U8 ("平移 Y"), -400.0, 400.0, 1.0,
                       [this] { return static_cast<double> (controlLayerTransform().offset.y); },
                       [this] (double v) { setControlLayerTransform ("offset", pairVar (controlLayerTransform().offset.x, v)); });

            addSlider (U8 ("缩放 X"), 0.05, 4.0, 0.05,
                       [this] { return static_cast<double> (controlLayerTransform().scale.x); },
                       [this] (double v) { setControlLayerTransform ("scale", pairVar (v, controlLayerTransform().scale.y)); });

            addSlider (U8 ("缩放 Y"), 0.05, 4.0, 0.05,
                       [this] { return static_cast<double> (controlLayerTransform().scale.y); },
                       [this] (double v) { setControlLayerTransform ("scale", pairVar (controlLayerTransform().scale.x, v)); });

            addCombo (U8 ("缩放模式"), fitChoices(),
                      [this] { const auto f = controlLayerTransform().fit;
                               return f.isEmpty() ? fitDefaultValue() : f; },
                      [this] (const juce::String& v)
                      { setControlLayerTransform ("fit", v == fitDefaultValue() ? juce::var() : juce::var (v)); });
        }

        // ---- 矢量绘图 ----

        void openVectorEditor (const juce::String& preferredName,
                               std::function<void (const juce::String&)> applyPath)
        {
            juce::Component::SafePointer<Panel> safeThis (this);

            auto editor = std::make_unique<VectorShapeEditor> (
                [safeThis, preferredName, applyPath] (const juce::String& svgText)
                {
                    if (safeThis == nullptr || applyPath == nullptr)
                        return;

                    juce::String error;
                    const auto relative = safeThis->document.importVectorAsset (svgText, preferredName, error);

                    if (relative.isEmpty())
                    {
                        safeThis->setStatus (U8 ("创建矢量图形失败：") + error, true);
                        return;
                    }

                    applyPath (relative);
                    safeThis->markDirty();
                    safeThis->refreshRows();
                    safeThis->setStatus (U8 ("已创建矢量图形：") + relative, false);
                });

            juce::DialogWindow::LaunchOptions options;
            options.content.setOwned (editor.release());
            options.dialogTitle = U8 ("内置矢量绘图");
            options.dialogBackgroundColour = juce::Colour (0xff232326);
            options.escapeKeyTriggersCloseButton = true;
            options.useNativeTitleBar = true;
            options.resizable = false;
            options.launchAsync();
        }

        void importAsset (const juce::String& slot)
        {
            fileChooser = std::make_unique<juce::FileChooser> (U8 ("选择贴图（PNG / JPG）"),
                                                               juce::File::getSpecialLocation (
                                                                   juce::File::userPicturesDirectory),
                                                               "*.png;*.jpg;*.jpeg");

            fileChooser->launchAsync (juce::FileBrowserComponent::openMode
                                          | juce::FileBrowserComponent::canSelectFiles,
                                      [this, slot] (const juce::FileChooser& chooser)
            {
                const auto file = chooser.getResult();

                if (! file.existsAsFile())
                    return;

                juce::String error;
                const auto relative = document.importAsset (file, error);

                if (relative.isEmpty())
                {
                    setStatus (U8 ("导入失败：") + error, true);
                    return;
                }

                document.setAsset (selected, slot, relative);
                markDirty();
                refreshRows();
                setStatus (U8 ("已导入 ") + relative, false);
            });
        }

        // ---- 写盘（节流：拖动数值时最多约 16 次/秒）----

        void markDirty()
        {
            if (! document.isWritable())
            {
                setStatus (U8 ("当前皮肤不是可编辑目录，改动只在内存里；请先加载一个外部皮肤"), true);
                return;
            }

            if (! isTimerRunning())
                startTimer (60);
        }

        void timerCallback() override
        {
            stopTimer();
            flush();
        }

        /** 立即写回并刷新预览。 */
        void flush()
        {
            if (! document.isWritable())
                return;

            juce::String error;

            if (document.writeThrough (error))
                setStatus (U8 ("已保存并刷新预览"), false);
            else
                setStatus (U8 ("保存失败：") + error, true);

            if (notifyChanged)
                notifyChanged();
        }

        // ---- 方案预设 ----

        /** 回到内置默认布局前，先把当前设计存成这个名字的预设。
            固定一个名字 = 只滚动一个槽位，不会攒出一堆备份。 */
        static juce::String backupPresetName() { return U8 ("恢复前备份"); }

        /** 选中的预设能不能删：内置那条删不掉，自己存的那条能删。 */
        bool canDeleteSelectedPreset() const
        {
            const auto name = presetBox.getText();

            return name.isNotEmpty()
                && ! SkinDocument::isBuiltInPreset (name)
                && document.listPresets().contains (name);
        }

        void refreshPresetList()
        {
            const auto previous = presetBox.getText();

            presetBox.clear (juce::dontSendNotification);

            // 内置那条永远排在最前面：它不来自预设目录，所以删不掉 ——
            // 用户就算误删了自己的预设，也删不掉这条退路
            presetBox.addItem (SkinDocument::builtInPresetName(), 1);

            const auto names = document.listPresets();

            for (int i = 0; i < names.size(); ++i)
                presetBox.addItem (names[i], i + 2);

            // 尽量保持原来的选择；原来那条没了（被删掉）就退回内置那条
            int restoredId = 0;

            for (int i = 0; i < presetBox.getNumItems(); ++i)
                if (presetBox.getItemText (i) == previous)
                {
                    restoredId = presetBox.getItemId (i);
                    break;
                }

            presetBox.setSelectedId (restoredId > 0 ? restoredId : 1, juce::dontSendNotification);

            presetDeleteButton.setEnabled (canDeleteSelectedPreset());
        }

        void saveCurrentPreset()
        {
            const auto name = presetNameEditor.getText().trim();

            if (name.isEmpty())
            {
                setStatus (U8 ("先输入预设名，再点「存为预设」"), true);
                presetNameEditor.grabKeyboardFocus();
                return;
            }

            juce::String error;

            if (! document.savePreset (name, error))
            {
                setStatus (U8 ("预设保存失败：") + error, true);
                return;
            }

            refreshPresetList();
            presetBox.setText (sanitisePresetName (name), juce::dontSendNotification);
            presetNameEditor.clear();
            setStatus (U8 ("已存为预设「") + sanitisePresetName (name) + U8 ("」"), false);
        }

        void loadSelectedPreset()
        {
            const auto name = presetBox.getText();

            if (name.isEmpty())
            {
                setStatus (U8 ("先选一个预设"), true);
                return;
            }

            if (SkinDocument::isBuiltInPreset (name))
            {
                restoreDefaultLayout();
                return;
            }

            juce::String error;

            if (! document.loadPreset (name, error))
            {
                setStatus (U8 ("加载预设失败：") + error, true);
                return;
            }

            // 清单与资源都换了：面板与编辑层都要按新文档重建
            rebuildRows();

            if (notifyChanged)
                notifyChanged();

            setStatus (U8 ("已加载预设「") + name + U8 ("」"), false);
        }

        /** 回到内置默认布局：先把当前设计落成「恢复前备份」预设，再把内置那份整体写回去。 */
        void restoreDefaultLayout()
        {
            juce::String backupError;

            if (! document.savePreset (backupPresetName(), backupError))
            {
                // 备份失败就干脆不动皮肤：宁可暂时回不了默认，也不能让改到一半的设计没了退路
                setStatus (U8 ("恢复前备份失败，已中止：") + backupError, true);
                return;
            }

            juce::String error;

            if (! document.restoreBuiltIn (error))
            {
                setStatus (U8 ("恢复默认布局失败：") + error, true);
                return;
            }

            rebuildRows();

            if (notifyChanged)
                notifyChanged();

            // 备份预设是新出现的：列表要重刷，并把选择停在内置那条上
            refreshPresetList();
            presetBox.setText (SkinDocument::builtInPresetName(), juce::dontSendNotification);

            setStatus (U8 ("已恢复内置默认布局；恢复前的设计已存为预设「") + backupPresetName() + U8 ("」"),
                       false);
        }

        void deleteSelectedPreset()
        {
            const auto name = presetBox.getText();

            if (name.isEmpty())
            {
                setStatus (U8 ("先选一个预设"), true);
                return;
            }

            if (SkinDocument::isBuiltInPreset (name))
            {
                setStatus (U8 ("「") + name + U8 ("」是内置预设，不能删除"), true);
                return;
            }

            const auto options = juce::MessageBoxOptions()
                                     .withIconType (juce::MessageBoxIconType::WarningIcon)
                                     .withTitle (U8 ("删除预设"))
                                     .withMessage (U8 ("确定删除预设「") + name
                                                   + U8 ("」吗？\n只删除预设，不影响当前皮肤。"))
                                     .withButton (U8 ("删除"))
                                     .withButton (U8 ("取消"));

            juce::Component::SafePointer<Panel> safeThis (this);

            juce::AlertWindow::showAsync (options, [safeThis, name] (int result) mutable
            {
                if (result != 1 || safeThis == nullptr)
                    return;

                juce::String error;

                if (! safeThis->document.deletePreset (name, error))
                {
                    safeThis->setStatus (U8 ("删除预设失败：") + error, true);
                    return;
                }

                safeThis->refreshPresetList();
                safeThis->setStatus (U8 ("已删除预设「") + name + U8 ("」"), false);
            });
        }

        void setStatus (const juce::String& text, bool isError)
        {
            statusLabel.setText (text, juce::dontSendNotification);
            statusLabel.setColour (juce::Label::textColourId,
                                   isError ? juce::Colours::orange : juce::Colour (0xff0fdc78));
        }

        /** 纯中文字面量的调用处（见 UiText.h 的说明）。 */
        void setStatus (const char* text, bool isError) { setStatus (U8 (text), isError); }

        SkinDocument& document;
        std::function<juce::Rectangle<int> (const juce::String&)> editorBounds;
        std::function<void()> notifyChanged;
        std::function<void (const juce::String&)> notifySelected;
        std::function<void (const juce::String&)> notifyTravelPart;
        std::function<juce::String()> shaderCompileError;
        std::function<juce::String()> shaderRuntimeStatus;

        ShaderSourceRow* shaderRow = nullptr;   // 非拥有：生命周期由 rows 管

        juce::ComboBox   elementBox;
        juce::TextButton prevButton { "<" }, nextButton { ">" }, saveButton { "Save" },
                         folderButton { "Open" };

        // 控件 / 图层 两种面板模式
        juce::TextButton elementModeButton, layerModeButton;
        bool             layerMode = false;
        /** 图层模式里当前选中的图层 id（空 = 没选）。 */
        juce::String     selectedLayer;

        /** 控件页里当前选中的控件图层 id（空 = 没选；所属控件见 selected）。 */
        juce::String     selectedControlLayer;
        /** 已经排了一次延后重建（防止同一拍里重复排队）。 */
        bool             rebuildPending = false;

        // 方案预设：选已有预设 → 加载 / 删除；输入名字 → 存为预设
        juce::ComboBox   presetBox;
        juce::TextEditor presetNameEditor;
        juce::TextButton presetSaveButton, presetLoadButton, presetDeleteButton;

        juce::Label      statusLabel;
        juce::Viewport   viewport;
        juce::Component  content;

        std::vector<Row> rows;
        juce::String     selected { "mix" };

        /** 运动轨迹分区正在编辑的部位（空 = 元素级）。 */
        juce::String     travelPart;
        /** travelPart 属于哪个元素：换元素时自动回到「整体」。 */
        juce::String     travelPartOwner;

        /** 被收起的分区标题。着色器分区默认收起（否则每次都要滚过元素自己的参数才看得到）。 */
        juce::StringArray collapsedSections;

        /** 正在构建的分区行下标（-1 = 还没有分区）。 */
        int currentSection = -1;

        std::unique_ptr<juce::FileChooser> fileChooser;
    };

    DesignerWindow::DesignerWindow (SkinDocument& document, Callbacks callbacks)
        : juce::DocumentWindow (U8 ("OnlyObsessionRemained Skin Designer 皮肤设计器"), juce::Colour (0xff232326),
                                juce::DocumentWindow::closeButton)
    {
        panel = std::make_unique<Panel> (document, std::move (callbacks));

        setUsingNativeTitleBar (true);
        setContentNonOwned (panel.get(), false);   // 自己定尺寸，别被内容高度牵着走
        setResizable (true, false);
        setResizeLimits (480, 420, 760, 1400);
        centreWithSize (520, 800);
        setAlwaysOnTop (true);
        setVisible (true);
    }

    DesignerWindow::~DesignerWindow()
    {
        setContentNonOwned (nullptr, false);
    }

    void DesignerWindow::closeButtonPressed()
    {
        setVisible (false);   // 由编辑器负责真正的关闭（它持有本窗口）
    }

    void DesignerWindow::selectElement (const juce::String& elementId)
    {
        if (panel != nullptr)
            panel->setSelected (elementId);
    }

    void DesignerWindow::selectLayer (const juce::String& layerId)
    {
        if (panel != nullptr)
            panel->setSelectedLayer (layerId);
    }

    void DesignerWindow::selectControlLayer (const juce::String& ownerId, const juce::String& layerId)
    {
        if (panel != nullptr)
            panel->setSelectedControlLayer (ownerId, layerId, true);
    }

    void DesignerWindow::refreshAll()
    {
        if (panel != nullptr)
            panel->refreshRows();
    }

    void DesignerWindow::onSkinChanged()
    {
        if (panel != nullptr)
            panel->reloadShaderSource();
    }
}