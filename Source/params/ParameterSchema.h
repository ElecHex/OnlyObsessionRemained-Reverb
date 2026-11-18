#pragma once

#include <cstring>

#include "params/ParameterIDs.h"

namespace params
{
    /**
        一个连续参数的全部元数据：范围、默认值、显示名、HUD 短名、皮肤元素 id、
        以及"原始值 → 引擎单位"的换算系数。

        这里是**唯一**来源：
          - Processor 用它生成 APVTS 布局（createParameterLayout）
          - Processor 用它把界面值换算成 ReverbEngine::Parameters
          - Editor 用它拿到参数 id / 显示名 / 皮肤 id
          - HudView 用它拿到控件短名

        以前这四份信息散在 createParameterLayout、readEngineParameters 里手写的
        `* 0.01f`、Editor 的三个数组和 HudView 的字符串字面量里，改一处不报错。
        新增参数时只改这张表。
    */

    struct FloatSpec
    {
        const char* id;            // APVTS 参数 ID（见 ParameterIDs.h）
        const char* displayName;   // 宿主与编辑器显示名
        const char* hudName;       // HUD 控件短名（已大写）
        const char* skinId;        // 皮肤清单里的元素 id
        float       minValue;
        float       maxValue;
        float       step;
        float       defaultValue;
        const char* unit;          // 参数单位标签（"" 表示无）
        float       engineScale;   // 界面值 × engineScale = 引擎单位
    };

    // 顺序即 Editor 里 7 个旋钮的顺序，也是 HUD 之外所有按序号取参数的顺序。
    inline constexpr FloatSpec kFloatSpecs[] =
    {
        { mix,      "Mix",       "MIX",       "mix",      0.0f, 100.0f, 0.01f, 30.0f, "%",  0.01f },
        { decay,    "Decay",     "DECAY",     "decay",    0.0f, 100.0f, 0.01f, 40.0f, "%",  0.01f },
        { preDelay, "Pre-Delay", "PRE-DELAY", "preDelay", 0.0f, 500.0f, 0.10f, 20.0f, "ms", 1.0f  },
        { mod,      "Mod",       "MOD",       "mod",      0.0f, 100.0f, 0.01f, 50.0f, "%",  0.01f },
        { highDamp, "High Damp", "HIGH DAMP", "highDamp", 0.0f, 100.0f, 0.01f, 50.0f, "%",  0.01f },
        { lowDamp,  "Low Damp",  "LOW DAMP",  "lowDamp",  0.0f, 100.0f, 0.01f, 20.0f, "%",  0.01f },
        { shimmer,  "Shimmer",   "SHMR",      "shimmer",  0.0f, 100.0f, 0.01f,  0.0f, "%",  0.01f },
    };

    inline constexpr int kNumFloatSpecs =
        static_cast<int> (sizeof (kFloatSpecs) / sizeof (kFloatSpecs[0]));

    /** 按语义取下标，避免调用处记序号。 */
    enum FloatIndex
    {
        idxMix = 0,
        idxDecay,
        idxPreDelay,
        idxMod,
        idxHighDamp,
        idxLowDamp,
        idxShimmer
    };

    /** Type 是 choice 参数，单独给出选项表，避免 "Plate/Room/Spring" 再散落一份。 */
    inline constexpr const char* kTypeChoices[] = { "Plate", "Room", "Spring" };
    inline constexpr int kNumTypeChoices =
        static_cast<int> (sizeof (kTypeChoices) / sizeof (kTypeChoices[0]));

    inline const FloatSpec& floatSpec (int index) noexcept { return kFloatSpecs[index]; }

    /** 界面值 → 引擎单位。 */
    inline float engineValue (int index, float raw) noexcept
    {
        return raw * kFloatSpecs[index].engineScale;
    }

    /** 按参数 id 取 HUD 短名；找不到时返回空串（HUD 控件标签用）。 */
    inline const char* hudNameFor (const char* id) noexcept
    {
        for (const auto& spec : kFloatSpecs)
            if (std::strcmp (spec.id, id) == 0)
                return spec.hudName;

        return "";
    }

    /** 按参数 id 取皮肤元素 id；找不到时返回空串。 */
    inline const char* skinIdFor (const char* id) noexcept
    {
        for (const auto& spec : kFloatSpecs)
            if (std::strcmp (spec.id, id) == 0)
                return spec.skinId;

        return "";
    }
}
