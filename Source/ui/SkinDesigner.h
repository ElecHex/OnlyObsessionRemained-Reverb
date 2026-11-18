#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <functional>
#include <memory>
#include <utility>
#include <vector>

#include "ui/SkinBundle.h"

/**
    皮肤设计器（开发期工具；只在 Standalone 里可用，见 OnlyObsessionRemainedEditor 的入口）。

    目标：像绘图软件那样改控件——**直接预览真实插件界面**、拖拽改位置尺寸、
    实时导入贴图、改控件形态与操作逻辑（旋钮↔推子、行程、拖动方式、数值框方位…）。

    三个部分：

    1. SkinDocument：以清单文本为底稿的文档模型。所有修改都先落在模型上，
       然后 writeThrough() 把清单写回磁盘、让 SkinBundle 重新加载——这样预览
       走的是**和正式皮肤完全相同的加载路径**，所见即所得。
       每次保存都会把上一版备份成 skin.json.bak（清单里的手写注释无法保留，
       备份是唯一的退路）。

    2. DesignerOverlay：盖在插件界面上的编辑层。显示所有元素矩形，支持点选、
       拖动移动、右下角拖动改尺寸、方向键 1 px 微调。它不是独立的渲染，
       只是把元素的真实 bounds 画出来，所以位置永远和实际控件一致。

    3. DesignerWindow：浮动属性面板。元素列表 + 布局 + 形态与交互 + 贴图槽位
       （导入图片会拷进皮肤的 assets/ 目录并写进清单，立刻预览）。
*/
namespace skin
{
    /** 设计器要操作的那份文档（清单 + 皮肤目录）。 */
    class SkinDocument
    {
    public:
        /** 当前皮肤是否可写（内部皮肤不可写，需要先"另存为可编辑副本"）。 */
        bool isWritable() const noexcept { return skinFolder.isDirectory(); }

        juce::File getFolder() const { return skinFolder; }

        /** 把当前皮肤另存为可编辑副本（写到用户文档目录），并载入外部皮肤。 */
        bool createEditableCopy (juce::String& error);

        /** 定位到当前皮肤目录；失败填充 error。 */
        bool attach (juce::String& error);

        /** 改矩形（自动补进清单）。 */
        void setRect (const juce::String& elementId, juce::Rectangle<int> rect);

        /** 改"形态与交互"参数（style / dragMode / sensitivity / trackInset / valueBox …）。 */
        void setParam (const juce::String& elementId, const juce::String& key, const juce::var& value);

        /** 元素的变换（元素自身写的键优先，其次 defaults.<type>）。 */
        Transform transformFor (const juce::String& elementId) const;

        /** 写元素级变换字段（rotation / pivot / offset / scale / fit）；写 void 表示删掉该键。 */
        void setTransformValue (const juce::String& elementId, const juce::String& key,
                                const juce::var& value);

        // ---- 运动轨迹（与运行时的 Skin::travelForPart 读同一份清单）----

        /** 元素级轨迹（元素自身优先，其次 defaults.<type>）。 */
        Travel travelFor (const juce::String& elementId) const;

        /** 某个部位的轨迹：在 travelFor() 之上再套 travel.parts.<part>。 */
        Travel travelForPart (const juce::String& elementId, const juce::String& part) const;

        /** 清单里为该元素（或 defaults.<type>）显式配置过轨迹的部位名。 */
        juce::StringArray travelPartsFor (const juce::String& elementId) const;

        /** 启用/停用一条轨迹。part 为空 = 元素级；否则写 travel.parts.<part>.enabled。 */
        void setTravelEnabled (const juce::String& elementId, const juce::String& part, bool enabled);

        /** 改控制点（index 0…3）。part 为空 = 元素级；否则写 travel.parts.<part>.p<index>。 */
        void setTravelPoint (const juce::String& elementId, const juce::String& part,
                             int index, juce::Point<float> point);

        /** 删除自定义轨迹（回到默认直线）。part 为空 = 删元素级 travel；否则删该部位。 */
        void resetTravel (const juce::String& elementId, const juce::String& part);

        /** 改文案。 */
        void setText (const juce::String& elementId, const juce::String& text);

        /** 把某个贴图槽位指向皮肤内的相对路径（空 = 清除）。 */
        void setAsset (const juce::String& elementId, const juce::String& slot, const juce::String& relativePath);

