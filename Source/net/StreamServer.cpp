#include "StreamServer.h"
#include "WebSocket.h"
#include "NetUtil.h"
#include <BinaryData.h>

namespace mlm
{
namespace
{
    constexpr int maxRequestBytes = 16 * 1024;
    constexpr int maxQueuedPackets = 50;   // ~1 s of audio per client before old packets are dropped
    constexpr int packetMs = 20;

    juce::String httpResponse (const juce::String& status, const juce::String& contentType, size_t contentLength,
                               const juce::String& extraHeaders = {})
    {
        return "HTTP/1.1 " + status + "\r\n"
             + "Content-Type: " + contentType + "\r\n"
             + "Content-Length: " + juce::String ((juce::int64) contentLength) + "\r\n"
             + "Cache-Control: no-store\r\n"
             + "Connection: close\r\n"
             + extraHeaders
             + "\r\n";
    }
}

//==============================================================================
class StreamServer::Client : public juce::Thread
{
public:
    Client (StreamServer& o, std::unique_ptr<juce::StreamingSocket> s)
        : juce::Thread ("MLiveMIX client"), owner (o), socket (std::move (s))
    {
        net::preventSigPipe (*socket);
    }

    ~Client() override
    {
        signalThreadShouldExit();
        socket->close();       // unblocks any pending read/write
        wakeUp.signal();
        stopThread (3000);
    }

    void run() override
    {
        handle();
        if (isWebSocket)
            owner.listeners.fetch_sub (1);
        socket->close();
        finished.store (true);
    }

    /** Called by the sender thread. Never blocks on the network. */
    void enqueue (const juce::MemoryBlock& frame)
    {
        if (! isWebSocket || finished.load())
            return;
        {
            const juce::ScopedLock sl (queueLock);
            while ((int) queue.size() >= maxQueuedPackets)
                queue.pop_front(); // slow client (e.g. phone screen off): drop the oldest
            queue.push_back (frame);
        }
        wakeUp.signal();
    }

    bool isFinished() const noexcept { return finished.load(); }

private:
    void handle()
    {
        juce::String request;
        if (! readRequest (request))
            return;

        const auto lines = juce::StringArray::fromLines (request);
        const auto first = juce::StringArray::fromTokens (lines[0], " ", "");
        if (first.size() < 2 || first[0] != "GET")
        {
            sendText ("405 Method Not Allowed", "text/plain; charset=utf-8", "Método não permitido");
            return;
        }

        juce::StringPairArray headers;
        for (int i = 1; i < lines.size(); ++i)
        {
            const int colon = lines[i].indexOfChar (':');
            if (colon > 0)
                headers.set (lines[i].substring (0, colon).trim().toLowerCase(), lines[i].substring (colon + 1).trim());
        }

        const auto path = first[1].upToFirstOccurrenceOf ("?", false, false);

        if (path == "/ws")
        {
            if (! headers["upgrade"].equalsIgnoreCase ("websocket") || headers["sec-websocket-key"].isEmpty())
            {
                sendText ("400 Bad Request", "text/plain; charset=utf-8", "WebSocket esperado");
                return;
            }
            runWebSocket (headers["sec-websocket-key"]);
        }
        else if (path == "/" || path == "/index.html")
        {
            const auto html = owner.getPageCopy();
            sendText ("200 OK", "text/html; charset=utf-8", html);
        }
        else if (path == "/status")
        {
            sendText ("200 OK", "application/json; charset=utf-8", owner.statusJson(),
                      "Access-Control-Allow-Origin: *\r\n");
        }
        else if (path == "/favicon.ico")
        {
            const auto head = httpResponse ("204 No Content", "image/x-icon", 0);
            net::sendAll (*socket, head.toRawUTF8(), head.getNumBytesAsUTF8());
        }
        else
        {
            sendText ("404 Not Found", "text/plain; charset=utf-8", "Não encontrado");
        }
    }

    bool readRequest (juce::String& out)
    {
        juce::MemoryBlock data;
        char buf[2048];
        const auto deadline = juce::Time::getMillisecondCounter() + 5000;

        while (! threadShouldExit() && juce::Time::getMillisecondCounter() < deadline)
        {
            const int ready = socket->waitUntilReady (true, 100);
            if (ready < 0)
                return false;
            if (ready == 0)
                continue;

            const int n = socket->read (buf, (int) sizeof (buf), false);
            if (n <= 0)
                return false;
            data.append (buf, (size_t) n);

            const auto text = data.toString();
            if (text.contains ("\r\n\r\n"))
            {
                out = text.upToFirstOccurrenceOf ("\r\n\r\n", false, false);
                return true;
            }
            if ((int) data.getSize() > maxRequestBytes)
                return false;
        }
        return false;
    }

