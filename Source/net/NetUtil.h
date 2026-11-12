#pragma once

#include <juce_core/juce_core.h>

namespace mlm::net
{
/** Private (LAN) IPv4 addresses of this computer, best candidate first (192.168.x, 10.x, 172.16-31.x). */
juce::StringArray getLanAddresses();

/** Makes sure writing to a socket whose peer has gone away can never raise SIGPIPE
    (which would kill the whole host application). Call once per accepted socket. */
void preventSigPipe (juce::StreamingSocket& socket);

/** Sends all bytes (handles partial writes, never raises SIGPIPE). Returns false on error/closed. */
bool sendAll (juce::StreamingSocket& socket, const void* data, size_t size);
}