        /** 导入一张图片：拷进 assets/，返回可写进清单的相对路径（失败返回空）。 */
        juce::String importAsset (const juce::File& source, juce::String& error);

        /** 把一段 SVG 文本写成皮肤内的矢量资源，返回可写进清单的相对路径（失败返回空）。 */
        juce::String importVectorAsset (const juce::String& svgText, const juce::String& preferredName,
                                        juce::String& error);

        // ---- 装饰图层（清单顶层 layers[]）----

        struct LayerInfo
        {
            juce::String id, name, imagePath;
            bool visible = true;
            bool hasRect = false;
            juce::Rectangle<int> rect;
            float opacity = 1.0f;
            Transform transform;
        };

        /** 按清单顺序（先绘制 = 数组靠前）返回全部图层。 */
        std::vector<LayerInfo> layers() const;

        LayerInfo layerFor (const juce::String& layerId) const;

        /** 追加一个图层（默认矩形 = 画布中部 120×120），返回新 id。 */
        juce::String addLayer();

        void removeLayer (const juce::String& layerId);

        /** 在数组里前后移动；到达边界返回 false。 */
        bool moveLayer (const juce::String& layerId, int delta);

        /** 改图层字段（rect / visible / opacity / name / image / rotation / pivot …）。 */
        void setLayerValue (const juce::String& layerId, const juce::String& key, const juce::var& value);

        // ---- 控件图层（elements[].layers[]：叠在控件之上，随控件移动 / 缩放 / 旋转）----

        struct ControlLayerInfo
        {
            juce::String id, name, imagePath;
            bool visible = true;
            float opacity = 1.0f;

            /** 归一化矩形 [x, y, w, h]：相对所属控件的本地矩形。 */
            juce::Rectangle<float> rect { 0.0f, 0.0f, 1.0f, 1.0f };
            /** 清单里是否显式写了 rect（缺省 = 铺满控件）。 */
            bool hasRect = true;

            Transform transform;
        };

        /** 按清单顺序（先绘制 = 数组靠前）返回某个控件的全部图层。 */
        std::vector<ControlLayerInfo> controlLayers (const juce::String& elementId) const;

        ControlLayerInfo controlLayerFor (const juce::String& elementId, const juce::String& layerId) const;

        /** 往控件上追加一个图层（默认铺满控件），返回新 id；失败返回空。 */
        juce::String addControlLayer (const juce::String& elementId);

        void removeControlLayer (const juce::String& elementId, const juce::String& layerId);

        /** 在数组里前后移动；到达边界返回 false。 */
        bool moveControlLayer (const juce::String& elementId, const juce::String& layerId, int delta);

        /** 改图层字段（rect / visible / opacity / name / image / rotation / pivot …）。 */
        void setControlLayerValue (const juce::String& elementId, const juce::String& layerId,
                                   const juce::String& key, const juce::var& value);

        /** 写归一化矩形（面板与编辑层共用）。 */
        void setControlLayerRect (const juce::String& elementId, const juce::String& layerId,
                                  juce::Rectangle<float> normalizedRect);

        /** 写回 skin.json（备份旧版）并让 SkinBundle 重新加载，预览随即更新。 */
        bool writeThrough (juce::String& error);

        /** 当前元素的矩形（清单里没写时返回 fallback）。 */
        juce::Rectangle<int> rectFor (const juce::String& elementId, juce::Rectangle<int> fallback) const;

        juce::var paramFor (const juce::String& elementId, const juce::String& key, const juce::var& fallback) const;

        juce::String textFor (const juce::String& elementId) const;

        juce::String assetFor (const juce::String& elementId, const juce::String& slot) const;

        // ---- 动态着色器（阶段三 3c）----

        /** 清单里 `shader.source` 指向的相对路径；空 = 这套皮肤没有着色器。 */
        juce::String shaderSourcePath() const;

        /** 设置 `shader.source`（空串 = 移除这一项）。 */
        void setShaderSourcePath (const juce::String& relativePath);

        /** 读取皮肤目录内的文本文件（着色器源码）；缺失返回空串。 */
        juce::String readSkinFile (const juce::String& relativePath) const;

