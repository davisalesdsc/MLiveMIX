// Loads an INSTALLED plugin exactly like a DAW does and exercises it.
//
//   MLiveMixPluginLoadTest <path-to-.vst3>
//   MLiveMixPluginLoadTest AU            (macOS: the Audio Unit aufx/Mlv1/Mlvm)
//
// Prints "LOAD OK" or "LOAD FAILED: <reason from the OS / format>" and exits 0 / 1.

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_audio_utils/juce_audio_utils.h>
#include <iostream>

namespace
{
int fail (const juce::String& why)
{
    std::cout << "LOAD FAILED: " << why << std::endl;
    return 1;
}
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;

    if (argc < 2)
        return fail ("usage: MLiveMixPluginLoadTest <plugin.vst3 | AU>");

    const juce::String target (argv[1]);
    juce::AudioPluginFormatManager formats;
    juce::String identifier;

    if (target == "AU")
    {
       #if JUCE_PLUGINHOST_AU && JUCE_MAC
        formats.addFormat (std::make_unique<juce::AudioUnitPluginFormat>());
        identifier = "AudioUnit:Effects/aufx,Mlv1,Mlvm";
       #else
        return fail ("AU hosting is only available on macOS");
       #endif
    }
    else
    {
        formats.addFormat (std::make_unique<juce::VST3PluginFormat>());
        identifier = juce::File (target).getFullPathName();
        if (! juce::File (identifier).exists())
            return fail ("not found: " + identifier);
    }

    auto* format = formats.getFormat (0);
    juce::OwnedArray<juce::PluginDescription> found;
    format->findAllTypesForFile (found, identifier);
    if (found.isEmpty())
        return fail ("the host found no plugin in " + identifier + " (the binary did not load or exposes no plugin)");

    const auto& desc = *found[0];
    std::cout << "found: " << desc.name << " by " << desc.manufacturerName << " (" << desc.pluginFormatName
              << ", version " << desc.version << ", " << desc.numInputChannels << " in / "
              << desc.numOutputChannels << " out)" << std::endl;

    juce::String error;
    auto instance = formats.createPluginInstance (desc, 48000.0, 512, error);
    if (instance == nullptr)
        return fail ("could not instantiate: " + error);

    std::cout << "instantiated: " << instance->getName() << ", latency " << instance->getLatencySamples() << std::endl;

    instance->setRateAndBufferSizeDetails (48000.0, 512);
    instance->prepareToPlay (48000.0, 512);

    // Process 2 seconds of noise; the plugin must pass the audio through untouched
    juce::Random rng (1);
    juce::AudioBuffer<float> buffer (juce::jmax (2, instance->getTotalNumOutputChannels()), 512), copy;
    juce::MidiBuffer midi;
    bool identical = true;
    for (int b = 0; b < 188; ++b)
    {
        for (int c = 0; c < buffer.getNumChannels(); ++c)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                buffer.setSample (c, i, rng.nextFloat() * 0.5f - 0.25f);
        copy.makeCopyOf (buffer);
        instance->processBlock (buffer, midi);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < buffer.getNumSamples(); ++i)
                identical = identical && std::abs (buffer.getSample (c, i) - copy.getSample (c, i)) < 1.0e-7f;
    }
    std::cout << "processed 2 s of audio, pass-through " << (identical ? "identical" : "CHANGED") << std::endl;

    // The plugin's own server must answer like it does for a phone
    {
        juce::StreamingSocket s;
        juce::String reply;
        for (int port = 8800; port < 8820 && reply.isEmpty(); ++port)
        {
            if (! s.connect ("127.0.0.1", port, 500))
                continue;
            const juce::String req = "GET /status HTTP/1.1\r\nHost: localhost\r\n\r\n";
            s.write (req.toRawUTF8(), (int) req.getNumBytesAsUTF8());
            char buf[2048] = {};
            if (s.waitUntilReady (true, 2000) == 1)
                reply = juce::String (buf, (size_t) juce::jmax (0, s.read (buf, sizeof (buf) - 1, false)));
            s.close();
        }
        std::cout << "server: " << (reply.contains ("MLiveMIX") ? "answering" : "NOT answering") << std::endl;
        if (! reply.contains ("MLiveMIX"))
            return fail ("the plugin's server did not answer on ports 8800-8819");
    }

    // Open and close the plugin window
    if (instance->hasEditor())
    {
        std::unique_ptr<juce::AudioProcessorEditor> editor (instance->createEditorAndMakeActive());
        if (editor == nullptr)
            return fail ("the plugin window could not be created");
        std::cout << "editor: " << editor->getWidth() << " x " << editor->getHeight() << std::endl;
        juce::Thread::sleep (300);
        editor.reset(); // the editor's destructor tells the plugin it is gone
    }

    juce::MemoryBlock state;
    instance->getStateInformation (state);
    instance->setStateInformation (state.getData(), (int) state.getSize());

    instance->releaseResources();
    instance.reset();

    if (! identical)
        return fail ("the plugin changed the audio");

    std::cout << "LOAD OK" << std::endl;
    return 0;
}
