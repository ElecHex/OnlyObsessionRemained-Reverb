#include "ui/WindowShaper.h"

#include "ui/UiText.h"

#include <vector>

#if JUCE_WINDOWS
 #include <windows.h>
#endif

namespace skin
{
    namespace window_shape
    {
        namespace
        {
            juce::String lastFailure;

            /** 蒙版采样阈值：与文档一致 —— alpha > 128 处属于窗口。 */
            constexpr int kAlphaThreshold = 128;

           #if JUCE_WINDOWS
            HWND hwndOf (juce::Component& component)
            {
                if (auto* peer = component.getPeer())
                    return static_cast<HWND> (peer->getNativeHandle());

                return {};
            }

            /** 客户区左上角在**窗口矩形**里的偏移。SetWindowRgn 用的是窗口坐标，
                而目标组件的本地坐标是相对客户区的，中间差着非客户区那圈边框。 */
            bool clientOriginInWindow (HWND hwnd, int& x, int& y)
            {
                RECT windowRect {};

                if (GetWindowRect (hwnd, &windowRect) == 0)
                    return false;

                POINT zero { 0, 0 };
                ClientToScreen (hwnd, &zero);

                x = zero.x - windowRect.left;
                y = zero.y - windowRect.top;
                return true;
            }

            /** 逻辑矩形 → 窗口物理像素矩形（先乘 DPI 缩放，再加客户区偏移）。 */
            RECT toWindowPixels (juce::Rectangle<int> area, double scale, int clientX, int clientY)
            {
                return { clientX + juce::roundToInt (area.getX()      * scale),
                         clientY + juce::roundToInt (area.getY()      * scale),
                         clientX + juce::roundToInt (area.getRight()  * scale),
                         clientY + juce::roundToInt (area.getBottom() * scale) };
            }
           #endif
        }

        bool isSupported()
        {
           #if JUCE_WINDOWS
            return true;
           #else
            return false;
           #endif
        }

        juce::String lastError()
        {
            return lastFailure;
        }

        void clearFromWindow (juce::Component& target)
        {
            lastFailure.clear();

           #if JUCE_WINDOWS
            if (auto hwnd = hwndOf (target))
                SetWindowRgn (hwnd, nullptr, TRUE);   // 传 nullptr = 恢复成整个矩形窗口
           #else
            juce::ignoreUnused (target);
           #endif
        }

        bool applyToWindow (juce::Component& target, const juce::Image& mask,
                            juce::Rectangle<int> maskArea, juce::Rectangle<int> extraArea)
        {
            lastFailure.clear();

            if (mask.isNull() || mask.getWidth() <= 0 || mask.getHeight() <= 0)
            {
                lastFailure = U8 ("蒙版图是空的");
                return false;
            }

            if (maskArea.isEmpty())
            {
                lastFailure = U8 ("蒙版区是空的");
                return false;
            }

           #if JUCE_WINDOWS
            auto* peer = target.getPeer();

            if (peer == nullptr)
            {
                lastFailure = U8 ("窗口还没建好（组件没有 peer）");
                return false;
            }

            const auto hwnd = hwndOf (target);

            if (hwnd == nullptr)
            {
                lastFailure = U8 ("拿不到窗口句柄");
                return false;
            }

            int clientX = 0, clientY = 0;

            if (! clientOriginInWindow (hwnd, clientX, clientY))
            {
                lastFailure = U8 ("读不到窗口边框");
                return false;
            }

            auto scale = peer->getPlatformScaleFactor();

            if (scale <= 0.0)
                scale = 1.0;

            const auto areaPixels = toWindowPixels (maskArea, scale, clientX, clientY);

            const int areaW = areaPixels.right - areaPixels.left;
            const int areaH = areaPixels.bottom - areaPixels.top;

            if (areaW <= 0 || areaH <= 0)
            {
                lastFailure = U8 ("蒙版区换算成像素后为空");
                return false;
            }

            // 逐行 run-length：区域只能是矩形集合（CreateRectRgn / ExtCreateRegion），
            // 所以这里不做曲线拟合 —— 代价是硬边，换来的是能和 GL 背景共存
            std::vector<RECT> rects;
            rects.reserve (static_cast<size_t> (areaH));

            const int maskW = mask.getWidth();
            const int maskH = mask.getHeight();

            for (int y = areaPixels.top; y < areaPixels.bottom; ++y)
            {
                // 采样行的蒙版纵坐标（最近邻：蒙版被拉伸到整个蒙版区）
                const int maskY = juce::jlimit (0, maskH - 1,
                                                (y - static_cast<int> (areaPixels.top)) * maskH / areaH);

                int runStart = -1;

                for (int x = areaPixels.left; x < areaPixels.right; ++x)
                {
                    const int maskX = juce::jlimit (0, maskW - 1,
                                                    (x - static_cast<int> (areaPixels.left)) * maskW / areaW);

                    const bool inside = mask.getPixelAt (maskX, maskY).getAlpha() > kAlphaThreshold;

                    if (inside)
                    {
                        if (runStart < 0)
                            runStart = x;
                    }
                    else if (runStart >= 0)
                    {
                        rects.push_back ({ runStart, y, x, y + 1 });
                        runStart = -1;
                    }
                }

                if (runStart >= 0)
                    rects.push_back ({ runStart, y, areaPixels.right, y + 1 });
            }

            // 标题栏之类的"额外要保留的区域"（Standalone 没隐标题栏时用）
            if (! extraArea.isEmpty())
            {
                rects.push_back (toWindowPixels (extraArea, scale, clientX, clientY));
            }

            if (rects.empty())
            {
                lastFailure = U8 ("蒙版里没有被保留的区域");
                clearFromWindow (target);
                return false;
            }

            RECT bounds = rects.front();

            for (const auto& r : rects)
            {
                bounds.left   = juce::jmin (bounds.left,   r.left);
                bounds.top    = juce::jmin (bounds.top,    r.top);
                bounds.right  = juce::jmax (bounds.right,  r.right);
                bounds.bottom = juce::jmax (bounds.bottom, r.bottom);
            }

            const size_t bytes = sizeof (RGNDATAHEADER) + rects.size() * sizeof (RECT);
            std::vector<char> buffer (bytes);

            auto* regionData = reinterpret_cast<RGNDATA*> (buffer.data());
            regionData->rdh.dwSize   = sizeof (RGNDATAHEADER);
            regionData->rdh.iType    = RDH_RECTANGLES;
            regionData->rdh.nCount   = static_cast<DWORD> (rects.size());
            regionData->rdh.nRgnSize = static_cast<DWORD> (rects.size() * sizeof (RECT));
            regionData->rdh.rcBound  = bounds;

            std::memcpy (regionData->Buffer, rects.data(), rects.size() * sizeof (RECT));

            HRGN region = ExtCreateRegion (nullptr, static_cast<DWORD> (bytes), regionData);

            if (region == nullptr)
            {
                lastFailure = U8 ("创建窗口区域失败");
                return false;
            }

            if (SetWindowRgn (hwnd, region, TRUE) == 0)
            {
                // 失败时区域还在我们手里，必须自己删；成功时系统接管，不能删
                DeleteObject (region);
                lastFailure = U8 ("设置窗口区域失败");
                return false;
            }

            return true;
           #else
            juce::ignoreUnused (target, mask, maskArea, extraArea);
            lastFailure = U8 ("当前平台不支持窗口区域裁剪");
            return false;
           #endif
        }
    }
}