        /** 把着色器源码写到皮肤目录内的相对路径（覆盖写）。 */
        bool writeSkinFile (const juce::String& relativePath, const juce::String& text,
                            juce::String& error);

        /** 把外部 .glsl 拷进皮肤的 `shaders/` 目录，返回可写进清单的相对路径。 */
        juce::String importShader (const juce::File& source, juce::String& error);

        /** 取 / 设 `shader.params` 下的值（例如帧率）。 */
        juce::var shaderParam (const juce::String& name, const juce::var& fallback) const;
        void      setShaderParam (const juce::String& name, const juce::var& value);

        // ---- 窗口与异形（阶段三 3e）----

        /** 清单里的 `window` 对象（不存在时按需创建）。写不存在的键时用它兜底。 */
        juce::DynamicObject* windowObject (bool createIfMissing);

        /** 出血（窗口比画布大出来的四边）；清单没写就是 0。 */
        juce::Rectangle<int> windowExpand() const;

        /** 改某一侧的出血（side: 0=左 1=上 2=右 3=下）。 */
        void setWindowExpand (int side, int pixels);

        /** 无标题栏（仅 Standalone 生效）。 */
        bool frameless() const;
        void setFrameless (bool shouldBeFrameless);

        /** 拖动区："auto" 或 "rect"；后者用下面的矩形（设计坐标）。 */
        juce::String dragAreaKind() const;
        juce::Rectangle<int> dragAreaRect() const;
        void setDragAreaAuto();
        void setDragAreaRect (juce::Rectangle<int> rect);

        /** `shape.mode`：off / auto / force。 */
        juce::String shapeMode() const;
        void setShapeMode (const juce::String& mode);

        /** `shape.mask` 的相对路径（空 = 没有异形）。 */
        juce::String shapeMaskPath() const;
        void setShapeMaskPath (const juce::String& relativePath);

        // ---- 方案预设（整份设计的完整快照）----

        /** 预设根目录：<用户配置>/OnlyObsessionRemained/designer-presets。 */
        static juce::File presetsFolder();

        /** 现有预设名（按字母序）。 */
        juce::StringArray listPresets() const;

        /** 把当前设计整体存成预设：内存里的清单（含尚未落盘的修改）+ assets/ + shaders/。
            已存在同名预设则整份覆盖。 */
        bool savePreset (const juce::String& name, juce::String& error);

        /** 用预设整体替换当前皮肤内容，并重新载入预览（会丢失当前未保存的改动）。 */
        bool loadPreset (const juce::String& name, juce::String& error);

        bool deletePreset (const juce::String& name, juce::String& error);

        /** 内置预设名：编译进二进制的默认布局（skins/<SKIN_NAME>，当前是 default）。
            它不来自预设目录，所以列表里永远排第一条、不可删除也不可覆盖。 */
        static juce::String builtInPresetName();
        static bool isBuiltInPreset (const juce::String& name);

        /** 把内置默认皮肤（构建期打包的那份，见 tools/skin_pack.cpp）整体写回当前
            皮肤目录并热重载 —— 这是"随时回到默认布局看看"的退路。
            调用前面板会把当前设计存成「恢复前备份」预设，所以改到一半的东西回得来。 */
        bool restoreBuiltIn (juce::String& error);

    private:
        juce::var  manifest;
        juce::File skinFolder;

        /** 从皮肤目录重读清单（attach / loadPreset 共用）。 */
        bool readManifestFromDisk (juce::String& error);

        juce::var& elementNode (const juce::String& elementId, bool createIfMissing);

        /** 清单里的 `shader` 对象（不存在时按需创建）。返回裸指针而非 var& ——
            JUCE 的 DynamicObject::getProperty 只给 const 引用，拿不到可写的引用；
            而 var 的副本内部共享同一个 DynamicObject，所以按指针改是安全的。 */
        juce::DynamicObject* shaderObject (bool createIfMissing);
    };

    /** 插件界面上的编辑层：点选 / 拖动 / 缩放元素矩形。 */
    class DesignerOverlay : public juce::Component
    {
    public:
        /** 元素几何信息由编辑器提供（永远与实际控件一致）。 */
        struct Item
        {
            juce::String id, type, label;
            bool hasRect = false;
            juce::Rectangle<int> rect;

            /** 清单顶层 layers[] 里的一项（不是控件）。 */
            bool isLayer = false;