    void sendText (const juce::String& status, const juce::String& type, const juce::String& body,
                   const juce::String& extraHeaders = {})
    {
        const auto head = httpResponse (status, type, body.getNumBytesAsUTF8(), extraHeaders);
        if (net::sendAll (*socket, head.toRawUTF8(), head.getNumBytesAsUTF8()))
            net::sendAll (*socket, body.toRawUTF8(), body.getNumBytesAsUTF8());
    }

    void runWebSocket (const juce::String& key)
    {
        const juce::String response = "HTTP/1.1 101 Switching Protocols\r\n"
                                      "Upgrade: websocket\r\n"
                                      "Connection: Upgrade\r\n"
                                      "Sec-WebSocket-Accept: " + ws::acceptKey (key) + "\r\n\r\n";
        if (! net::sendAll (*socket, response.toRawUTF8(), response.getNumBytesAsUTF8()))
            return;

        isWebSocket = true;
        owner.listeners.fetch_add (1);

        const auto hello = juce::String ("{\"type\":\"hello\",\"name\":\"MLiveMIX\",\"version\":\"")
                         + JucePlugin_VersionString + "\",\"sampleRate\":" + juce::String (owner.tap.getSampleRate()) + "}";
        if (! sendFrame (ws::makeFrame (ws::text, hello.toRawUTF8(), hello.getNumBytesAsUTF8())))
            return;

        juce::MemoryBlock rx;
        char buf[1024];

        while (! threadShouldExit())
        {
            wakeUp.wait (25);

            // 1) send whatever is queued
            for (;;)
            {
                juce::MemoryBlock frame;
                {
                    const juce::ScopedLock sl (queueLock);
                    if (queue.empty())
                        break;
                    frame = std::move (queue.front());
                    queue.pop_front();
                }
                if (! sendFrame (frame))
                    return;
            }

            // 2) handle what the browser sent (ping / close)
            const int ready = socket->waitUntilReady (true, 0);
            if (ready < 0)
                return;
            if (ready > 0)
            {
                const int n = socket->read (buf, (int) sizeof (buf), false);
                if (n <= 0)
                    return; // closed
                rx.append (buf, (size_t) n);

                for (;;)
                {
                    ws::Frame f;
                    const int used = ws::parseFrame (static_cast<const juce::uint8*> (rx.getData()), rx.getSize(), f, 64 * 1024);
                    if (used < 0)
                        return;
                    if (used == 0)
                        break;
                    rx.removeSection (0, (size_t) used);

                    if (f.opcode == ws::close)
                    {
                        sendFrame (ws::makeFrame (ws::close, nullptr, 0));
                        return;
                    }
                    if (f.opcode == ws::ping)
                        sendFrame (ws::makeFrame (ws::pong, f.payload.getData(), f.payload.getSize()));
                }
            }
        }
    }

    bool sendFrame (const juce::MemoryBlock& frame)
    {
        return net::sendAll (*socket, frame.getData(), frame.getSize());
    }

    StreamServer& owner;
    std::unique_ptr<juce::StreamingSocket> socket;
    juce::CriticalSection queueLock;
    std::deque<juce::MemoryBlock> queue;
    juce::WaitableEvent wakeUp;
    std::atomic<bool> finished { false };
    std::atomic<bool> isWebSocket { false };
};

//==============================================================================
class StreamServer::ListenerThread : public juce::Thread
{
public:
    explicit ListenerThread (StreamServer& o) : juce::Thread ("MLiveMIX listener"), owner (o) {}

    void run() override
    {
        while (! threadShouldExit())
        {
            std::unique_ptr<juce::StreamingSocket> s (owner.listenSocket->waitForNextConnection());
            if (threadShouldExit())
                break;
            if (s != nullptr)
                owner.addClient (std::move (s));
            else
                wait (50); // listener closed or transient error
        }
    }

private:
    StreamServer& owner;
};

//==============================================================================
class StreamServer::SenderThread : public juce::Thread
{
public:
    explicit SenderThread (StreamServer& o) : juce::Thread ("MLiveMIX sender"), owner (o) {}

