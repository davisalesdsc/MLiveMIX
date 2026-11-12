// MLiveMIX tests.
//
//   MLiveMixTests                       -> unit + integration tests (exit code != 0 on failure)
//   MLiveMixTests --serve <sec> <port>  -> serves a test tone (L 440 Hz, R 660 Hz) for a browser test
//   MLiveMixTests --snapshot out.png    -> picture of the plugin window

#include <juce_audio_utils/juce_audio_utils.h>
#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "net/WebSocket.h"
#include "net/StreamServer.h"
#include "net/NetUtil.h"
#include "net/RemoteLink.h"
#include "ui/QrCodeComponent.h"
#include <iostream>

using namespace mlm;

namespace
{
int failures = 0;

void check (bool okay, const juce::String& what)
{
    std::cout << (okay ? "  [ok]   " : "  [FAIL] ") << what << std::endl;
    if (! okay)
        ++failures;
}

juce::String hex (const std::array<juce::uint8, 20>& d)
{
    return juce::String::toHexString (d.data(), (int) d.size(), 0);
}

/** Minimal test client. */
struct TestClient
{
    juce::StreamingSocket socket;
    juce::MemoryBlock rx;

    bool connect (int port) { return socket.connect ("127.0.0.1", port, 2000); }

    bool send (const juce::String& s) { return net::sendAll (socket, s.toRawUTF8(), s.getNumBytesAsUTF8()); }

    /** Reads until the peer closes or the timeout expires. */
    juce::String readAll (int timeoutMs = 3000)
    {
        juce::MemoryBlock data;
        char buf[4096];
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (juce::Time::getMillisecondCounter() < end)
        {
            if (socket.waitUntilReady (true, 100) != 1) continue;
            const int n = socket.read (buf, sizeof (buf), false);
            if (n <= 0) break;
            data.append (buf, (size_t) n);
        }
        return data.toString();
    }

    juce::String readHttpHeader (int timeoutMs = 3000)
    {
        char buf[1024];
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (juce::Time::getMillisecondCounter() < end)
        {
            const auto text = rx.toString();
            const int pos = text.indexOf ("\r\n\r\n");
            if (pos >= 0)
            {
                rx.removeSection (0, (size_t) pos + 4);
                return text.substring (0, pos);
            }
            if (socket.waitUntilReady (true, 100) != 1) continue;
            const int n = socket.read (buf, sizeof (buf), false);
            if (n <= 0) break;
            rx.append (buf, (size_t) n);
        }
        return {};
    }

    bool readFrame (ws::Frame& f, int timeoutMs = 3000)
    {
        char buf[65536];
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) timeoutMs;
        while (juce::Time::getMillisecondCounter() < end)
        {
            const int used = ws::parseFrame (static_cast<const juce::uint8*> (rx.getData()), rx.getSize(), f);
            if (used > 0) { rx.removeSection (0, (size_t) used); return true; }
            if (used < 0) return false;
            if (socket.waitUntilReady (true, 100) != 1) continue;
            const int n = socket.read (buf, sizeof (buf), false);
            if (n <= 0) return false;
            rx.append (buf, (size_t) n);
        }
        return false;
    }

    bool openWebSocket (int port, juce::String& headerOut, const juce::String& path = "/ws",
                        const juce::String& host = "127.0.0.1")
    {
        if (! socket.connect (host, port, 2000)) return false;
        send ("GET " + path + " HTTP/1.1\r\nHost: " + host + ":" + juce::String (port)
              + "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
              "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n\r\n");
        headerOut = readHttpHeader();
        return headerOut.startsWith ("HTTP/1.1 101");
    }
};

juce::String httpGet (int port, const juce::String& path)
{
    TestClient c;
    if (! c.connect (port)) return {};
    c.send ("GET " + path + " HTTP/1.1\r\nHost: localhost\r\n\r\n");
    return c.readAll();
}

/** Pushes 'seconds' of audio into the tap in 10 ms blocks, paced by the real clock (like a DAW's
    audio thread): if a sleep oversleeps, the next iteration catches up, so the amount of audio
    always matches the elapsed time. */
