#pragma once

#include <juce_core/juce_core.h>
#include <array>

namespace mlm::ws
{
/** SHA-1 digest (needed only for the WebSocket handshake, RFC 6455). */
std::array<juce::uint8, 20> sha1 (const void* data, size_t size);

/** Sec-WebSocket-Accept value for a client's Sec-WebSocket-Key. */
juce::String acceptKey (const juce::String& clientKey);

enum Opcode : juce::uint8
{
    continuation = 0x0, text = 0x1, binary = 0x2, close = 0x8, ping = 0x9, pong = 0xA
};

/** Builds one unmasked server->client frame (FIN set). */
juce::MemoryBlock makeFrame (Opcode opcode, const void* payload, size_t size);

struct Frame
{
    Opcode opcode = text;
    bool fin = true;
    juce::MemoryBlock payload;
};

/** Parses one frame from the start of 'data'. Returns bytes consumed, 0 if incomplete, -1 if invalid. */
int parseFrame (const juce::uint8* data, size_t size, Frame& out, size_t maxPayload = 1 << 20);

/** Header of an audio packet (binary message), followed by interleaved int16 little-endian samples. */
struct AudioPacketHeader
{
    static constexpr int size = 16;
    // bytes 0..3  "MLV1"
    // bytes 4..7  sample rate (uint32 LE)
    // bytes 8..11 sequence number (uint32 LE)
    // bytes 12..13 channels (uint16 LE)
    // bytes 14..15 flags (uint16 LE, reserved)
};

/** Encodes interleaved float samples into an audio packet payload. */
juce::MemoryBlock makeAudioPacket (const float* interleaved, int numFrames, int numChannels,
                                   int sampleRate, juce::uint32 sequence);
}
