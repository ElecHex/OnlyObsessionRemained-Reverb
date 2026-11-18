#include "PluginProcessor.h"

#include "PluginEditor.h"
#include "params/ParameterIDs.h"
#include "params/ParameterSchema.h"

OnlyObsessionRemainedProcessor::OnlyObsessionRemainedProcessor()
    : juce::AudioProcessor (BusesProperties()
                                .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                                .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout()),
      presetManager (apvts)
{
    cacheParameterPointers();
}

OnlyObsessionRemainedProcessor::~OnlyObsessionRemainedProcessor() = default;

juce::AudioProcessorValueTreeState::ParameterLayout OnlyObsessionRemainedProcessor::createParameterLayout()
{
    using namespace juce;

    AudioProcessorValueTreeState::ParameterLayout layout;

    StringArray typeItems;

    for (int i = 0; i < params::kNumTypeChoices; ++i)
        typeItems.add (params::kTypeChoices[i]);

    layout.add (std::make_unique<AudioParameterChoice> (
        ParameterID { params::type, params::versionHint }, "Type", typeItems, 0));

    // 范围 / 默认值 / 单位 / 显示名都来自 ParameterSchema.h，这里不再重复一份。
    for (const auto& spec : params::kFloatSpecs)
    {
        layout.add (std::make_unique<AudioParameterFloat> (
            ParameterID { spec.id, params::versionHint },
            spec.displayName,
            NormalisableRange<float> (spec.minValue, spec.maxValue, spec.step),
            spec.defaultValue,
            AudioParameterFloatAttributes().withLabel (spec.unit)));
    }

    return layout;
}

void OnlyObsessionRemainedProcessor::cacheParameterPointers()
{
    typeParam     = apvts.getRawParameterValue (params::type);
    mixParam      = apvts.getRawParameterValue (params::mix);
    decayParam    = apvts.getRawParameterValue (params::decay);
    preDelayParam = apvts.getRawParameterValue (params::preDelay);
    modParam      = apvts.getRawParameterValue (params::mod);
    highDampParam = apvts.getRawParameterValue (params::highDamp);
    lowDampParam  = apvts.getRawParameterValue (params::lowDamp);
    shimmerParam  = apvts.getRawParameterValue (params::shimmer);
}

ReverbEngine::Parameters OnlyObsessionRemainedProcessor::readEngineParameters() const
{
    ReverbEngine::Parameters p;

    p.type       = static_cast<ReverbEngine::ReverbType> (juce::roundToInt (typeParam->load()));
    p.mix        = params::engineValue (params::idxMix,      mixParam->load());
    p.decay      = params::engineValue (params::idxDecay,    decayParam->load());
    p.preDelayMs = params::engineValue (params::idxPreDelay, preDelayParam->load());
    p.mod        = params::engineValue (params::idxMod,      modParam->load());
    p.highDamp   = params::engineValue (params::idxHighDamp, highDampParam->load());
    p.lowDamp    = params::engineValue (params::idxLowDamp,  lowDampParam->load());
    p.shimmer    = params::engineValue (params::idxShimmer,  shimmerParam->load());

    return p;
}

void OnlyObsessionRemainedProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    engine.prepare (sampleRate, samplesPerBlock, getTotalNumOutputChannels());
}

void OnlyObsessionRemainedProcessor::releaseResources()
{
    engine.reset();
}

bool OnlyObsessionRemainedProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& mainOut = layouts.getMainOutputChannelSet();
    const auto& mainIn  = layouts.getMainInputChannelSet();

    if (mainOut != mainIn)
        return false;

    return mainOut == juce::AudioChannelSet::mono()
        || mainOut == juce::AudioChannelSet::stereo();
}

void OnlyObsessionRemainedProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer& midiMessages)
{
    juce::ignoreUnused (midiMessages);

    const auto startTicks = juce::Time::getHighResolutionTicks();
    const auto numSamples = buffer.getNumSamples();

    // 清空未参与渲染的输出声道
    for (int ch = getTotalNumInputChannels(); ch < getTotalNumOutputChannels(); ++ch)
        buffer.clear (ch, 0, buffer.getNumSamples());

    engine.setParameters (readEngineParameters());
    engine.process (buffer);

    const auto sampleRate = getSampleRate();

    if (sampleRate > 0.0 && numSamples > 0)
    {
        const auto elapsed = juce::Time::highResolutionTicksToSeconds (
            juce::Time::getHighResolutionTicks() - startTicks);
        const auto budget = static_cast<double> (numSamples) / sampleRate;
        const auto instant = static_cast<float> (elapsed / budget * 100.0);
        const auto previous = dspLoadPercent.load (std::memory_order_relaxed);

        dspLoadPercent.store (previous * 0.85f + instant * 0.15f, std::memory_order_relaxed);
    }
}

juce::AudioProcessorEditor* OnlyObsessionRemainedProcessor::createEditor()
{
    return new OnlyObsessionRemainedEditor (*this);
}

bool OnlyObsessionRemainedProcessor::hasEditor() const { return true; }

const juce::String OnlyObsessionRemainedProcessor::getName() const { return JucePlugin_Name; }

bool OnlyObsessionRemainedProcessor::acceptsMidi() const  { return false; }
bool OnlyObsessionRemainedProcessor::producesMidi() const { return false; }
bool OnlyObsessionRemainedProcessor::isMidiEffect() const { return false; }

double OnlyObsessionRemainedProcessor::getTailLengthSeconds() const
{
    // 长衰减尾巴需要给宿主报足够长的 tail，避免尾音被截断
    return 30.0;
}

int OnlyObsessionRemainedProcessor::getNumPrograms()
{
    return presetManager.getNumPresets();
}

int OnlyObsessionRemainedProcessor::getCurrentProgram()
{
    return presetManager.getCurrentIndex();
}

void OnlyObsessionRemainedProcessor::setCurrentProgram (int index)
{
    if (juce::isPositiveAndBelow (index, presetManager.getNumPresets())
        && index != presetManager.getCurrentIndex())
        presetManager.loadPreset (index);
}

const juce::String OnlyObsessionRemainedProcessor::getProgramName (int index)
{
    return presetManager.getPresetName (index);
}

void OnlyObsessionRemainedProcessor::changeProgramName (int /*index*/, const juce::String& /*newName*/)
{
    // 出厂预设名称不可改；用户预设的名称即文件名，改名请用文件系统或另存
}

void OnlyObsessionRemainedProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    if (auto xml = apvts.copyState().createXml())
        copyXmlToBinary (*xml, destData);
}

void OnlyObsessionRemainedProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
        if (xml->hasTagName (apvts.state.getType()))
            apvts.replaceState (juce::ValueTree::fromXml (*xml));
}

// JUCE 插件入口
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new OnlyObsessionRemainedProcessor();
}