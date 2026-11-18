#pragma once

/**
    出厂预设表（纯数据，不依赖 JUCE）。

    单独抽成头文件的目的是让插件（PresetManager）与离线渲染工具（onlyobsessionremained_render）
    共用同一份数据，保证"试听到的 = 插件里能选到的"，不会各自维护而漂移。

    参数按插件 UI 的原始单位给出：0..100（预延迟为 ms）。
*/
namespace factory_presets
{
    struct Preset
    {
        const char* name;
        int   type;         // 0 板式 / 1 房间 / 2 弹簧
        float mix;
        float decay;
        float preDelayMs;
        float mod;
        float highDamp;
        float lowDamp;
        float shimmer;
    };

    inline constexpr Preset kPresets[] =
    {
        { "Plate / Medium",    0, 30.0f, 40.0f,  20.0f, 50.0f, 50.0f, 20.0f,   0.0f },
        { "Plate / Long Tail", 0, 35.0f, 78.0f,  30.0f, 55.0f, 45.0f, 15.0f,   0.0f },
        { "Plate / Bright",    0, 28.0f, 45.0f,  10.0f, 40.0f, 18.0f,  8.0f,   0.0f },
        { "Plate / Dark",      0, 32.0f, 60.0f,  25.0f, 50.0f, 80.0f, 25.0f,   0.0f },
        { "Room / Small",      1, 25.0f, 30.0f,   8.0f, 45.0f, 55.0f, 25.0f,   0.0f },
        { "Room / Hall",       1, 42.0f, 80.0f,  45.0f, 50.0f, 50.0f, 18.0f,   0.0f },
        { "Spring / Drip",     2, 35.0f, 45.0f,   8.0f, 60.0f, 45.0f, 25.0f,   0.0f },
        { "Spring / Long",     2, 40.0f, 72.0f,  20.0f, 55.0f, 40.0f, 20.0f,   0.0f },
        { "Shimmer / Pad",     0, 50.0f, 85.0f,  40.0f, 60.0f, 45.0f, 25.0f,  65.0f },
        { "Shimmer / Bloom",   1, 45.0f, 80.0f,  30.0f, 55.0f, 40.0f, 20.0f, 100.0f },
        { "Shimmer / Spring",  2, 45.0f, 70.0f,  25.0f, 60.0f, 40.0f, 20.0f,  80.0f },
    };

    inline constexpr int kCount = static_cast<int> (sizeof (kPresets) / sizeof (kPresets[0]));
}