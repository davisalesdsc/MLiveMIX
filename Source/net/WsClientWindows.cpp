// Windows WebSocket client on the WinHTTP WebSocket API (TLS handled by the system).

#if defined (_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #include <windows.h>
 #include <winhttp.h>
#endif

#include "WsClient.h"

#if JUCE_WINDOWS

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace mlm
{
namespace
{
    class WinHttpWsClient : public WsClient
    {
    public:
        ~WinHttpWsClient() override { close(); }

        bool connect (const juce::String& url, int timeoutMs, juce::String& error) override
        {
            // WinHttpCrackUrl only knows http/https: ws -> http, wss -> https
            juce::String httpUrl = url;
            if (httpUrl.startsWithIgnoreCase ("wss://"))     httpUrl = "https://" + httpUrl.substring (6);
            else if (httpUrl.startsWithIgnoreCase ("ws://")) httpUrl = "http://" + httpUrl.substring (5);

            const std::wstring wide = httpUrl.toWideCharPointer();
            std::vector<wchar_t> host (512), path (4096), extra (4096);
            URL_COMPONENTS uc {};
            uc.dwStructSize = sizeof (uc);
            uc.lpszHostName = host.data();   uc.dwHostNameLength = (DWORD) host.size();
            uc.lpszUrlPath = path.data();    uc.dwUrlPathLength = (DWORD) path.size();
            uc.lpszExtraInfo = extra.data(); uc.dwExtraInfoLength = (DWORD) extra.size();
            if (! WinHttpCrackUrl (wide.c_str(), 0, 0, &uc))
            {
                error = "Endereço inválido";
                return false;
            }
            const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;
            const std::wstring fullPath = std::wstring (uc.lpszUrlPath, uc.dwUrlPathLength)
                                        + std::wstring (uc.lpszExtraInfo, uc.dwExtraInfoLength);

            session = WinHttpOpen (L"MLiveMIX", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (session == nullptr) { error = "WinHTTP indisponível"; return false; }
            WinHttpSetTimeouts (session, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

            connection = WinHttpConnect (session, std::wstring (uc.lpszHostName, uc.dwHostNameLength).c_str(), uc.nPort, 0);
            if (connection == nullptr) { error = "Não foi possível conectar"; close(); return false; }

            HINTERNET request = WinHttpOpenRequest (connection, L"GET", fullPath.c_str(), nullptr, WINHTTP_NO_REFERER,
                                                    WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
            if (request == nullptr) { error = "Falha ao criar o pedido"; close(); return false; }

            bool ok = WinHttpSetOption (request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0)
                   && WinHttpSendRequest (request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0)
                   && WinHttpReceiveResponse (request, nullptr);
            if (! ok)
            {
                error = "Não foi possível conectar (erro " + juce::String ((int) GetLastError()) + ")";
                WinHttpCloseHandle (request);
                close();
                return false;
            }

            DWORD status = 0, statusSize = sizeof (status);
            WinHttpQueryHeaders (request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                 &status, &statusSize, WINHTTP_NO_HEADER_INDEX);
            if (status != 101)
            {
                error = "Servidor recusou (" + juce::String ((int) status) + ")";
                WinHttpCloseHandle (request);
                close();
                return false;
            }

            socket = WinHttpWebSocketCompleteUpgrade (request, 0);
            WinHttpCloseHandle (request);
            if (socket == nullptr) { error = "Falha ao abrir o WebSocket"; close(); return false; }

            alive = true;
            receiver = std::thread ([this] { receiveLoop(); });
            return true;
        }

        bool sendBinary (const void* data, size_t size) override
        {
            if (! alive) return false;
            if (WinHttpWebSocketSend (socket, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, (PVOID) data, (DWORD) size) != NO_ERROR)
                alive = false;
            return alive.load();
        }

        bool sendText (const juce::String& text) override
        {
            if (! alive) return false;
            if (WinHttpWebSocketSend (socket, WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE,
                                      (PVOID) text.toRawUTF8(), (DWORD) text.getNumBytesAsUTF8()) != NO_ERROR)
                alive = false;
            return alive.load();
        }

        bool poll (juce::StringArray& texts) override
        {
            std::lock_guard<std::mutex> l (lock);
            texts.addArray (received);
            received.clear();
            return alive.load();
        }

        void close() override
        {
            alive = false;
            if (socket != nullptr)
            {
                WinHttpCloseHandle (socket); // also aborts the pending receive
                socket = nullptr;
            }
            if (receiver.joinable())
                receiver.join();
            if (connection != nullptr) { WinHttpCloseHandle (connection); connection = nullptr; }
            if (session != nullptr)    { WinHttpCloseHandle (session);    session = nullptr; }
        }

    private:
        void receiveLoop()
        {
            std::vector<BYTE> buffer (64 * 1024);
            std::string message;
            while (alive)
            {
                DWORD read = 0;
                WINHTTP_WEB_SOCKET_BUFFER_TYPE type {};
                if (WinHttpWebSocketReceive (socket, buffer.data(), (DWORD) buffer.size(), &read, &type) != NO_ERROR
                    || type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE)
                {
                    alive = false;
                    break;
                }
                if (type == WINHTTP_WEB_SOCKET_UTF8_FRAGMENT_BUFFER_TYPE || type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
                {
                    message.append ((const char*) buffer.data(), read);
                    if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE)
                    {
                        std::lock_guard<std::mutex> l (lock);
                        received.add (juce::String::fromUTF8 (message.data(), (int) message.size()));
                        message.clear();
                    }
                }
            }
        }

        HINTERNET session = nullptr, connection = nullptr, socket = nullptr;
        std::atomic<bool> alive { false };
        std::thread receiver;
        std::mutex lock;
        juce::StringArray received;
    };
}

std::unique_ptr<WsClient> createWinHttpWsClient()
{
    return std::make_unique<WinHttpWsClient>();
}
}

#endif
