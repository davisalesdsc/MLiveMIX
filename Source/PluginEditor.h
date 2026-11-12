#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include "PluginProcessor.h"
#include "ui/QrCodeComponent.h"

class MLiveMixAudioProcessorEditor : public juce::AudioProcessorEditor,
                                     private juce::Timer
{
public:
    explicit MLiveMixAudioProcessorEditor (MLiveMixAudioProcessor&);
    ~MLiveMixAudioProcessorEditor() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    /** Shows the REMOTO (internet) tab instead of the local one. */
    void setRemoteMode (bool shouldShowRemote) { remoteMode = shouldShowRemote; updateStatus(); resized(); repaint(); }

    /** Address shown in the QR code ("http://192.168.0.10:8800/"). Exposed for tests. */
    juce::String getListenUrl() const;

private:
    void timerCallback() override;
    void refreshAddresses();
    void updateStatus();

    class LookAndFeel : public juce::LookAndFeel_V4
    {
    public:
        LookAndFeel();
        void drawButtonBackground (juce::Graphics&, juce::Button&, const juce::Colour&, bool, bool) override;
        juce::Font getTextButtonFont (juce::TextButton&, int) override;
    };

    MLiveMixAudioProcessor& processor;
    LookAndFeel lnf;

    mlm::QrCodeComponent qr;
    juce::Label urlLabel, statusLabel;
    juce::TextButton openHereButton, copyButton, switchNetworkButton, retryButton, streamButton;
    juce::TextButton localTab, remoteTab, remoteStartButton, newLinkButton;
    bool remoteMode = false;
    bool confirmNewLink = false;

    /** The link shown/copied in the current tab. */
    juce::String currentUrl();

    juce::StringArray addresses;
    int addressIndex = 0;
    float meterL = 0.0f, meterR = 0.0f;
    int slowCounter = 0;
    juce::Colour statusColour;

    juce::Rectangle<int> qrCard, meterArea, stepsArea, statusDot;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (MLiveMixAudioProcessorEditor)
};
