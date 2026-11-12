#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "net/AudioTap.h"
#include "net/StreamServer.h"

/**
    MLiveMIX: put it on the master bus. The audio passes through untouched (the DAW keeps
    playing on your speakers/headphones as usual); a copy is streamed to phones and browsers
    on the local network.
*/
class MLiveMixAudioProcessor : public juce::AudioProcessor
{
public:
    MLiveMixAudioProcessor();
    ~MLiveMixAudioProcessor() override;

    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override {}
    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;
    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;
    using AudioProcessor::processBlock;

    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override { return true; }

    const juce::String getName() const override { return JucePlugin_Name; }
    bool acceptsMidi() const override { return false; }
    bool producesMidi() const override { return false; }
    bool isMidiEffect() const override { return false; }
    double getTailLengthSeconds() const override { return 0.0; }

    int getNumPrograms() override { return 1; }
    int getCurrentProgram() override { return 0; }
    void setCurrentProgram (int) override {}
    const juce::String getProgramName (int) override { return {}; }
    void changeProgramName (int, const juce::String&) override {}

    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    //==============================================================================
    mlm::AudioTap& getTap() noexcept           { return tap; }
    mlm::StreamServer& getServer() noexcept    { return server; }

    /** Starts the server if it is not running (also used by the "try again" button). */
    bool ensureServerRunning();

    void setStreaming (bool on)                { tap.setEnabled (on); }
    bool isStreaming() const noexcept          { return tap.isEnabled(); }

    /** Which LAN address the QR code shows (remembered with the session). */
    juce::String preferredAddress;

    std::atomic<float> peakLeft { 0.0f }, peakRight { 0.0f };
    std::atomic<bool> audioIsRunning { false };

private:
    mlm::AudioTap tap;
    mlm::StreamServer server { tap };
    std::atomic<juce::uint32> lastProcessMs { 0 };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MLiveMixAudioProcessor)
};
