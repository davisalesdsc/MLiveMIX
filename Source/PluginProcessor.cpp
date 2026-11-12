#include "PluginProcessor.h"
#include "PluginEditor.h"

#ifndef MLIVEMIX_RELAY_URL
 #define MLIVEMIX_RELAY_URL ""
#endif

MLiveMixAudioProcessor::MLiveMixAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput  ("Input",  juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true))
{
    server.setRemote (&remote);
}

MLiveMixAudioProcessor::~MLiveMixAudioProcessor()
{
    server.setRemote (nullptr);
    server.stop();
    remote.stop();
}

juce::String MLiveMixAudioProcessor::getRelayBase()
{
    // Built-in server (set when the plugin is compiled), overridable for testing
    const auto env = juce::SystemStats::getEnvironmentVariable ("MLIVEMIX_RELAY_URL", {});
    return env.isNotEmpty() ? env : juce::String (MLIVEMIX_RELAY_URL);
}

void MLiveMixAudioProcessor::ensureRemoteIdentity()
{
    if (remoteRoom.isEmpty() || remoteKey.isEmpty())
    {
        remoteRoom = mlm::RemoteLink::makeRandomId (16); // 128-bit, unguessable link
        remoteKey  = mlm::RemoteLink::makeRandomId (24); // secret that lets only this plugin transmit
    }
    remote.configure (getRelayBase(), remoteRoom, remoteKey);
}

void MLiveMixAudioProcessor::startRemote()
{
    ensureRemoteIdentity();
    ensureServerRunning(); // the sender thread feeds the remote link
    remote.start();
}

void MLiveMixAudioProcessor::newRemoteLink()
{
    const bool wasOn = remote.isOn();
    remote.stop();
    remoteRoom.clear();
    remoteKey.clear();
    ensureRemoteIdentity();
    if (wasOn)
        remote.start();
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
    state.setProperty ("room", remoteRoom, nullptr);
    state.setProperty ("roomKey", remoteKey, nullptr);
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
            remoteRoom = state.getProperty ("room", "").toString();
            remoteKey = state.getProperty ("roomKey", "").toString();
            ensureRemoteIdentity();
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