            /** 某个控件的图层（ownerId 是控件 id，layerId 是 elements[].layers[] 里的 id）。 */
            bool isControlLayer = false;
            juce::String ownerId, layerId;

            /** 控件图层的归一化矩形（相对 owner 控件）；非控件图层不用。 */
            juce::Rectangle<float> normalizedRect { 0.0f, 0.0f, 1.0f, 1.0f };

            /** 控件图层自己的变换（在 owner 的元素变换之上再套一层）。 */
            Transform layerTransform;

            /** 元素级变换：编辑层据此画出旋转后的框，拖动命中也要走它。 */
            Transform transform;

            /** 元素级运动轨迹（贝塞尔控制点，坐标是控件矩形的归一化值）。 */
            Travel travel;

            /** 各部位自己的轨迹；空串不代表任何部位，键就是清单里的部位名。 */
            std::vector<std::pair<juce::String, Travel>> travelParts;
        };

        DesignerOverlay();

        void setItems (std::vector<Item> newItems);
        void setSelected (const juce::String& elementId);
        juce::String getSelected() const { return selected; }

        /** 3e：画布在编辑层本地坐标里的矩形 + 形状蒙版（空图 = 没有异形）。
            编辑层据此画出"出血区"与画布边框，并把蒙版以低不透明度叠一层当作形状轮廓。 */
        void setCanvasInfo (juce::Rectangle<int> canvasArea, juce::Image shapeMaskImage);

        /** 当前正在编辑的轨迹部位（空 = 元素整体）。同一位置的多个控制点只让"当前部位"接管命中，
            避免几条重合曲线互相抢鼠标。 */
        void setActiveTravelPart (const juce::String& part);
        juce::String getActiveTravelPart() const { return activeTravelPart; }

        /** 拖动/缩放过程中的回调（用于把改动写进文档）。 */
        std::function<void (const juce::String& id, juce::Rectangle<int> rect)> onRectChanged;
        std::function<void (const juce::String& id)> onSelectionChanged;

        /** 拖动过程中的即时预览（只改画面，不写文件）。 */
        std::function<void (const juce::String& id, juce::Rectangle<int> rect)> onRectPreview;

        /** 轨迹控制点拖拽结束（松手才回调；part 为空 = 元素级，index 0…3）。 */
        std::function<void (const juce::String& id, const juce::String& part,
                            int index, juce::Point<float> point)> onTravelPointChanged;

        /** 选中的是图层（与控件分开，编辑器按图层路径写盘）。 */
        std::function<void (const juce::String& layerId)> onLayerSelected;
        std::function<void (const juce::String& layerId, juce::Rectangle<int> rect)> onLayerRectPreview;
        std::function<void (const juce::String& layerId, juce::Rectangle<int> rect)> onLayerRectChanged;

        /** 选中的是某个控件的图层（面板据此切到「控件图层」分区）。 */
        std::function<void (const juce::String& ownerId, const juce::String& layerId)> onControlLayerSelected;
        std::function<void (const juce::String& ownerId, const juce::String& layerId,
                            juce::Rectangle<float> normalizedRect)> onControlLayerRectPreview;
        std::function<void (const juce::String& ownerId, const juce::String& layerId,
                            juce::Rectangle<float> normalizedRect)> onControlLayerRectChanged;

        /** 3e：编辑层上"空白处"的按下与拖动（没命中任何条目时）。
            异形窗口的无标题栏拖动、右键菜单都挂在窗口空白处，编辑层开着时会把这些点击吃掉，
            所以这里把它们原样转交给编辑器。 */
        std::function<void (const juce::MouseEvent&)> onEmptyAreaPressed;
        std::function<void (const juce::MouseEvent&)> onEmptyAreaDragged;

        void paint (juce::Graphics&) override;
        void mouseDown (const juce::MouseEvent&) override;
        void mouseDrag (const juce::MouseEvent&) override;
        void mouseUp (const juce::MouseEvent&) override;
        bool keyPressed (const juce::KeyPress&) override;

    private:
        const Item* findItem (juce::Point<int> position) const;
        const Item* currentItem() const;

        /** 缩放手把所在的方块（跟着旋转走：始终贴在矩形的右下角）。 */
        juce::Rectangle<float> handleArea (const Item& item) const;

