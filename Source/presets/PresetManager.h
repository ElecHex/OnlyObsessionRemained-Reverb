#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

/**
    预设管理：出厂预设（编译期表格）+ 用户预设（磁盘 XML）。

    用户预设直接以 APVTS 状态 XML 存盘（与插件 state 同格式），因此后续新增参数时
    旧预设天然兼容，不需要维护版本迁移代码。

    载入/保存/删除后都会发出 change 消息，UI 据此刷新列表与选中项。
*/
class PresetManager : public juce::ChangeBroadcaster
{
public:
    explicit PresetManager (juce::AudioProcessorValueTreeState& stateToUse);

    /** 重新扫描用户预设目录并刷新列表（启动时与保存/删除后调用）。 */
    void refreshUserPresets();

    int getNumPresets() const noexcept;
    juce::String getPresetName (int index) const;
    bool isUserPreset (int index) const noexcept;

    static int getNumFactoryPresets() noexcept;

    /** 载入指定预设（出厂或用户），并记住当前索引。 */
    void loadPreset (int index);

    int  getCurrentIndex() const noexcept { return currentIndex; }
    void setCurrentIndex (int index) noexcept;

    /** 以当前参数保存为用户预设；同名会覆盖。失败时通过 errorOut 返回原因。 */
    bool saveUserPreset (const juce::String& name, juce::String& errorOut);

    /** 删除指定索引的用户预设；出厂预设不可删。 */
    bool deleteUserPreset (int index, juce::String& errorOut);

    static juce::File getUserPresetDirectory();

private:
    juce::AudioProcessorValueTreeState& apvts;

    juce::Array<juce::File> userPresetFiles;   // 已按文件名排序
    int currentIndex = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PresetManager)
};