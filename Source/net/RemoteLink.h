#pragma once

#include <juce_core/juce_core.h>
#include "WsClient.h"
#include <atomic>
#include <vector>

namespace mlm
{
/**
    Remote listening: the plugin connects OUT to the relay (so it works behind any router)
    and sends the mix there; anyone with the link listens through the relay.

    Audio arrives from the stream's sender thread (pushAudio, never from the audio thread),
    is grouped into 40 ms packets and is only sent while someone is listening remotely.
    The connection is retried automatically while the link is switched on.
*/
class RemoteLink : private juce::Thread
{
public:
    enum class State { off, connecting, live, retrying, unavailable };

    RemoteLink();
    ~RemoteLink() override;

    /** relayBase: "https://mlivemix-relay.example.workers.dev" (or http://127.0.0.1:8787 in tests). */
    void configure (const juce::String& relayBase, const juce::String& roomId, const juce::String& hostKey);

    void start();
    void stop();
    bool isOn() const noexcept { return on.load(); }

    State getState() const noexcept           { return state.load(); }
    int getListeners() const noexcept         { return remoteListeners.load(); }
    juce::String getLastError() const;
    juce::String getShareUrl() const;          // https://.../r/<room>
    juce::uint64 getPacketsSent() const noexcept { return packetsSent.load(); }

    /** True while remote listeners want audio (so the sender should keep producing it). */
    bool wantsAudio() const noexcept { return on.load() && state.load() == State::live && remoteListeners.load() > 0; }

    /** Interleaved stereo float frames, any amount (called by the sender thread). */
    void pushAudio (const float* interleaved, int numFrames, int sampleRate);

    /** Test hook: use our own ws:// client instead of the platform one. */
    void forcePlainClient (bool shouldForce) noexcept { forcePlain = shouldForce; }

    static juce::String makeRandomId (int numBytes);

private:
    void run() override;
    bool session();   // one connection, returns when it ends
    void setError (const juce::String& e);

    juce::String relayBase, room, key;
    mutable juce::CriticalSection configLock, errorLock;
    juce::String lastError;

    juce::CriticalSection audioLock;
    std::vector<float> pending;
    std::atomic<int> sampleRate { 48000 };

    std::atomic<bool> on { false }, forcePlain { false };
    std::atomic<State> state { State::off };
    std::atomic<int> remoteListeners { 0 };
    std::atomic<juce::uint64> packetsSent { 0 };
    juce::WaitableEvent wake;
};
}
