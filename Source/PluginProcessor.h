#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "dsp/ReverbEngine.h"
#include "presets/PresetManager.h"

class OnlyObsessionRemainedProcessor final : public juce::AudioProcessor
{
public:
    OnlyObsessionRemainedProcessor();
    ~OnlyObsessionRemainedProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages) override;

    /** 湿声输出电平（0…1），供界面上的动态着色器（阶段三 3c）做视觉呼吸。 */
    float getWetOutputLevel() const noexcept { return engine.getWetLevel(); }

    /** 最近一次 processBlock 的耗时占音频预算的比例（0…100），供 HUD 读数用。 */
    float getDspLoadPercent() const noexcept
    {
        return dspLoadPercent.load (std::memory_order_relaxed);
    }

    /**
        当前参数的归一化视图（与引擎实际使用的完全一致）。
        界面着色器取 uniform 用它，避免在界面里重复一遍"百分比 → 0…1"的换算而走样。
    */
    ReverbEngine::Parameters getNormalizedParameters() const { return readEngineParameters(); }

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    juce::AudioProcessorValueTreeState apvts;
    PresetManager                    presetManager;   // 必须声明在 apvts 之后（构造依赖它）

private:
    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    /** 构造期一次性缓存参数指针，避免音频线程做字符串查找。 */
    void cacheParameterPointers();

    ReverbEngine::Parameters readEngineParameters() const;

    ReverbEngine engine;

    /** 音频线程写、界面线程读：只做一次 85/15 的指数平滑，避免读数抖动。 */
    std::atomic<float> dspLoadPercent { 0.0f };

    std::atomic<float>* typeParam     = nullptr;
    std::atomic<float>* mixParam      = nullptr;
    std::atomic<float>* decayParam    = nullptr;
    std::atomic<float>* preDelayParam = nullptr;
    std::atomic<float>* modParam      = nullptr;
    std::atomic<float>* highDampParam = nullptr;
    std::atomic<float>* lowDampParam  = nullptr;
    std::atomic<float>* shimmerParam  = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (OnlyObsessionRemainedProcessor)
};