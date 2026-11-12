#include "RemoteLink.h"
#include "WebSocket.h"
#include <random>

namespace mlm
{
namespace
{
    constexpr int packetMs = 40;            // internet: a balance between latency and message count
    constexpr int maxPendingSeconds = 2;    // never keep more than this if the network stalls
}

RemoteLink::RemoteLink() : juce::Thread ("MLiveMIX remote") {}

RemoteLink::~RemoteLink()
{
    stop();
}

juce::String RemoteLink::makeRandomId (int numBytes)
{
    std::random_device rd;
    std::vector<juce::uint8> bytes ((size_t) numBytes);
    for (auto& b : bytes)
        b = (juce::uint8) (rd() & 0xff);
    // base64url without padding: safe in links and paths
    return juce::Base64::toBase64 (bytes.data(), bytes.size())
               .replaceCharacter ('+', '-').replaceCharacter ('/', '_').removeCharacters ("=");
}

void RemoteLink::configure (const juce::String& base, const juce::String& roomId, const juce::String& hostKey)
{
    const juce::ScopedLock sl (configLock);
    relayBase = base.trimCharactersAtEnd ("/");
    room = roomId;
    key = hostKey;
}

juce::String RemoteLink::getShareUrl() const
{
    const juce::ScopedLock sl (configLock);
    if (relayBase.isEmpty() || room.isEmpty())
        return {};
    return relayBase + "/r/" + room;
}

juce::String RemoteLink::getLastError() const
{
    const juce::ScopedLock sl (errorLock);
    return lastError;
}

void RemoteLink::setError (const juce::String& e)
{
    const juce::ScopedLock sl (errorLock);
    lastError = e;
}

void RemoteLink::start()
{
    if (on.exchange (true))
        return;
    setError ({});
    state = State::connecting;
    startThread();
}

void RemoteLink::stop()
{
    if (! on.exchange (false))
        return;
    signalThreadShouldExit();
    wake.signal();
    stopThread (5000);
    state = State::off;
    remoteListeners = 0;
    const juce::ScopedLock sl (audioLock);
    pending.clear();
}

void RemoteLink::pushAudio (const float* interleaved, int numFrames, int sr)
{
    if (! wantsAudio())
        return;
    sampleRate = sr;
    {
        const juce::ScopedLock sl (audioLock);
        pending.insert (pending.end(), interleaved, interleaved + (size_t) numFrames * 2);
        const size_t cap = (size_t) sr * 2 * maxPendingSeconds;
        if (pending.size() > cap)
            pending.erase (pending.begin(), pending.end() - (std::ptrdiff_t) cap);
    }
    wake.signal();
}

void RemoteLink::run()
{
    int backoffMs = 1000;
    while (! threadShouldExit())
    {
        state = State::connecting;
        const bool hadSession = session();
        remoteListeners = 0;
        if (threadShouldExit())
            break;
        if (state.load() == State::unavailable)
        {
            // e.g. platform without TLS support: nothing will change by retrying
            while (! threadShouldExit())
                wake.wait (500);
            break;
        }
        state = State::retrying;
        backoffMs = hadSession ? 1000 : juce::jmin (backoffMs * 2, 15000);
        wake.wait (backoffMs);
    }
}

bool RemoteLink::session()
{
    juce::String base, r, k;
    {
        const juce::ScopedLock sl (configLock);
        base = relayBase; r = room; k = key;
    }
    if (base.isEmpty() || r.isEmpty() || k.isEmpty())
    {
        setError ("Servidor remoto não configurado");
        state = State::unavailable;
        return false;
    }

    const auto wsBase = base.startsWithIgnoreCase ("https://") ? "wss://" + base.substring (8)
                      : base.startsWithIgnoreCase ("http://")  ? "ws://"  + base.substring (7)
                      : base;
    const auto url = wsBase + "/r/" + r + "/host?key=" + k;

    juce::String whyNot;
    std::unique_ptr<WsClient> client = forcePlain.load() ? createPlainWsClient() : WsClient::create (url, whyNot);
    if (client == nullptr)
    {
        setError (whyNot);
        state = State::unavailable;
        return false;
    }

    juce::String error;
    if (! client->connect (url, 8000, error))
    {
        setError (error);
        if (error.contains ("403"))
            setError ("Este link pertence a outro computador. Gere um NOVO LINK.");
        return false;
    }

    setError ({});
    state = State::live;

    auto sendHello = [&]
    {
        const auto hello = juce::String ("{\"type\":\"hello\",\"name\":\"MLiveMIX\",\"version\":\"")
                         + JucePlugin_VersionString + "\",\"sampleRate\":" + juce::String (sampleRate.load()) + "}";
        return client->sendText (hello);
    };
    int helloRate = sampleRate.load();
    if (! sendHello())
        return true;

    juce::uint32 sequence = 0;
    std::vector<float> chunk;

    while (! threadShouldExit())
    {
        wake.wait (50);

        juce::StringArray texts;
        if (! client->poll (texts))
            break;
        for (const auto& t : texts)
        {
            auto parsed = juce::JSON::parse (t);
            if (parsed.getProperty ("type", {}).toString() == "listeners")
                remoteListeners = (int) parsed.getProperty ("count", 0);
        }

        if (sampleRate.load() != helloRate)
        {
            helloRate = sampleRate.load();
            if (! sendHello())
                break;
        }

        const int sr = sampleRate.load();
        const int framesPerPacket = sr * packetMs / 1000;
        bool ok = true;
        for (;;)
        {
            {
                const juce::ScopedLock sl (audioLock);
                if (remoteListeners.load() == 0)
                {
                    pending.clear();
                    break;
                }
                if ((int) (pending.size() / 2) < framesPerPacket)
                    break;
                chunk.assign (pending.begin(), pending.begin() + framesPerPacket * 2);
                pending.erase (pending.begin(), pending.begin() + framesPerPacket * 2);
            }
            const auto packet = ws::makeAudioPacket (chunk.data(), framesPerPacket, 2, sr, sequence++);
            if (! client->sendBinary (packet.getData(), packet.getSize())) { ok = false; break; }
            packetsSent.fetch_add (1);
        }
        if (! ok)
            break;
    }

    client->close();
    if (! threadShouldExit())
        setError ("Conexão com o servidor remoto caiu — reconectando…");
    return true;
}
}
