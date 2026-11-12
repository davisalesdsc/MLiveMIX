#pragma once

#include <juce_core/juce_core.h>
#include <memory>

namespace mlm
{
/**
    Outgoing WebSocket connection (plugin -> remote relay).

    Implementations:
      - macOS:   NSURLSessionWebSocketTask (wss:// and ws://), macOS 10.15+
      - Windows: WinHTTP WebSocket API (wss:// and ws://)
      - others / ws:// in tests: a small client of our own on top of juce::StreamingSocket (no TLS)

    All calls come from ONE thread (the RemoteLink thread).
*/
class WsClient
{
public:
    virtual ~WsClient() = default;

    /** Blocks until connected (HTTP 101) or failed. */
    virtual bool connect (const juce::String& url, int timeoutMs, juce::String& error) = 0;

    /** Queues/sends one binary message. Returns false if the connection is gone. */
    virtual bool sendBinary (const void* data, size_t size) = 0;
    virtual bool sendText (const juce::String& text) = 0;

    /** Collects text messages received since the last call. Returns false if the connection is gone. */
    virtual bool poll (juce::StringArray& texts) = 0;

    virtual void close() = 0;

    /** Picks the implementation for this platform and URL scheme. Returns nullptr if unsupported. */
    static std::unique_ptr<WsClient> create (const juce::String& url, juce::String& whyNot);
};

/** Our own minimal client (ws:// only, no TLS). Also used by the tests. */
std::unique_ptr<WsClient> createPlainWsClient();

#if JUCE_MAC
std::unique_ptr<WsClient> createAppleWsClient();
#elif JUCE_WINDOWS
std::unique_ptr<WsClient> createWinHttpWsClient();
#endif
}
