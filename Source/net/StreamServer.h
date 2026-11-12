#pragma once

#include <juce_core/juce_core.h>
#include "AudioTap.h"
#include "RemoteLink.h"
#include <atomic>
#include <deque>

namespace mlm
{
/**
    Embedded HTTP + WebSocket server.

      GET /        -> the listening page (embedded HTML)
      GET /status  -> JSON status
      GET /ws      -> WebSocket: one "hello" text message, then binary audio packets
                      (see ws::makeAudioPacket), ~20 ms each

    Threads: one listener (accept), one sender (reads the AudioTap and fans packets out),
    one per client (handshake, its own bounded queue, ping/close). Nothing here ever touches
    the audio thread except through the lock-free AudioTap.
*/
class StreamServer
{
public:
    explicit StreamServer (AudioTap& tap);
    ~StreamServer();

    /** Starts listening on the first free port in [firstPort, firstPort + attempts). */
    bool start (int firstPort = 8800, int attempts = 20);
    void stop();

    bool isRunning() const noexcept       { return running.load(); }
    int getPort() const noexcept          { return port.load(); }
    int getNumListeners() const noexcept  { return listeners.load(); }
    juce::uint64 getPacketsSent() const noexcept { return packetsSent.load(); }
    juce::String getLastError() const;

    /** Also feed the audio to a remote link (internet listening). nullptr to detach. */
    void setRemote (RemoteLink* r) noexcept { remote.store (r); }

    /** Page served at "/" (defaults to the embedded web page). */
    void setPage (const juce::String& html);

private:
    class Client;
    class ListenerThread;
    class SenderThread;

    void addClient (std::unique_ptr<juce::StreamingSocket> socket);
    void broadcast (const juce::MemoryBlock& frame);
    void removeFinishedClients();
    juce::String statusJson() const;
    juce::String getPageCopy() const { const juce::ScopedLock sl (pageLock); return page; }

    AudioTap& tap;
    std::unique_ptr<juce::StreamingSocket> listenSocket;
    std::unique_ptr<ListenerThread> listenerThread;
    std::unique_ptr<SenderThread> senderThread;

    juce::CriticalSection clientLock;
    juce::OwnedArray<Client> clients;

    juce::String page;
    mutable juce::CriticalSection pageLock;

    std::atomic<bool> running { false };
    std::atomic<int> port { 0 };
    std::atomic<int> listeners { 0 };
    std::atomic<juce::uint64> packetsSent { 0 };
    std::atomic<RemoteLink*> remote { nullptr };

    mutable juce::CriticalSection errorLock;
    juce::String lastError;

    JUCE_DECLARE_NON_COPYABLE (StreamServer)
};
}