        /** 控件的局部坐标 → 编辑层坐标（元素变换之后）。 */
        juce::AffineTransform overlayTransform (const Item& item) const;

        /** 条目在编辑层里的矩形（控件图层会换算成 owner 控件里的实际区域）。 */
        juce::Rectangle<float> itemArea (const Item& item) const;

        /** 条目从"自身局部坐标"到编辑层坐标的完整仿射（控件图层 = 图层变换 → 元素变换）。 */
        juce::AffineTransform itemAffine (const Item& item) const;

        /** 命中判定：先把点逆变换回条目自身的局部坐标，再做矩形包含测试。 */
        bool itemHitTest (const Item& item, juce::Point<int> position) const;

        /** 只命中“虚线框那一圈”：让铺满控件的图层不抢走控件内部的点击。 */
        bool itemFrameHitTest (const Item& item, juce::Point<int> position) const;

        /** 控件图层的框只在"它所属控件被选中"时显示，免得满屏都是虚线。 */
        bool itemIsVisibleInOverlay (const Item& item) const;

        /** 某个控制点在编辑层里的位置。 */
        juce::Point<float> travelHandlePosition (const Item& item, const Travel& travel,
                                                 int index) const;

        std::vector<Item> items;
        juce::String selected;
        juce::String activeTravelPart;

        juce::Rectangle<int> canvasArea;   // 3e：画布（设计区）在编辑层里的矩形
        juce::Image          shapeMaskImage; // 3e：形状蒙版（空 = 矩形窗口）

        bool dragging = false, resizing = false;
        juce::Point<int> dragStart;
        juce::Rectangle<int> rectAtDragStart;

        /** 控件图层拖动 / 缩放开始时的归一化矩形。 */
        juce::Rectangle<float> normalizedAtDragStart { 0.0f, 0.0f, 1.0f, 1.0f };

        /** 正在拖的轨迹控制点：travelDragPoint < 0 = 没在拖；part 为空 = 元素级。 */
        int travelDragPoint = -1;
        juce::String travelDragPart;

        static constexpr int kHandleSize = 10;
        static constexpr float kTravelHandleRadius = 8.0f;

        /** 控件图层虚线框的点击容差（只命中框线附近，不抢控件内部的点击）。 */
        static constexpr float kFrameHitTolerance = 6.0f;
    };

    /** 浮动属性面板窗口。 */
    class DesignerWindow : public juce::DocumentWindow
    {
    public:
        /**
            设计器面板需要的回调，由编辑器提供。
            收成一个结构，是因为原来的三个 lambda 已经够多，再零星加两个就会失控。
        */
        struct Callbacks
        {
            /** 元素 id → 该元素当前的真实几何（面板兜底用）。 */
            std::function<juce::Rectangle<int> (const juce::String&)> editorBoundsFor;

            /** 文档已改动（面板写完盘后回调，让编辑层刷新）。 */
            std::function<void()> documentChanged;

            /** 用户在面板里选了某个元素（同步给编辑层的选中态）。 */
            std::function<void (const juce::String&)> elementSelected;

            /** 用户在面板里换了"正在编辑的轨迹部位"（空 = 整体）。 */
            std::function<void (const juce::String&)> travelPartSelected;

            /** 着色器最近一次编译错误；空 = 正常。 */
            std::function<juce::String()> shaderCompileError;

            /** 着色器运行状态一句话（GPU 就绪 / 已降级 / 未启用），用于面板提示。 */
            std::function<juce::String()> shaderRuntimeStatus;
        };

        DesignerWindow (SkinDocument& document, Callbacks callbacks);
        ~DesignerWindow() override;

        void closeButtonPressed() override;

        /** 选择某个元素并刷新面板。 */
        void selectElement (const juce::String& elementId);

        /** 选择某个图层（面板切到图层管理页）。 */
        void selectLayer (const juce::String& layerId);

        /** 选择某个控件图层（面板留在控件页，滚到「控件图层」分区）。 */
        void selectControlLayer (const juce::String& ownerId, const juce::String& layerId);

        void refreshAll();

        /** 皮肤换了：面板里的着色器编辑区必须跟着换皮肤的文件（否则会把上一套的源码写过去）。 */
        void onSkinChanged();

    private:
        class Panel;
        std::unique_ptr<Panel> panel;
    };
}