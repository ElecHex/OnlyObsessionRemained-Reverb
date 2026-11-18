cmake_minimum_required(VERSION 3.22)

project(OnlyObsessionRemained VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 源码为 UTF-8（含中文注释）。MSVC 默认按本地代码页解析，会把中文注释的
# 尾字节误吞到下一行，因此显式声明源与执行字符集均为 UTF-8。
if (MSVC)
    add_compile_options(/utf-8)
endif()

# JUCE 8.0.15 通过 FetchContent 拉取（首次配置需要联网，之后走构建目录里的缓存）。
# 版本必须锁死：音频标定、着色器契约都是在这一个版本上验证的。
set(EXPECTED_JUCE_VERSION "8.0.15")

include(FetchContent)

FetchContent_Declare(JUCE
    GIT_REPOSITORY https://github.com/juce-framework/JUCE.git
    GIT_TAG 8.0.15
    GIT_SHALLOW TRUE
)

FetchContent_MakeAvailable(JUCE)

# project() 只在子目录作用域里定义 JUCE_VERSION，父目录读不到；直接读它源码树里的
# CMakeLists 的 project 行，得到实际版本。
file(READ "${juce_SOURCE_DIR}/CMakeLists.txt" _juce_cmake_text)
string(REGEX MATCH "project\\(JUCE VERSION ([0-9]+\\.[0-9]+\\.[0-9]+)" _juce_version_match "${_juce_cmake_text}")
set(_juce_version "${CMAKE_MATCH_1}")

if (NOT _juce_version STREQUAL EXPECTED_JUCE_VERSION)
    message(FATAL_ERROR
        "JUCE 版本不匹配：期望 ${EXPECTED_JUCE_VERSION}，实际 ${_juce_version}。"
        "本项目的声音标定与着色器契约都在 ${EXPECTED_JUCE_VERSION} 上验证，"
        "升级前先跑离线测试与截图验收，再更新 EXPECTED_JUCE_VERSION。")
endif()

juce_add_plugin(OnlyObsessionRemained
    PRODUCT_NAME "OnlyObsessionRemained"
    COMPANY_NAME "OnlyObsessionRemained"
    # 这两个 4 字符短码与 PRODUCT_NAME 一起构成 VST3 的插件唯一 ID。
    # 改名会让 ID 变化，宿主会把本版当作一个"新插件"（旧工程里引用的旧实例不会自动映射）。
    PLUGIN_MANUFACTURER_CODE Oobs
    PLUGIN_CODE Ormd
    IS_SYNTH FALSE
    NEEDS_MIDI_INPUT FALSE
    NEEDS_MIDI_OUTPUT FALSE
    IS_MIDI_EFFECT FALSE
    EDITOR_WANTS_KEYBOARD_FOCUS FALSE
    COPY_PLUGIN_AFTER_BUILD FALSE
    FORMATS VST3 Standalone
)

target_sources(OnlyObsessionRemained PRIVATE
    Source/PluginProcessor.cpp
    Source/PluginEditor.cpp
    Source/presets/PresetManager.cpp
    Source/dsp/ReverbEngine.cpp
    Source/dsp/tanks/PlateTank.cpp
    Source/dsp/tanks/RoomTank.cpp
    Source/dsp/tanks/SpringTank.cpp
    Source/ui/SkinBundle.cpp
    Source/ui/SkinDesigner.cpp
    Source/ui/ShaderBackground.cpp
    Source/ui/SkinnedLookAndFeel.cpp
    Source/ui/WindowShaper.cpp
    Source/ui/HudView.cpp
)

target_include_directories(OnlyObsessionRemained PRIVATE Source)

# ---- 皮肤资产管线 ----
# skins/<名字>/ 下的清单与贴图由 tools/skin_pack.cpp 编成 SkinBinaryData.cpp。
# 皮肤目录里任何文件变化都会重新打包并重链接插件；因此"导入贴图 → 重新编译"
# 是一条自动链路，美术不需要手动跑任何命令。
# 想换内置皮肤：cmake -DSKIN_NAME=你的目录名 ...
add_executable(skin_pack tools/skin_pack.cpp)
target_compile_features(skin_pack PRIVATE cxx_std_17)

set(SKIN_NAME "default" CACHE STRING "编译进插件的皮肤名（对应 skins/<名字>）")
set(SKIN_DIR "${CMAKE_CURRENT_SOURCE_DIR}/skins/${SKIN_NAME}")
set(SKIN_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")

if(NOT EXISTS "${SKIN_DIR}/skin.json")
    message(FATAL_ERROR "找不到皮肤清单：${SKIN_DIR}/skin.json（SKIN_NAME = ${SKIN_NAME}）")
endif()

# 只把 skin_pack 真正会打包的扩展名列进依赖。原来 GLOB 整个目录，改一个
# .bak / .psd / 说明文件也会重新打包并重链接插件；白名单与 tools/skin_pack.cpp
# 的 kAllowedExt 必须保持一致。
file(GLOB_RECURSE SKIN_FILES CONFIGURE_DEPENDS
    "${SKIN_DIR}/*.json"
    "${SKIN_DIR}/*.png"
    "${SKIN_DIR}/*.jpg"
    "${SKIN_DIR}/*.jpeg"
    "${SKIN_DIR}/*.glsl"
)

add_custom_command(
    OUTPUT "${SKIN_GEN_DIR}/SkinBinaryData.cpp" "${SKIN_GEN_DIR}/SkinBinaryData.h"
    COMMAND skin_pack "${SKIN_DIR}" "${SKIN_GEN_DIR}"
    DEPENDS skin_pack ${SKIN_FILES}
    COMMENT "打包皮肤 ${SKIN_NAME}"
    VERBATIM
)

set_source_files_properties("${SKIN_GEN_DIR}/SkinBinaryData.cpp" PROPERTIES GENERATED TRUE)

target_sources(OnlyObsessionRemained PRIVATE "${SKIN_GEN_DIR}/SkinBinaryData.cpp")
target_include_directories(OnlyObsessionRemained PRIVATE "${SKIN_GEN_DIR}")

target_compile_definitions(OnlyObsessionRemained PUBLIC
    JUCE_WEB_BROWSER=0
    JUCE_USE_CURL=0
    JUCE_DISPLAY_SPLASH_SCREEN=0
    JUCE_STRICT_REFCOUNTEDPOINTER=1
    # 新插件，无 VST2 历史版本，关闭 VST3→VST2 参数 ID 兼容护栏
    JUCE_VST3_CAN_REPLACE_VST2=0
)

target_link_libraries(OnlyObsessionRemained
    PRIVATE
        juce::juce_audio_utils
        juce::juce_dsp
        # 动态着色器背景。GPU 不可用时自动走 CPU 绘制通路。
        juce::juce_opengl
    PUBLIC
        juce::juce_recommended_config_flags
        juce::juce_recommended_warning_flags
)

# 离线验证 harness：只编译纯 C++ 的 DSP 核心，不依赖 JUCE。
# 好处是算法迭代不必等插件框架，也能在无宿主环境下量化验证 RT60 / 回声密度。
add_executable(plate_offline_test
    offline/PlateOfflineTest.cpp
    Source/dsp/tanks/PlateTank.cpp
    Source/dsp/tanks/RoomTank.cpp
    Source/dsp/tanks/SpringTank.cpp
)

target_include_directories(plate_offline_test PRIVATE Source)
target_compile_features(plate_offline_test PRIVATE cxx_std_17)

enable_testing()
add_test(NAME plate_offline COMMAND plate_offline_test)
set_tests_properties(plate_offline PROPERTIES TIMEOUT 600)
