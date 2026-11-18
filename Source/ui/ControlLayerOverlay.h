#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "ui/SkinBundle.h"

namespace skin
{
    /**
        控件图层渲染层（`elements[].layers[]` 的运行时落点）。

        做法：给每个带图层的控件挂一个透明子组件。因为子组件的坐标是**相对父控件**的，
        所以控件移动、缩放、旋转（Component::setTransform）时图层天然跟随，不需要每帧
        自己算一次全局位置；绘制顺序也天然是"控件内容之上、编辑层之下"。

        命中：setInterceptsMouseClicks(false, false)，鼠标事件穿透回控件本身
        （旋钮拖动、文本框点击都不受影响）。

        每帧读取 SkinBundle 的当前皮肤：换皮肤/热重载后指针不需要特殊维护。
    */
    class ControlLayerOverlay final : public juce::Component
    {
    public:
        explicit ControlLayerOverlay (juce::String trackedElementId)
            : elementId (std::move (trackedElementId))
        {
            setInterceptsMouseClicks (false, false);
            setOpaque (false);
        }

        void setElementId (const juce::String& newId)
        {
            if (elementId == newId)
                return;

            elementId = newId;
            repaint();
        }

        juce::String getElementId() const { return elementId; }

        /**
            设计器拖动中的临时布局（只改画面，松手才落盘）。
            normalizedRect 相对控件矩形（0…1）；layerId 为空 = 取消预览。
        */
        void setPreview (const juce::String& layerId, juce::Rectangle<float> normalizedRect)
        {
            previewLayerId = layerId;
            previewRect    = normalizedRect;
            repaint();
        }

        void clearPreview()
        {
            if (previewLayerId.isEmpty())
                return;

            previewLayerId.clear();
            repaint();
        }

        void paint (juce::Graphics& g) override
        {
            const auto& skin = SkinBundle::getInstance().getCurrent();
            const auto* element = skin.findElement (elementId);

            if (element == nullptr || element->layers.empty())
                return;

            const auto local = getLocalBounds().toFloat();

            if (local.isEmpty())
                return;

            for (const auto& layer : element->layers)
            {
                if (! layer.visible)
                    continue;

                const auto image = skin.imageFor (layer.imagePath);

                if (image.isNull())
                    continue;

                auto normalized = layer.rect;

                if (previewLayerId.isNotEmpty() && previewLayerId == layer.id)
                    normalized = previewRect;

                const auto area = juce::Rectangle<float> (normalized.getX() * local.getWidth(),
                                                          normalized.getY() * local.getHeight(),
                                                          normalized.getWidth() * local.getWidth(),
                                                          normalized.getHeight() * local.getHeight());

                if (area.isEmpty())
                    continue;

                // 变换与透明度都要还原：否则同一层后面画的图层会一直带着它们
                const juce::Graphics::ScopedSaveState save (g);

                if (! layer.transform.isIdentity())
                    g.addTransform (layer.transform.toAffine (area));

                g.setOpacity (juce::jlimit (0.0f, 1.0f, layer.opacity));
                g.drawImage (image, area, layer.transform.placement (false));
            }
        }

    private:
        juce::String elementId;
        juce::String previewLayerId;
        juce::Rectangle<float> previewRect;

        JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (ControlLayerOverlay)
    };
}
