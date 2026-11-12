#include "PluginProcessor.h"
#include "PluginEditor.h"

MLiveMixAudioProcessor::MLiveMixAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
}

MLiveMixAudioProcessor::~MLiveMixAudioProcessor()
{
    server.stop();
}

bool MLiveMixAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto& in  = layouts.getMainInputChannelSet();
    const auto& out = layouts.getMainOutputChannelSet();
    return in == out && (out == juce::AudioChannelSet::stereo() || out == juce::AudioChannelSet::mono());
}

void MLiveMixAudioProcessor::prepareToPlay (double sampleRate, int)
{
    tap.setSampleRate (sampleRate);
    ensureServerRunning();
}

bool MLiveMixAudioProcessor::ensureServerRunning()
{
    return server.isRunning() || server.start (8800, 20);
}

void MLiveMixAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    // The audio is NOT modified: this plugin only listens.
    const int n = buffer.getNumSamples();
    const int ch = buffer.getNumChannels();
    if (n <= 0 || ch <= 0)
        return;

    const float* left  = buffer.getReadPointer (0);
    const float* right = ch > 1 ? buffer.getReadPointer (1) : left;
    tap.push (left, right, n);

    // Block peaks for the editor's meter (the editor applies the fall-off)
    const float pl = buffer.getMagnitude (0, 0, n);
    peakLeft.store (pl);
    peakRight.store (ch > 1 ? buffer.getMagnitude (1, 0, n) : pl);
    audioIsRunning.store (true);
}

//==============================================================================
void MLiveMixAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    juce::ValueTree state ("MLiveMIX");
    state.setProperty ("streaming", isStreaming(), nullptr);
    state.setProperty ("address", preferredAddress, nullptr);
    if (auto xml = state.createXml())
        copyXmlToBinary (*xml, destData);
}

void MLiveMixAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    if (auto xml = getXmlFromBinary (data, sizeInBytes))
    {
        const auto state = juce::ValueTree::fromXml (*xml);
        if (state.hasType ("MLiveMIX"))
        {
            setStreaming ((bool) state.getProperty ("streaming", true));
            preferredAddress = state.getProperty ("address", "").toString();
        }
    }
}

juce::AudioProcessorEditor* MLiveMixAudioProcessor::createEditor()
{
    return new MLiveMixAudioProcessorEditor (*this);
}

juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new MLiveMixAudioProcessor();
}
