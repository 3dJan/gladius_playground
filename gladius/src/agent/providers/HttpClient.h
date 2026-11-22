#pragma once

#include "../../io/CancellationToken.h"

#include <coro/coro.hpp>
#include <functional>
#include <string>
#include <vector>

namespace gladius::agent
{
    /// @brief Outgoing HTTP POST request parameters
    struct HttpRequest
    {
        std::string url;
        std::vector<std::string> headers;
        std::string body;
        int timeoutSeconds = 120;
    };

    /// @brief HTTP response summary
    struct HttpResponse
    {
        long statusCode = 0;
        std::string body;
        std::string error;
        bool cancelled = false;
    };

    /// @brief Callback invoked for each complete Server-Sent Events (SSE) message
    using SseEventCallback =
      std::function<void(std::string const & eventName, std::string const & data)>;

    /// @brief Incremental Server-Sent Events (SSE) stream parser
    class SseStreamParser
    {
      public:
        explicit SseStreamParser(SseEventCallback onEvent);

        /// @brief Feed a raw chunk of UTF-8 bytes from the HTTP stream
        void feed(std::string const & chunk);

        /// @brief Flush any pending event at end-of-stream
        void finish();

      private:
        void processLine(std::string const & line);
        void dispatchEvent();

        SseEventCallback m_onEvent;
        std::string m_buffer;
        std::string m_currentEvent;
        std::string m_currentData;
        bool m_hasData = false;
    };

    /// @brief Transport abstraction for HTTP JSON and SSE requests (enables unit test mocking)
    class IHttpTransport
    {
      public:
        virtual ~IHttpTransport() = default;

        [[nodiscard]] virtual HttpResponse
        getJson(HttpRequest const & /*request*/, io::CancellationToken const & /*cancellation*/)
        {
            return HttpResponse{0, "", "GET not implemented", false};
        }

        [[nodiscard]] virtual HttpResponse
        postJson(HttpRequest const & request, io::CancellationToken const & cancellation) = 0;

        [[nodiscard]] virtual HttpResponse
        postSse(HttpRequest const & request,
                SseEventCallback onEvent,
                io::CancellationToken const & cancellation) = 0;

        [[nodiscard]] virtual coro::task<HttpResponse>
        getJsonAsync(HttpRequest const & request, io::CancellationToken const & cancellation)
        {
            co_return getJson(request, cancellation);
        }

        [[nodiscard]] virtual coro::task<HttpResponse>
        postJsonAsync(HttpRequest const & request, io::CancellationToken const & cancellation)
        {
            co_return postJson(request, cancellation);
        }

        [[nodiscard]] virtual coro::task<HttpResponse>
        postSseAsync(HttpRequest const & request,
                     SseEventCallback onEvent,
                     io::CancellationToken const & cancellation)
        {
            co_return postSse(request, std::move(onEvent), cancellation);
        }
    };

    /// @brief libcurl-backed HTTPS + SSE transport for native builds (Fetch API on Emscripten)
    class CurlHttpTransport final : public IHttpTransport
    {
      public:
        CurlHttpTransport();

        [[nodiscard]] HttpResponse
        getJson(HttpRequest const & request, io::CancellationToken const & cancellation) override;

        [[nodiscard]] HttpResponse
        postJson(HttpRequest const & request, io::CancellationToken const & cancellation) override;

        [[nodiscard]] HttpResponse
        postSse(HttpRequest const & request,
                SseEventCallback onEvent,
                io::CancellationToken const & cancellation) override;

        [[nodiscard]] coro::task<HttpResponse>
        getJsonAsync(HttpRequest const & request,
                     io::CancellationToken const & cancellation) override;

        [[nodiscard]] coro::task<HttpResponse>
        postJsonAsync(HttpRequest const & request,
                      io::CancellationToken const & cancellation) override;

        [[nodiscard]] coro::task<HttpResponse>
        postSseAsync(HttpRequest const & request,
                     SseEventCallback onEvent,
                     io::CancellationToken const & cancellation) override;
    };
} // namespace gladius::agent
