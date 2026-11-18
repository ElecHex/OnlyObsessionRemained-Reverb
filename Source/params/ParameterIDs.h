#pragma once

/**
    参数 ID 与枚举的唯一来源。
    所有插件参数（Processor / Editor / 引擎映射）都必须引用这里的常量，禁止散落字符串字面量。
*/
namespace params
{
    inline constexpr const char* type      = "type";
    inline constexpr const char* mix       = "mix";
    inline constexpr const char* decay     = "decay";
    inline constexpr const char* preDelay  = "preDelay";
    inline constexpr const char* mod       = "mod";
    inline constexpr const char* highDamp  = "highDamp";
    inline constexpr const char* lowDamp   = "lowDamp";
    inline constexpr const char* shimmer   = "shimmer";

    // JUCE 要求每个参数带版本号，用于后续参数结构演进的兼容处理
    inline constexpr int versionHint = 1;
}