    void run() override
    {
        std::vector<float> pending;
        std::vector<float> chunk;
        juce::uint32 sequence = 0;
        int cleanupCounter = 0;

        while (! threadShouldExit())
        {
            wait (5);

            const int sr = juce::jmax (8000, owner.tap.getSampleRate());
            const int framesPerPacket = juce::jmax (64, sr * packetMs / 1000);

            const int ready = owner.tap.getNumReady();
            if (ready > 0)
            {
                chunk.resize ((size_t) ready * 2);
                const int got = owner.tap.pop (chunk.data(), ready);
                pending.insert (pending.end(), chunk.begin(), chunk.begin() + got * 2);
            }

            auto* remote = owner.remote.load();
            const bool remoteWants = remote != nullptr && remote->wantsAudio();

            // Nobody listening: throw the audio away, so a new listener starts "live", not with old audio
            if (owner.listeners.load() == 0 && ! remoteWants)
                pending.clear();

            // Never let a backlog build up (e.g. a stalled thread)
            const size_t maxPending = (size_t) framesPerPacket * 2 * 25;
            if (pending.size() > maxPending)
                pending.erase (pending.begin(), pending.end() - (std::ptrdiff_t) maxPending);

            while ((int) (pending.size() / 2) >= framesPerPacket)
            {
                if (owner.listeners.load() > 0)
                {
                    const auto packet = ws::makeAudioPacket (pending.data(), framesPerPacket, 2, sr, sequence);
                    owner.broadcast (ws::makeFrame (ws::binary, packet.getData(), packet.getSize()));
                    owner.packetsSent.fetch_add (1);
                }
                if (remoteWants)
                    remote->pushAudio (pending.data(), framesPerPacket, sr);
                ++sequence;
                pending.erase (pending.begin(), pending.begin() + framesPerPacket * 2);
            }

            if (++cleanupCounter >= 200) // ~1 s
            {
                cleanupCounter = 0;
                owner.removeFinishedClients();
            }
        }
    }

private:
    StreamServer& owner;
};

//==============================================================================
StreamServer::StreamServer (AudioTap& t) : tap (t)
{
    page = juce::String::fromUTF8 (BinaryData::index_html, BinaryData::index_htmlSize);
}

StreamServer::~StreamServer()
{
    stop();
}

void StreamServer::setPage (const juce::String& html)
{
    const juce::ScopedLock sl (pageLock);
    page = html;
}

juce::String StreamServer::getLastError() const
{
    const juce::ScopedLock sl (errorLock);
    return lastError;
}

bool StreamServer::start (int firstPort, int attempts)
{
    if (running.load())
        return true;

    for (int p = firstPort; p < firstPort + attempts; ++p)
    {
        auto s = std::make_unique<juce::StreamingSocket>();
        if (s->createListener (p, {}))
        {
            listenSocket = std::move (s);
            port.store (p);
            running.store (true);

            listenerThread = std::make_unique<ListenerThread> (*this);
            senderThread = std::make_unique<SenderThread> (*this);
            listenerThread->startThread();
            senderThread->startThread (juce::Thread::Priority::high);
            {
                const juce::ScopedLock sl (errorLock);
                lastError.clear();
            }
            return true;
        }
    }

    const juce::ScopedLock sl (errorLock);
    lastError = "Nenhuma porta livre entre " + juce::String (firstPort) + " e " + juce::String (firstPort + attempts - 1);
    return false;
}

void StreamServer::stop()
{
    if (! running.exchange (false))
        return;

    if (listenerThread != nullptr)
        listenerThread->signalThreadShouldExit();
    if (listenSocket != nullptr)
        listenSocket->close(); // unblocks accept()
    if (listenerThread != nullptr)
        listenerThread->stopThread (3000);
    if (senderThread != nullptr)
        senderThread->stopThread (3000);

    {
        const juce::ScopedLock sl (clientLock);
        clients.clear(); // each Client closes its socket and joins its thread
    }

    listenerThread.reset();
    senderThread.reset();
    listenSocket.reset();
    listeners.store (0);
    port.store (0);
}

void StreamServer::addClient (std::unique_ptr<juce::StreamingSocket> socket)
{
    auto* c = new Client (*this, std::move (socket));
    {
        const juce::ScopedLock sl (clientLock);
        clients.add (c);
    }
    c->startThread();
}

void StreamServer::broadcast (const juce::MemoryBlock& frame)
{
    const juce::ScopedLock sl (clientLock);
    for (auto* c : clients)
        c->enqueue (frame);
}

void StreamServer::removeFinishedClients()
{
    const juce::ScopedLock sl (clientLock);
    for (int i = clients.size(); --i >= 0;)
        if (clients[i]->isFinished())
            clients.remove (i);
}

juce::String StreamServer::statusJson() const
{
    return "{\"name\":\"MLiveMIX\",\"version\":\"" + juce::String (JucePlugin_VersionString)
         + "\",\"listeners\":" + juce::String (listeners.load())
         + ",\"sampleRate\":" + juce::String (tap.getSampleRate())
         + ",\"streaming\":" + (tap.isEnabled() ? "true" : "false")
         + ",\"port\":" + juce::String (port.load()) + "}";
}
}
