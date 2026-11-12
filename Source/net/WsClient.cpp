#include "WsClient.h"
#include "WebSocket.h"
#include "NetUtil.h"
#include <random>

namespace mlm
{
namespace
{
    /** Client frames must be masked (RFC 6455 5.3). */
    juce::MemoryBlock makeMaskedFrame (ws::Opcode opcode, const void* payload, size_t size, std::mt19937& rng)
    {
        juce::uint8 header[14];
        size_t h = 2;
        header[0] = (juce::uint8) (0x80 | opcode);
        if (size < 126)
        {
            header[1] = (juce::uint8) (0x80 | size);
        }
        else if (size < 65536)
        {
            header[1] = 0x80 | 126;
            header[2] = (juce::uint8) (size >> 8);
            header[3] = (juce::uint8) size;
            h = 4;
        }
        else
        {
            header[1] = 0x80 | 127;
            for (int i = 0; i < 8; ++i)
                header[2 + i] = (juce::uint8) ((juce::uint64) size >> (56 - 8 * i));
            h = 10;
        }
        const juce::uint32 m = rng();
        const juce::uint8 mask[4] = { (juce::uint8) m, (juce::uint8) (m >> 8), (juce::uint8) (m >> 16), (juce::uint8) (m >> 24) };
        std::memcpy (header + h, mask, 4);
        h += 4;

        juce::MemoryBlock frame (h + size);
        auto* out = static_cast<juce::uint8*> (frame.getData());
        std::memcpy (out, header, h);
        const auto* in = static_cast<const juce::uint8*> (payload);
        for (size_t i = 0; i < size; ++i)
            out[h + i] = (juce::uint8) (in[i] ^ mask[i & 3]);
        return frame;
    }

    class PlainWsClient : public WsClient
    {
    public:
        bool connect (const juce::String& url, int timeoutMs, juce::String& error) override
        {
            if (! url.startsWithIgnoreCase ("ws://"))
            {
                error = "Este sistema não suporta conexão segura (wss)";
                return false;
            }

            auto rest = url.substring (5);
            auto hostPort = rest.upToFirstOccurrenceOf ("/", false, false);
            auto path = rest.fromFirstOccurrenceOf ("/", true, false);
            if (path.isEmpty())
                path = "/";
            const auto host = hostPort.upToFirstOccurrenceOf (":", false, false);
            const int port = hostPort.containsChar (':') ? hostPort.fromFirstOccurrenceOf (":", false, false).getIntValue() : 80;

            if (! socket.connect (host, port, timeoutMs))
            {
                error = "Não foi possível conectar a " + hostPort;
                return false;
            }
            net::preventSigPipe (socket);

            juce::uint8 keyBytes[16];
            for (auto& b : keyBytes)
                b = (juce::uint8) rng();
            const auto key = juce::Base64::toBase64 (keyBytes, sizeof (keyBytes));

            const juce::String request = "GET " + path + " HTTP/1.1\r\nHost: " + hostPort
                                       + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + key
                                       + "\r\nSec-WebSocket-Version: 13\r\nUser-Agent: MLiveMIX\r\n\r\n";
            if (! net::sendAll (socket, request.toRawUTF8(), request.getNumBytesAsUTF8()))
            {
                error = "Falha ao enviar o pedido de conexão";
                return false;
            }

            // Read the response header
            const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
            char buf[2048];
            while (juce::Time::getMillisecondCounter() < end)
            {
                const auto text = rx.toString();
                const int pos = text.indexOf ("\r\n\r\n");
                if (pos >= 0)
                {
                    const auto header = text.substring (0, pos);
                    rx.removeSection (0, (size_t) pos + 4);
                    if (! header.startsWith ("HTTP/1.1 101"))
                    {
                        error = "Servidor recusou: " + header.upToFirstOccurrenceOf ("\r\n", false, false).fromFirstOccurrenceOf (" ", false, false);
                        return false;
                    }
                    if (! header.containsIgnoreCase (ws::acceptKey (key)))
                    {
                        error = "Resposta inválida do servidor";
                        return false;
                    }
                    connected = true;
                    return true;
                }
                if (socket.waitUntilReady (true, 100) != 1)
                    continue;
                const int n = socket.read (buf, (int) sizeof (buf), false);
                if (n <= 0)
                    break;
                rx.append (buf, (size_t) n);
            }
            error = "Tempo esgotado ao conectar";
            return false;
        }

        bool sendBinary (const void* data, size_t size) override
        {
            if (! connected) return false;
            const auto f = makeMaskedFrame (ws::binary, data, size, rng);
            connected = net::sendAll (socket, f.getData(), f.getSize());
            return connected;
        }

        bool sendText (const juce::String& text) override
        {
            if (! connected) return false;
            const auto f = makeMaskedFrame (ws::text, text.toRawUTF8(), text.getNumBytesAsUTF8(), rng);
            connected = net::sendAll (socket, f.getData(), f.getSize());
            return connected;
        }

        bool poll (juce::StringArray& texts) override
        {
            if (! connected) return false;
            char buf[4096];
            while (socket.waitUntilReady (true, 0) == 1)
            {
                const int n = socket.read (buf, (int) sizeof (buf), false);
                if (n <= 0) { connected = false; return false; }
                rx.append (buf, (size_t) n);
            }
            for (;;)
            {
                ws::Frame f;
                const int used = ws::parseFrame (static_cast<const juce::uint8*> (rx.getData()), rx.getSize(), f);
                if (used < 0) { connected = false; return false; }
                if (used == 0) break;
                rx.removeSection (0, (size_t) used);
                if (f.opcode == ws::text)
                    texts.add (f.payload.toString());
                else if (f.opcode == ws::ping)
                {
                    const auto pong = makeMaskedFrame (ws::pong, f.payload.getData(), f.payload.getSize(), rng);
                    net::sendAll (socket, pong.getData(), pong.getSize());
                }
                else if (f.opcode == ws::close)
                {
                    connected = false;
                    return false;
                }
            }
            return true;
        }

        void close() override
        {
            if (connected)
            {
                const auto f = makeMaskedFrame (ws::close, nullptr, 0, rng);
                net::sendAll (socket, f.getData(), f.getSize());
            }
            connected = false;
            socket.close();
        }

    private:
        juce::StreamingSocket socket;
        juce::MemoryBlock rx;
        bool connected = false;
        std::mt19937 rng { std::random_device{}() };
    };
}

std::unique_ptr<WsClient> createPlainWsClient()
{
    return std::make_unique<PlainWsClient>();
}

std::unique_ptr<WsClient> WsClient::create (const juce::String& url, juce::String& whyNot)
{
   #if JUCE_MAC
    juce::ignoreUnused (url);
    if (auto c = createAppleWsClient())
        return c;
    whyNot = "A transmissão remota precisa do macOS 10.15 ou mais novo";
    return nullptr;
   #elif JUCE_WINDOWS
    juce::ignoreUnused (url, whyNot);
    return createWinHttpWsClient();
   #else
    if (url.startsWithIgnoreCase ("ws://"))
        return createPlainWsClient();
    whyNot = "A transmissão remota segura (wss) não está disponível neste sistema";
    return nullptr;
   #endif
}
}
