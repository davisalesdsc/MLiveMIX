// macOS WebSocket client on NSURLSessionWebSocketTask (TLS handled by the system).
// Compiled with ARC (-fobjc-arc, see CMakeLists.txt).

#include "WsClient.h"

#if JUCE_MAC

#import <Foundation/Foundation.h>
#include <atomic>
#include <mutex>

namespace mlm
{
namespace
{
    /** State shared with the completion handlers, which may run after the client is gone. */
    struct Shared
    {
        std::mutex lock;
        juce::StringArray texts;
        std::atomic<bool> alive { false };
        std::atomic<int> pendingSends { 0 };
    };

    API_AVAILABLE (macos (10.15))
    void receiveLoop (NSURLSessionWebSocketTask* task, std::shared_ptr<Shared> shared)
    {
        [task receiveMessageWithCompletionHandler: ^(NSURLSessionWebSocketMessage* message, NSError* error)
        {
            if (error != nil || message == nil)
            {
                shared->alive = false;
                return;
            }
            if (message.type == NSURLSessionWebSocketMessageTypeString && message.string != nil)
            {
                std::lock_guard<std::mutex> l (shared->lock);
                shared->texts.add (juce::String::fromUTF8 (message.string.UTF8String));
            }
            receiveLoop (task, shared);
        }];
    }

    API_AVAILABLE (macos (10.15))
    class AppleWsClient : public WsClient
    {
    public:
        ~AppleWsClient() override { close(); }

        bool connect (const juce::String& url, int timeoutMs, juce::String& error) override
        {
            @autoreleasepool
            {
                NSURL* u = [NSURL URLWithString: [NSString stringWithUTF8String: url.toRawUTF8()]];
                if (u == nil)
                {
                    error = "Endereço inválido";
                    return false;
                }

                NSURLSessionConfiguration* config = [NSURLSessionConfiguration ephemeralSessionConfiguration];
                config.timeoutIntervalForRequest = (double) timeoutMs / 1000.0;
                session = [NSURLSession sessionWithConfiguration: config];
                task = [session webSocketTaskWithURL: u];
                task.maximumMessageSize = 1 << 20;
                [task resume];

                // The relay always sends a first message to the transmitter (the listener count).
                // Waiting for it proves the handshake (HTTP 101) succeeded; a refusal (403, ...) or a
                // network error ends the receive with an error instead.
                dispatch_semaphore_t done = dispatch_semaphore_create (0);
                auto firstError = std::make_shared<NSError* __strong> (nil);
                auto s = shared;
                [task receiveMessageWithCompletionHandler: ^(NSURLSessionWebSocketMessage* message, NSError* e)
                {
                    if (e != nil || message == nil)
                        *firstError = e != nil ? e : [NSError errorWithDomain: @"MLiveMIX" code: -1 userInfo: nil];
                    else if (message.type == NSURLSessionWebSocketMessageTypeString && message.string != nil)
                    {
                        std::lock_guard<std::mutex> l (s->lock);
                        s->texts.add (juce::String::fromUTF8 (message.string.UTF8String));
                    }
                    dispatch_semaphore_signal (done);
                }];

                const auto deadline = dispatch_time (DISPATCH_TIME_NOW, (int64_t) timeoutMs * (int64_t) NSEC_PER_MSEC);
                if (dispatch_semaphore_wait (done, deadline) != 0)
                {
                    error = "Tempo esgotado ao conectar";
                    close();
                    return false;
                }
                if (*firstError != nil)
                {
                    error = describe (*firstError);
                    close();
                    return false;
                }

                shared->alive = true;
                receiveLoop (task, shared);
                return true;
            }
        }

        bool sendBinary (const void* data, size_t size) override
        {
            if (! shared->alive || task == nil)
                return false;
            if (shared->pendingSends.load() > 64)
                return true; // network slower than the audio: drop this packet rather than pile up

            @autoreleasepool
            {
                NSData* d = [NSData dataWithBytes: data length: size];
                send ([[NSURLSessionWebSocketMessage alloc] initWithData: d]);
            }
            return shared->alive.load();
        }

        bool sendText (const juce::String& text) override
        {
            if (! shared->alive || task == nil)
                return false;
            @autoreleasepool
            {
                NSString* s = [NSString stringWithUTF8String: text.toRawUTF8()];
                send ([[NSURLSessionWebSocketMessage alloc] initWithString: s]);
            }
            return shared->alive.load();
        }

        bool poll (juce::StringArray& texts) override
        {
            std::lock_guard<std::mutex> l (shared->lock);
            texts.addArray (shared->texts);
            shared->texts.clear();
            return shared->alive.load();
        }

        void close() override
        {
            shared->alive = false;
            if (task != nil)
                [task cancelWithCloseCode: NSURLSessionWebSocketCloseCodeNormalClosure reason: nil];
            if (session != nil)
                [session invalidateAndCancel];
            task = nil;
            session = nil;
        }

    private:
        void send (NSURLSessionWebSocketMessage* message)
        {
            auto s = shared;
            s->pendingSends.fetch_add (1);
            [task sendMessage: message completionHandler: ^(NSError* e)
            {
                s->pendingSends.fetch_sub (1);
                if (e != nil)
                    s->alive = false;
            }];
        }

        juce::String describe (NSError* e)
        {
            if (auto* http = (NSHTTPURLResponse*) task.response)
                if ([http isKindOfClass: [NSHTTPURLResponse class]] && http.statusCode != 101 && http.statusCode != 0)
                    return "Servidor recusou (" + juce::String ((int) http.statusCode) + ")";
            return juce::String::fromUTF8 (e.localizedDescription.UTF8String);
        }

        NSURLSession* session = nil;
        NSURLSessionWebSocketTask* task = nil;
        std::shared_ptr<Shared> shared = std::make_shared<Shared>();
    };
}

std::unique_ptr<WsClient> createAppleWsClient()
{
    if (@available (macOS 10.15, *))
        return std::make_unique<AppleWsClient>();
    return nullptr;
}
}

#endif
