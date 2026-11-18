#include "presets/PresetManager.h"

#include "params/ParameterIDs.h"
#include "presets/FactoryPresets.h"
#include "ui/UiText.h"

#include <algorithm>

namespace
{
    using FactoryPreset = factory_presets::Preset;

    const FactoryPreset* const kFactoryPresets = factory_presets::kPresets;
    constexpr int kNumFactoryPresets = factory_presets::kCount;

    /** 按参数 ID 写入一个归一化值。 */
    void setParam (juce::AudioProcessorValueTreeState& apvts, const char* id, float valueInUiUnits)
    {
        if (auto* param = apvts.getParameter (id))
            param->setValueNotifyingHost (param->convertTo0to1 (valueInUiUnits));
    }

    juce::String withExtension (const juce::File& f) { return f.getFileNameWithoutExtension(); }
}

PresetManager::PresetManager (juce::AudioProcessorValueTreeState& stateToUse)
    : apvts (stateToUse)
{
    refreshUserPresets();
}

void PresetManager::refreshUserPresets()
{
    userPresetFiles.clear();

    const auto dir = getUserPresetDirectory();

    if (dir.isDirectory())
        for (const auto& f : dir.findChildFiles (juce::File::findFiles, false, "*.xml"))
            userPresetFiles.add (f);

    std::sort (userPresetFiles.begin(), userPresetFiles.end(),
               [] (const juce::File& a, const juce::File& b)
               {
                   return a.getFileNameWithoutExtension()
                            .compareIgnoreCase (b.getFileNameWithoutExtension()) < 0;
               });
}

int PresetManager::getNumFactoryPresets() noexcept
{
    return kNumFactoryPresets;
}

int PresetManager::getNumPresets() const noexcept
{
    return kNumFactoryPresets + userPresetFiles.size();
}

bool PresetManager::isUserPreset (int index) const noexcept
{
    return index >= kNumFactoryPresets && index < getNumPresets();
}

juce::String PresetManager::getPresetName (int index) const
{
    if (index < kNumFactoryPresets)
        return index >= 0 ? juce::String (kFactoryPresets[index].name) : juce::String();

    const int userIndex = index - kNumFactoryPresets;

    if (userIndex >= 0 && userIndex < userPresetFiles.size())
        return withExtension (userPresetFiles[userIndex]);

    return {};
}

void PresetManager::setCurrentIndex (int index) noexcept
{
    if (juce::isPositiveAndBelow (index, getNumPresets()))
        currentIndex = index;
}

void PresetManager::loadPreset (int index)
{
    if (! juce::isPositiveAndBelow (index, getNumPresets()))
        return;

    if (index < kNumFactoryPresets)
    {
        const auto& p = kFactoryPresets[index];

        setParam (apvts, params::type,      static_cast<float> (p.type));
        setParam (apvts, params::mix,       p.mix);
        setParam (apvts, params::decay,     p.decay);
        setParam (apvts, params::preDelay,  p.preDelayMs);
        setParam (apvts, params::mod,       p.mod);
        setParam (apvts, params::highDamp,  p.highDamp);
        setParam (apvts, params::lowDamp,   p.lowDamp);
        setParam (apvts, params::shimmer,   p.shimmer);
    }
    else
    {
        const auto file = userPresetFiles[index - kNumFactoryPresets];

        if (auto xml = juce::XmlDocument::parse (file))
            if (xml->hasTagName (apvts.state.getType()))
                apvts.replaceState (juce::ValueTree::fromXml (*xml));
    }

    currentIndex = index;

    sendChangeMessage();
}

bool PresetManager::saveUserPreset (const juce::String& rawName, juce::String& errorOut)
{
    const auto name = rawName.trim();

    if (name.isEmpty())
    {
        errorOut = U8 ("预设名不能为空。");
        return false;
    }

    const auto dir = getUserPresetDirectory();

    if (! dir.isDirectory() && ! dir.createDirectory())
    {
        errorOut = U8 ("无法创建预设目录：") + dir.getFullPathName();
        return false;
    }

    auto xml = apvts.copyState().createXml();

    if (xml == nullptr)
    {
        errorOut = U8 ("无法序列化当前参数。");
        return false;
    }

    const auto file = dir.getChildFile (juce::File::createLegalFileName (name) + ".xml");

    if (! file.replaceWithText (xml->toString()))
    {
        errorOut = U8 ("写入失败：") + file.getFullPathName();
        return false;
    }

    refreshUserPresets();
    sendChangeMessage();
    return true;
}

bool PresetManager::deleteUserPreset (int index, juce::String& errorOut)
{
    if (! isUserPreset (index))
    {
        errorOut = U8 ("该预设不是用户预设，无法删除。");
        return false;
    }

    const auto file = userPresetFiles[index - kNumFactoryPresets];

    if (! file.deleteFile())
    {
        errorOut = U8 ("删除失败：") + file.getFullPathName();
        return false;
    }

    refreshUserPresets();

    if (currentIndex >= getNumPresets())
        currentIndex = juce::jmax (0, getNumPresets() - 1);

    sendChangeMessage();
    return true;
}

juce::File PresetManager::getUserPresetDirectory()
{
    return juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
             .getChildFile ("OnlyObsessionRemained")
             .getChildFile ("Presets");
}