void pushTone (AudioTap& tap, double seconds, double sr, double& phase, bool realTime = true)
{
    const int block = (int) (sr / 100);
    std::vector<float> l ((size_t) block), r ((size_t) block);
    const int blocks = (int) (seconds * 100);
    const double start = juce::Time::getMillisecondCounterHiRes();

    for (int b = 0; b < blocks; ++b)
    {
        if (realTime)
        {
            const double due = start + b * 10.0;
            const double now = juce::Time::getMillisecondCounterHiRes();
            if (due > now)
                juce::Thread::sleep ((int) (due - now));
        }
        for (int i = 0; i < block; ++i)
        {
            l[(size_t) i] = 0.5f * (float) std::sin (phase * 440.0);
            r[(size_t) i] = 0.5f * (float) std::sin (phase * 660.0);
            phase += juce::MathConstants<double>::twoPi / sr;
        }
        tap.push (l.data(), r.data(), block);
    }
}

void unitTests()
{
    std::cout << "Unit tests" << std::endl;

    check (hex (ws::sha1 ("abc", 3)) == "a9993e364706816aba3e25717850c26c9cd0d89d", "SHA-1('abc')");
    check (hex (ws::sha1 ("", 0)) == "da39a3ee5e6b4b0d3255bfef95601890afd80709", "SHA-1('')");
    const char* fox = "The quick brown fox jumps over the lazy dog";
    check (hex (ws::sha1 (fox, strlen (fox))) == "2fd4e1c67a2d28fced849ee1bb76e7391b93eb12", "SHA-1(fox)");
    std::string big (1000, 'a');
    check (hex (ws::sha1 (big.data(), big.size())) == "291e9a6c66994949b57ba5e650361e98fc36b1ba", "SHA-1(1000 x 'a')");
    check (ws::acceptKey ("dGhlIHNhbXBsZSBub25jZQ==") == "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "WebSocket accept key (RFC 6455 example)");

    // Frame round trip at every length boundary
    bool allOk = true;
    for (size_t len : { (size_t) 0, (size_t) 1, (size_t) 125, (size_t) 126, (size_t) 65535, (size_t) 65536, (size_t) 70000 })
    {
        juce::MemoryBlock payload (len);
        for (size_t i = 0; i < len; ++i) static_cast<juce::uint8*> (payload.getData())[i] = (juce::uint8) (i * 7);
        const auto frame = ws::makeFrame (ws::binary, payload.getData(), len);
        ws::Frame f;
        const int used = ws::parseFrame (static_cast<const juce::uint8*> (frame.getData()), frame.getSize(), f);
        allOk = allOk && used == (int) frame.getSize() && f.opcode == ws::binary && f.payload == payload;
        // incomplete data must ask for more, not fail
        allOk = allOk && ws::parseFrame (static_cast<const juce::uint8*> (frame.getData()), frame.getSize() - 1, f) == 0;
    }
    check (allOk, "WebSocket frames: 0 / 125 / 126 / 65535 / 65536 / 70000 bytes round trip");

    // Masked client frame ("Hello", RFC 6455 section 5.7)
    const juce::uint8 masked[] = { 0x81, 0x85, 0x37, 0xfa, 0x21, 0x3d, 0x7f, 0x9f, 0x4d, 0x51, 0x58 };
    ws::Frame f;
    check (ws::parseFrame (masked, sizeof (masked), f) == (int) sizeof (masked) && f.payload.toString() == "Hello",
           "masked client frame decodes to 'Hello'");

    // Audio packet encoding
    const float samples[] = { 0.0f, 1.0f, -1.0f, 0.5f, 2.0f, -2.0f };
    const auto pkt = ws::makeAudioPacket (samples, 3, 2, 48000, 7);
    const auto* p = static_cast<const juce::uint8*> (pkt.getData());
    auto i16 = [p] (int idx) { return (juce::int16) (p[16 + idx * 2] | (p[17 + idx * 2] << 8)); };
    check (pkt.getSize() == 16 + 12 && p[0] == 'M' && p[3] == '1'
               && (p[4] | p[5] << 8 | p[6] << 16) == 48000 && p[8] == 7 && p[12] == 2
               && i16 (0) == 0 && i16 (1) == 32767 && i16 (2) == -32767 && i16 (3) == 16384
               && i16 (4) == 32767 && i16 (5) == -32767,
           "audio packet header + int16 samples (with clipping)");

    // QR code
    QrCodeComponent qr;
    qr.setText ("http://192.168.0.10:8800/");
    check (qr.getSize() >= 21 && qr.isDark (0, 0) && qr.isDark (6, 6) && ! qr.isDark (7, 7), "QR code generated (finder pattern ok)");

    // Tap never blocks and drops when full
    AudioTap tap;
    std::vector<float> zeros (AudioTap::capacityFrames + 1000, 0.0f);
    tap.push (zeros.data(), zeros.data(), (int) zeros.size());
    check (tap.getFramesDropped() >= 1000, "tap drops (never blocks) when the reader falls behind");
}

void serverTests()
{
    std::cout << "Server tests" << std::endl;
    const double sr = 48000.0;

    AudioTap tap;
    tap.setSampleRate (sr);
    StreamServer server (tap);
    const bool started = server.start (18800, 20);
    check (started && server.getPort() >= 18800, "server listening on port " + juce::String (server.getPort()));
    const int port = server.getPort();

    // Second server on the same range picks the next free port
    AudioTap tap2;
    StreamServer server2 (tap2);
    const bool started2 = server2.start (port, 20);
    check (started2 && server2.getPort() > port, "second instance takes the next port (" + juce::String (server2.getPort()) + ")");
    server2.stop();

    // HTTP
    const auto page = httpGet (port, "/");
    check (page.startsWith ("HTTP/1.1 200") && page.contains ("text/html") && page.contains ("MLiveMIX") && page.contains ("OUVIR"),
           "GET / serves the listening page");
    const auto status = httpGet (port, "/status");
    check (status.contains ("\"listeners\":0") && status.contains ("\"sampleRate\":48000"), "GET /status JSON");
    check (httpGet (port, "/nada").startsWith ("HTTP/1.1 404"), "unknown path -> 404");

    // WebSocket
    TestClient a;
    juce::String header;
    check (a.openWebSocket (port, header) && header.contains ("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo="),
           "WebSocket handshake (101 + correct accept key)");
    ws::Frame f;
    check (a.readFrame (f) && f.opcode == ws::text && f.payload.toString().contains ("\"type\":\"hello\"")
               && f.payload.toString().contains ("48000"),
           "hello message with sample rate");
    juce::Thread::sleep (100);
    check (server.getNumListeners() == 1, "listener counted");

    // Push a known signal and check the received samples bit for bit
    std::vector<juce::int16> expL, expR;
    {
        double ph = 0.0;
        for (int k = 0; k < (int) sr * 2; ++k)
        {
            expL.push_back ((juce::int16) juce::roundToInt (0.5f * (float) std::sin (ph * 440.0) * 32767.0f));
            expR.push_back ((juce::int16) juce::roundToInt (0.5f * (float) std::sin (ph * 660.0) * 32767.0f));
            ph += juce::MathConstants<double>::twoPi / sr;
        }
    }
    double phase = 0.0;
    std::thread producer ([&] { pushTone (tap, 1.0, sr, phase); });
    int packets = 0, offset = -1;
    bool exact = true, sequenceOk = true;
    juce::uint32 lastSeq = 0;
    for (int k = 0; k < 20; ++k)
    {
        if (! a.readFrame (f) || f.opcode != ws::binary) { exact = false; break; }
        const auto* d = static_cast<const juce::uint8*> (f.payload.getData());
        const juce::uint32 seq = (juce::uint32) (d[8] | d[9] << 8 | d[10] << 16 | d[11] << 24);
        const int ch = d[12];
        const int frames = (int) (f.payload.getSize() - 16) / (2 * ch);
        if (frames != 960 || ch != 2) { exact = false; break; }
        if (k > 0 && seq != lastSeq + 1) sequenceOk = false;
        lastSeq = seq;

        auto L = [d] (int i) { return (juce::int16) (d[16 + i * 4] | d[17 + i * 4] << 8); };
        auto R = [d] (int i) { return (juce::int16) (d[18 + i * 4] | d[19 + i * 4] << 8); };

        if (offset < 0) // the stream starts live: find where in the tone the first packet begins
            for (int o = 0; o < (int) sr && offset < 0; ++o)
            {
                bool m = true;
                for (int i = 0; i < 16 && m; ++i)
                    m = L (i) == expL[(size_t) (o + i)] && R (i) == expR[(size_t) (o + i)];
                if (m) offset = o;
            }
        if (offset < 0) { exact = false; break; }

        for (int i = 0; i < frames; ++i)
        {
            const size_t idx = (size_t) (offset + k * 960 + i);
            if (idx >= expL.size() || L (i) != expL[idx] || R (i) != expR[idx]) { exact = false; break; }
        }
        ++packets;
    }
    producer.join();
    check (packets == 20 && exact, "20 packets of 20 ms: every sample identical to the source (16-bit), no gaps");
    check (sequenceOk, "packet sequence numbers are continuous");

    // A client that never reads (phone asleep) must not stall the others
    TestClient sleepy;
    juce::String h2;
    sleepy.openWebSocket (port, h2);
    TestClient b;
    juce::String h3;
    b.openWebSocket (port, h3);
    b.readFrame (f); // hello
    std::thread producer2 ([&] { pushTone (tap, 3.0, sr, phase); });
    int bPackets = 0;
    const auto t0 = juce::Time::getMillisecondCounter();
    while (juce::Time::getMillisecondCounter() - t0 < 2500)
        if (b.readFrame (f, 500) && f.opcode == ws::binary)
            ++bPackets;
    producer2.join();
    check (bPackets > 90, "a stalled client does not hold back the others (" + juce::String (bPackets)
                              + " packets in 2.5 s; server sent " + juce::String ((juce::int64) server.getPacketsSent())
                              + " total, tap dropped " + juce::String ((int) tap.getFramesDropped()) + " frames)");

    // Abrupt disconnects while audio flows must not crash (SIGPIPE) and must be cleaned up
    sleepy.socket.close();
    a.socket.close();
    std::thread producer3 ([&] { pushTone (tap, 2.5, sr, phase); });
    producer3.join();
    b.socket.close();
    juce::Thread::sleep (1500);
    check (server.getNumListeners() == 0, "abrupt disconnects survived and cleaned up (listeners = 0)");

    server.stop();
    check (! server.isRunning() && server.getPort() == 0, "server stops cleanly");
}


/** Plugin -> relay -> listener, against a relay running locally (wrangler dev). */
void remoteTests (const juce::String& relayBase)
{
    std::cout << "Remote tests (relay " << relayBase << ")" << std::endl;
    const double sr = 48000.0;
    const auto hostPort = relayBase.fromFirstOccurrenceOf ("://", false, false);
    const auto relayHost = hostPort.upToFirstOccurrenceOf (":", false, false);
    const int relayPort = hostPort.fromFirstOccurrenceOf (":", false, false).getIntValue();

    AudioTap tap;
    tap.setSampleRate (sr);
    StreamServer server (tap);
    RemoteLink remote;
    server.setRemote (&remote);
    server.start (19800, 20);

    const auto room = RemoteLink::makeRandomId (16), key = RemoteLink::makeRandomId (24);
    remote.configure (relayBase, room, key);
    check (remote.getShareUrl() == relayBase + "/r/" + room && room.length() >= 21, "share link " + remote.getShareUrl());
    remote.start();

    auto waitFor = [] (std::function<bool()> cond, int ms)
    {
        const auto end = juce::Time::getMillisecondCounter() + (juce::uint32) ms;
        while (! cond() && juce::Time::getMillisecondCounter() < end) juce::Thread::sleep (20);
        return cond();
    };
    check (waitFor ([&] { return remote.getState() == RemoteLink::State::live; }, 10000),
           "plugin connected to the relay (" + remote.getLastError() + ")");

    TestClient listener;
    juce::String header;
    check (listener.openWebSocket (relayPort, header, "/r/" + room + "/ws", relayHost), "listener opens the link");
    check (waitFor ([&] { return remote.getListeners() == 1; }, 5000), "plugin sees 1 remote listener");

    // Expected tone (same generator as pushTone)
    std::vector<juce::int16> expL, expR;
    {
        double ph = 0.0;
        for (int k = 0; k < (int) sr * 4; ++k)
        {
            expL.push_back ((juce::int16) juce::roundToInt (0.5f * (float) std::sin (ph * 440.0) * 32767.0f));
            expR.push_back ((juce::int16) juce::roundToInt (0.5f * (float) std::sin (ph * 660.0) * 32767.0f));
            ph += juce::MathConstants<double>::twoPi / sr;
        }
    }
    double phase = 0.0;
    std::thread producer ([&] { pushTone (tap, 3.0, sr, phase); });

    bool helloOk = false, exact = true, seqOk = true;
    int packets = 0, offset = -1;
    juce::uint32 lastSeq = 0;
    const auto end = juce::Time::getMillisecondCounter() + 5000;
    while (packets < 15 && juce::Time::getMillisecondCounter() < end)
    {
        ws::Frame f;
        if (! listener.readFrame (f, 1000)) continue;
        if (f.opcode == ws::text)
        {
            const auto t = f.payload.toString();
            if (t.contains ("\"sampleRate\":48000") && t.contains ("\"remote\":true")) helloOk = true;
            continue;
        }
        const auto* d = static_cast<const juce::uint8*> (f.payload.getData());
        const juce::uint32 seq = (juce::uint32) (d[8] | d[9] << 8 | d[10] << 16 | d[11] << 24);
        const int frames = (int) (f.payload.getSize() - 16) / 4;
        if (frames != 4800) { exact = false; break; }
        if (packets > 0 && seq != lastSeq + 1) seqOk = false;
        lastSeq = seq;
        auto L = [d] (int i) { return (juce::int16) (d[16 + i * 4] | d[17 + i * 4] << 8); };
        auto R = [d] (int i) { return (juce::int16) (d[18 + i * 4] | d[19 + i * 4] << 8); };
        if (offset < 0)
            for (int o = 0; o < (int) sr * 2 && offset < 0; ++o)
            {
                bool m = true;
                for (int i = 0; i < 16 && m; ++i) m = L (i) == expL[(size_t) (o + i)] && R (i) == expR[(size_t) (o + i)];
                if (m) offset = o;
            }
        if (offset < 0) { exact = false; break; }
        for (int i = 0; i < frames && exact; ++i)
        {
            const size_t idx = (size_t) (offset + packets * 4800 + i);
            exact = idx < expL.size() && L (i) == expL[idx] && R (i) == expR[idx];
        }
        ++packets;
    }
    producer.join();
    check (helloOk, "listener got the sample rate through the relay");
    check (packets == 15 && exact && seqOk,
           "15 packets of 100 ms via the internet relay: every sample identical, no gaps (" + juce::String (packets) + ")");

    // Another computer (different key) cannot take over the same link
    RemoteLink intruder;
    intruder.configure (relayBase, room, RemoteLink::makeRandomId (24));
    intruder.start();
    const bool refused = waitFor ([&] { return intruder.getLastError().contains ("outro computador"); }, 8000);
    check (refused, "a different key is refused (\"" + intruder.getLastError() + "\")");
    intruder.stop();

    // Stopping tells the listener
    remote.stop();
    bool toldOffline = false;
    const auto end2 = juce::Time::getMillisecondCounter() + 4000;
    while (! toldOffline && juce::Time::getMillisecondCounter() < end2)
    {
        ws::Frame f;
        if (listener.readFrame (f, 500) && f.opcode == ws::text && f.payload.toString().contains ("\"online\":false"))
            toldOffline = true;
    }
    check (toldOffline && remote.getState() == RemoteLink::State::off, "stopping the transmission tells the listener");

    // Same key again later: the link keeps working (fixed link)
    remote.start();
    check (waitFor ([&] { return remote.getState() == RemoteLink::State::live; }, 10000), "transmission restarts on the same link");
    remote.stop();
    server.setRemote (nullptr);
    server.stop();
}

void processorTests()
{
    std::cout << "Plugin tests" << std::endl;
    MLiveMixAudioProcessor proc;
    proc.setRateAndBufferSizeDetails (44100.0, 512);
    proc.prepareToPlay (44100.0, 512);
    check (proc.getServer().isRunning(), "plugin starts its server in prepareToPlay (port " + juce::String (proc.getServer().getPort()) + ")");

    juce::Random rng (3);
    juce::AudioBuffer<float> buffer (2, 512), copy (2, 512);
    juce::MidiBuffer midi;
    bool identical = true;
    for (int b = 0; b < 50; ++b)
    {
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 512; ++i)
                buffer.setSample (c, i, rng.nextFloat() * 2.0f - 1.0f);
        copy.makeCopyOf (buffer);
        proc.processBlock (buffer, midi);
        for (int c = 0; c < 2; ++c)
            for (int i = 0; i < 512; ++i)
                identical = identical && juce::exactlyEqual (buffer.getSample (c, i), copy.getSample (c, i));
    }
    check (identical, "audio passes through bit-identical (the DAW output is untouched)");
    check (proc.getTap().getFramesWritten() == 50 * 512, "audio copied to the stream");

    juce::AudioProcessor::BusesLayout mono;
    mono.inputBuses.add (juce::AudioChannelSet::mono());
    mono.outputBuses.add (juce::AudioChannelSet::mono());
    check (proc.checkBusesLayoutSupported (mono), "mono master supported");

    juce::MemoryBlock state;
    proc.setStreaming (false);
    proc.preferredAddress = "10.0.0.5";
    proc.getStateInformation (state);
    MLiveMixAudioProcessor other;
    other.setStateInformation (state.getData(), (int) state.getSize());
    check (! other.isStreaming() && other.preferredAddress == "10.0.0.5", "state save/restore");
}
}

