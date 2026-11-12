#if defined (_WIN32)
 #include <winsock2.h>
#else
 #include <sys/types.h>
 #include <sys/socket.h>
 #include <cerrno>
#endif

#include "NetUtil.h"

namespace mlm::net
{
namespace
{
    int rank (const juce::IPAddress& a)
    {
        const auto& b = a.address;
        if (b[0] == 192 && b[1] == 168)              return 0;
        if (b[0] == 10)                              return 1;
        if (b[0] == 172 && b[1] >= 16 && b[1] <= 31) return 2;
        return -1; // not a private LAN address
    }
}

juce::StringArray getLanAddresses()
{
    juce::Array<juce::IPAddress> all;
    juce::IPAddress::findAllAddresses (all, false);

    std::vector<std::pair<int, juce::String>> found;
    for (const auto& a : all)
    {
        const int r = rank (a);
        if (r >= 0)
            found.emplace_back (r, a.toString());
    }
    std::stable_sort (found.begin(), found.end(), [] (const auto& x, const auto& y) { return x.first < y.first; });

    juce::StringArray result;
    for (const auto& f : found)
        result.addIfNotAlreadyThere (f.second);
    return result;
}

void preventSigPipe (juce::StreamingSocket& socket)
{
   #if defined (SO_NOSIGPIPE)
    int one = 1;
    setsockopt (socket.getRawSocketHandle(), SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof (one));
   #else
    juce::ignoreUnused (socket);
   #endif
}

bool sendAll (juce::StreamingSocket& socket, const void* data, size_t size)
{
    const auto handle = socket.getRawSocketHandle();
    if (handle < 0 || ! socket.isConnected())
        return false;

    const char* p = static_cast<const char*> (data);
    size_t left = size;

   #if defined (MSG_NOSIGNAL)
    const int flags = MSG_NOSIGNAL; // Linux
   #else
    const int flags = 0;            // macOS uses SO_NOSIGPIPE, Windows has no SIGPIPE
   #endif

    while (left > 0)
    {
       #if defined (_WIN32)
        const int n = ::send ((SOCKET) handle, p, (int) juce::jmin (left, (size_t) (1 << 30)), flags);
       #else
        const auto n = ::send (handle, p, left, flags);
        if (n < 0 && errno == EINTR)
            continue;
       #endif
        if (n <= 0)
            return false;

        p += n;
        left -= (size_t) n;
    }
    return true;
}
}
