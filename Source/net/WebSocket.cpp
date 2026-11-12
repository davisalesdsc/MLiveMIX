#include "WebSocket.h"

namespace mlm::ws
{
namespace
{
    inline juce::uint32 rol (juce::uint32 v, int s) noexcept { return (v << s) | (v >> (32 - s)); }

    void put16 (juce::uint8* p, juce::uint16 v) noexcept { p[0] = (juce::uint8) v; p[1] = (juce::uint8) (v >> 8); }
    void put32 (juce::uint8* p, juce::uint32 v) noexcept
    {
        for (int i = 0; i < 4; ++i)
            p[i] = (juce::uint8) (v >> (8 * i));
    }
}

std::array<juce::uint8, 20> sha1 (const void* data, size_t size)
{
    juce::uint32 h[5] = { 0x67452301u, 0xEFCDAB89u, 0x98BADCFEu, 0x10325476u, 0xC3D2E1F0u };

    // Message + 0x80 + zero padding + 64-bit big-endian bit length, in 64-byte blocks
    const size_t total = ((size + 8) / 64 + 1) * 64;
    std::vector<juce::uint8> msg (total, 0);
    std::memcpy (msg.data(), data, size);
    msg[size] = 0x80;
    const juce::uint64 bits = (juce::uint64) size * 8;
    for (int i = 0; i < 8; ++i)
        msg[total - 1 - (size_t) i] = (juce::uint8) (bits >> (8 * i));

    for (size_t block = 0; block < total; block += 64)
    {
        juce::uint32 w[80];
        for (int i = 0; i < 16; ++i)
        {
            const auto* p = msg.data() + block + (size_t) i * 4;
            w[i] = (juce::uint32) p[0] << 24 | (juce::uint32) p[1] << 16 | (juce::uint32) p[2] << 8 | p[3];
        }
        for (int i = 16; i < 80; ++i)
            w[i] = rol (w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        juce::uint32 a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i)
        {
            juce::uint32 f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }

            const juce::uint32 t = rol (a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol (b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    std::array<juce::uint8, 20> out {};
    for (int i = 0; i < 5; ++i)
        for (int j = 0; j < 4; ++j)
            out[(size_t) (i * 4 + j)] = (juce::uint8) (h[i] >> (24 - 8 * j));
    return out;
}

juce::String acceptKey (const juce::String& clientKey)
{
    const auto s = clientKey.trim() + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
    const auto utf8 = s.toStdString();
    const auto digest = sha1 (utf8.data(), utf8.size());
    return juce::Base64::toBase64 (digest.data(), digest.size());
}

juce::MemoryBlock makeFrame (Opcode opcode, const void* payload, size_t size)
{
    juce::uint8 header[10];
    size_t headerSize = 2;
    header[0] = (juce::uint8) (0x80 | opcode);

    if (size < 126)
    {
        header[1] = (juce::uint8) size;
    }
    else if (size < 65536)
    {
        header[1] = 126;
        header[2] = (juce::uint8) (size >> 8);
        header[3] = (juce::uint8) size;
        headerSize = 4;
    }
    else
    {
        header[1] = 127;
        for (int i = 0; i < 8; ++i)
            header[2 + i] = (juce::uint8) ((juce::uint64) size >> (56 - 8 * i));
        headerSize = 10;
    }

    juce::MemoryBlock frame (headerSize + size);
    std::memcpy (frame.getData(), header, headerSize);
    if (size > 0)
        std::memcpy (static_cast<juce::uint8*> (frame.getData()) + headerSize, payload, size);
    return frame;
}

int parseFrame (const juce::uint8* data, size_t size, Frame& out, size_t maxPayload)
{
    if (size < 2)
        return 0;

    const bool fin = (data[0] & 0x80) != 0;
    const auto opcode = (Opcode) (data[0] & 0x0F);
    const bool masked = (data[1] & 0x80) != 0;
    juce::uint64 len = data[1] & 0x7F;
    size_t pos = 2;

    if (len == 126)
    {
        if (size < 4) return 0;
        len = (juce::uint64) data[2] << 8 | data[3];
        pos = 4;
    }
    else if (len == 127)
    {
        if (size < 10) return 0;
        len = 0;
        for (int i = 0; i < 8; ++i)
            len = (len << 8) | data[2 + i];
        pos = 10;
    }

    if (len > maxPayload)
        return -1;

    juce::uint8 mask[4] = { 0, 0, 0, 0 };
    if (masked)
    {
        if (size < pos + 4) return 0;
        std::memcpy (mask, data + pos, 4);
        pos += 4;
    }

    if (size < pos + len)
        return 0;

    out.fin = fin;
    out.opcode = opcode;
    out.payload.setSize ((size_t) len);
    auto* p = static_cast<juce::uint8*> (out.payload.getData());
    for (size_t i = 0; i < (size_t) len; ++i)
        p[i] = (juce::uint8) (data[pos + i] ^ (masked ? mask[i & 3] : 0));

    return (int) (pos + len);
}

juce::MemoryBlock makeAudioPacket (const float* interleaved, int numFrames, int numChannels,
                                   int sampleRate, juce::uint32 sequence)
{
    const size_t numSamples = (size_t) numFrames * (size_t) numChannels;
    juce::MemoryBlock packet ((size_t) AudioPacketHeader::size + numSamples * 2);
    auto* p = static_cast<juce::uint8*> (packet.getData());

    p[0] = 'M'; p[1] = 'L'; p[2] = 'V'; p[3] = '1';
    put32 (p + 4, (juce::uint32) sampleRate);
    put32 (p + 8, sequence);
    put16 (p + 12, (juce::uint16) numChannels);
    put16 (p + 14, 0);

    auto* s = p + AudioPacketHeader::size;
    for (size_t i = 0; i < numSamples; ++i)
    {
        const float x = juce::jlimit (-1.0f, 1.0f, interleaved[i]);
        const auto v = (juce::int16) juce::roundToInt (x * 32767.0f);
        put16 (s + i * 2, (juce::uint16) v);
    }
    return packet;
}
}