int main (int argc, char** argv)
{
    juce::ScopedJuceInitialiser_GUI init;
    const juce::String mode = argc > 1 ? juce::String (argv[1]) : juce::String();

    if (mode == "--serve")
    {
        const double seconds = argc > 2 ? juce::String (argv[2]).getDoubleValue() : 20.0;
        const int port = argc > 3 ? juce::String (argv[3]).getIntValue() : 8800;
        AudioTap tap;
        tap.setSampleRate (48000.0);
        StreamServer server (tap);
        if (! server.start (port, 1)) { std::cout << "could not listen on " << port << std::endl; return 1; }
        std::cout << "serving on http://127.0.0.1:" << server.getPort() << "/ for " << seconds << " s" << std::endl;
        double phase = 0.0;
        pushTone (tap, seconds, 48000.0, phase);
        std::cout << "packets sent: " << server.getPacketsSent() << std::endl;
        server.stop();
        return 0;
    }

    if (mode == "--remote-test" && argc > 2)
    {
        remoteTests (juce::String (argv[2]));
        std::cout << (failures == 0 ? "REMOTE TESTS PASSED" : juce::String (failures) + " REMOTE TEST(S) FAILED") << std::endl;
        return failures == 0 ? 0 : 1;
    }

    if (mode == "--serve-remote" && argc > 5)
    {
        // --serve-remote <seconds> <relayBase> <room> <key>: tone through the relay (browser test)
        AudioTap tap;
        tap.setSampleRate (48000.0);
        StreamServer server (tap);
        RemoteLink remote;
        server.setRemote (&remote);
        server.start (19900, 20);
        remote.configure (juce::String (argv[3]), juce::String (argv[4]), juce::String (argv[5]));
        remote.start();
        std::cout << "remote link: " << remote.getShareUrl() << std::endl;
        double phase = 0.0;
        pushTone (tap, juce::String (argv[2]).getDoubleValue(), 48000.0, phase);
        std::cout << "remote packets sent: " << remote.getPacketsSent() << ", listeners at end: " << remote.getListeners() << std::endl;
        remote.stop();
        server.setRemote (nullptr);
        server.stop();
        return 0;
    }

    if (mode == "--snapshot" && argc > 2)
    {
        MLiveMixAudioProcessor proc;
        proc.prepareToPlay (48000.0, 512);
        std::unique_ptr<juce::AudioProcessorEditor> editor (proc.createEditor());
        if (argc > 3 && juce::String (argv[3]) == "remote")
            if (auto* e = dynamic_cast<MLiveMixAudioProcessorEditor*> (editor.get()))
                e->setRemoteMode (true);
        const auto image = editor->createComponentSnapshot (editor->getLocalBounds(), true, 2.0f);
        juce::File out { juce::String (argv[2]) };
        out.deleteFile();
        juce::PNGImageFormat png;
        if (auto stream = out.createOutputStream())
            png.writeImageToStream (image, *stream);
        std::cout << "snapshot: " << out.getFullPathName() << std::endl;
        return 0;
    }

    std::cout << "MLiveMIX tests" << std::endl;
    unitTests();
    serverTests();
    processorTests();
    std::cout << (failures == 0 ? "ALL TESTS PASSED" : juce::String (failures) + " TEST(S) FAILED") << std::endl;
    return failures == 0 ? 0 : 1;
}
