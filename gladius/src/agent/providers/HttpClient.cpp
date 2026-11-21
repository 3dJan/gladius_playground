#include "HttpClient.h"

#if !defined(__EMSCRIPTEN__)
#include <curl/curl.h>
#endif
#include <mutex>

namespace gladius::agent
{
    namespace
    {
#if !defined(__EMSCRIPTEN__)
        void ensureCurlGlobalInit()
        {
            static std::once_flag flag;
            std::call_once(flag, []() { curl_global_init(CURL_GLOBAL_DEFAULT); });
        }

        struct CurlWriteContext
        {
            std::string * responseBody = nullptr;
            SseStreamParser * sseParser = nullptr;
        };

        std::size_t curlWriteCallback(char * ptr, std::size_t size, std::size_t nmemb, void * userdata)
        {
            std::size_t const total = size * nmemb;
            auto * ctx = static_cast<CurlWriteContext *>(userdata);
            if (ctx == nullptr)
            {
                return total;
            }
            if (ctx->sseParser != nullptr)
            {
                ctx->sseParser->feed(std::string(ptr, total));
            }
            else if (ctx->responseBody != nullptr)
            {
                ctx->responseBody->append(ptr, total);
            }
            return total;
        }

        int curlProgressCallback(void * clientp,
                                 curl_off_t /*dltotal*/,
                                 curl_off_t /*dlnow*/,
                                 curl_off_t /*ultotal*/,
                                 curl_off_t /*ulnow*/)
        {
            auto const * token = static_cast<io::CancellationToken const *>(clientp);
            if (token != nullptr && token->isCancelled())
            {
                return 1; // Non-zero aborts transfer with CURLE_ABORTED_BY_CALLBACK
            }
            return 0;
        }

        HttpResponse performCurlPost(HttpRequest const & request,
                                     SseStreamParser * sseParser,
                                     io::CancellationToken const & cancellation)
        {
            HttpResponse response;
            if (cancellation.isCancelled())
            {
                response.cancelled = true;
                response.error = "Request cancelled";
                return response;
            }

            CURL * curl = curl_easy_init();
            if (curl == nullptr)
            {
                response.error = "Failed to initialize CURL handle";
                return response;
            }

            curl_slist * headerList = nullptr;
            for (auto const & h : request.headers)
            {
                headerList = curl_slist_append(headerList, h.c_str());
            }

            CurlWriteContext writeCtx;
            writeCtx.responseBody = &response.body;
            writeCtx.sseParser = sseParser;

            curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList);
            curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, &curlWriteCallback);
            curl_easy_setopt(curl, CURLOPT_WRITEDATA, &writeCtx);
            curl_easy_setopt(curl, CURLOPT_TIMEOUT, static_cast<long>(request.timeoutSeconds));
            curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, &curlProgressCallback);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, const_cast<io::CancellationToken *>(&cancellation));

            CURLcode const code = curl_easy_perform(curl);
            curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);

            if (code == CURLE_ABORTED_BY_CALLBACK)
            {
                response.cancelled = true;
                response.error = "Request cancelled";
            }
            else if (code != CURLE_OK)
            {
                response.error = curl_easy_strerror(code);
            }

            if (sseParser != nullptr)
            {
                sseParser->finish();
            }

            curl_slist_free_all(headerList);
            curl_easy_cleanup(curl);
            return response;
        }
#endif
    } // namespace

    SseStreamParser::SseStreamParser(SseEventCallback onEvent)
        : m_onEvent(std::move(onEvent))
    {
    }

    void SseStreamParser::feed(std::string const & chunk)
    {
        m_buffer.append(chunk);
        std::size_t pos = 0;
        while (true)
        {
            std::size_t const nl = m_buffer.find('\n', pos);
            if (nl == std::string::npos)
            {
                break;
            }
            std::string line = m_buffer.substr(pos, nl - pos);
            if (!line.empty() && line.back() == '\r')
            {
                line.pop_back();
            }
            processLine(line);
            pos = nl + 1;
        }
        if (pos > 0)
        {
            m_buffer.erase(0, pos);
        }
    }

    void SseStreamParser::finish()
    {
        if (!m_buffer.empty())
        {
            if (m_buffer.back() == '\r')
            {
                m_buffer.pop_back();
            }
            processLine(m_buffer);
            m_buffer.clear();
        }
        dispatchEvent();
    }

    void SseStreamParser::processLine(std::string const & line)
    {
        if (line.empty())
        {
            dispatchEvent();
            return;
        }
        if (line[0] == ':')
        {
            return; // SSE comment / keepalive
        }

        std::string field = line;
        std::string value;
        std::size_t const colon = line.find(':');
        if (colon != std::string::npos)
        {
            field = line.substr(0, colon);
            std::size_t valStart = colon + 1;
            if (valStart < line.size() && line[valStart] == ' ')
            {
                ++valStart;
            }
            value = line.substr(valStart);
        }

        if (field == "event")
        {
            m_currentEvent = value;
        }
        else if (field == "data")
        {
            if (m_hasData)
            {
                m_currentData.push_back('\n');
            }
            m_currentData.append(value);
            m_hasData = true;
        }
    }

    void SseStreamParser::dispatchEvent()
    {
        if (m_hasData && m_onEvent)
        {
            m_onEvent(m_currentEvent, m_currentData);
        }
        m_currentEvent.clear();
        m_currentData.clear();
        m_hasData = false;
    }

    CurlHttpTransport::CurlHttpTransport()
    {
#if !defined(__EMSCRIPTEN__)
        ensureCurlGlobalInit();
#endif
    }

    HttpResponse CurlHttpTransport::postJson(HttpRequest const & request,
                                             io::CancellationToken const & cancellation)
    {
#if !defined(__EMSCRIPTEN__)
        return performCurlPost(request, nullptr, cancellation);
#else
        (void) request;
        (void) cancellation;
        return HttpResponse{0, "", "Browser fetch transport not yet wired", false};
#endif
    }

    HttpResponse CurlHttpTransport::postSse(HttpRequest const & request,
                                            SseEventCallback onEvent,
                                            io::CancellationToken const & cancellation)
    {
#if !defined(__EMSCRIPTEN__)
        SseStreamParser parser(std::move(onEvent));
        return performCurlPost(request, &parser, cancellation);
#else
        (void) request;
        (void) onEvent;
        (void) cancellation;
        return HttpResponse{0, "", "Browser fetch SSE transport not yet wired", false};
#endif
    }
} // namespace gladius::agent
