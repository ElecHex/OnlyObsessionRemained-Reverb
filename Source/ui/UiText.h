#pragma once

#include <juce_core/juce_core.h>

/**
    源代码里的中文字面量统一用 U8(...) 包一层。

    为什么必须这样：JUCE 的 String(const char*) 按 Latin-1 解释字节
    （见 juce_String.cpp 里那条 "values greater than 127" 断言），而 MSVC 在 /utf-8 下
    把字面量写成 UTF-8 字节——直接写 "中文" 会被解释成一串带音标的拉丁字符，
    界面上就是乱码。ASCII 字面量不受影响，所以只有中文（或任何非 ASCII）才需要包。

    界面文字来自皮肤清单时不用管：JSON 是 UTF-8，解析路径本来就是对的。
*/
#define U8(x) juce::String::fromUTF8 (x)