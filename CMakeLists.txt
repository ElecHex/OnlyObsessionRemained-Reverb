cmake_minimum_required(VERSION 3.16)

project(OnlyObsessionRemainedReverb VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 源码注释为中文（UTF-8）。MSVC 默认按本地代码页解析，会把中文注释的尾字节
# 误吞到下一行，因此显式声明源与执行字符集均为 UTF-8。
if (MSVC)
    add_compile_options(/utf-8)
endif()

# 三个混响腔体只依赖 dsp/core 里的纯 C++ 基础件，不依赖 JUCE。
add_library(oob_reverb STATIC
    Source/dsp/tanks/PlateTank.cpp
    Source/dsp/tanks/RoomTank.cpp
    Source/dsp/tanks/SpringTank.cpp
)

target_include_directories(oob_reverb PUBLIC Source)

if (MSVC)
    target_compile_options(oob_reverb PRIVATE /W4)
else()
    target_compile_options(oob_reverb PRIVATE -Wall -Wextra)
endif()

# 离线验证 harness：不需要音频设备、不需要插件宿主，直接量化验证
# RT60 标定、阻尼频带差异、色散、回声密度、稳定性与非线性压缩量。
add_executable(plate_offline_test offline/PlateOfflineTest.cpp)
target_link_libraries(plate_offline_test PRIVATE oob_reverb)
target_include_directories(plate_offline_test PRIVATE Source)

enable_testing()
add_test(NAME plate_offline COMMAND plate_offline_test)
set_tests_properties(plate_offline PROPERTIES TIMEOUT 600)
