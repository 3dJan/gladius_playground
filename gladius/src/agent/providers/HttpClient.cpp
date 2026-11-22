#include "HttpClient.h"

#if !defined(__EMSCRIPTEN__)
#include <curl/curl.h>
#else
#include <coroutine>
#include <emscripten.h>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
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

        HttpResponse performCurlRequest(HttpRequest const & request,
                                        bool isGet,
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
            if (isGet)
            {
                curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
            }
            else
            {
                curl_easy_setopt(curl, CURLOPT_POST, 1L);
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.data());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
            }
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

    HttpResponse CurlHttpTransport::getJson(HttpRequest const & request,
                                            io::CancellationToken const & cancellation)
    {
#if !defined(__EMSCRIPTEN__)
        return performCurlRequest(request, true, nullptr, cancellation);
#else
        (void) request;
        (void) cancellation;
        return HttpResponse{0, "", "Browser fetch transport not yet wired", false};
#endif
    }

    HttpResponse CurlHttpTransport::postJson(HttpRequest const & request,
                                             io::CancellationToken const & cancellation)
    {
#if !defined(__EMSCRIPTEN__)
        return performCurlRequest(request, false, nullptr, cancellation);
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
        return performCurlRequest(request, false, &parser, cancellation);
#else
        (void) request;
        (void) onEvent;
        (void) cancellation;
        return HttpResponse{0, "", "Synchronous fetch is not supported in browser builds", false};
#endif
    }

#if !defined(__EMSCRIPTEN__)
    coro::task<HttpResponse> CurlHttpTransport::getJsonAsync(
      HttpRequest const & request,
      io::CancellationToken const & cancellation)
    {
        co_return getJson(request, cancellation);
    }

    coro::task<HttpResponse> CurlHttpTransport::postJsonAsync(
      HttpRequest const & request,
      io::CancellationToken const & cancellation)
    {
        co_return postJson(request, cancellation);
    }

    coro::task<HttpResponse> CurlHttpTransport::postSseAsync(
      HttpRequest const & request,
      SseEventCallback onEvent,
      io::CancellationToken const & cancellation)
    {
        co_return postSse(request, std::move(onEvent), cancellation);
    }
#else
    namespace
    {
        struct BrowserFetchState
        {
            std::coroutine_handle<> awaiting;
            HttpResponse response;
            std::optional<SseStreamParser> sseParser;
            io::CancellationToken const * cancellation = nullptr;
            bool abandoned = false;
        };

        extern "C"
        {
            EMSCRIPTEN_KEEPALIVE void
            gladiusOnFetchChunk(void * statePtr, char const * data, int len)
            {
                if (statePtr == nullptr || data == nullptr || len <= 0)
                {
                    return;
                }
                auto * stateShared = static_cast<std::shared_ptr<BrowserFetchState> *>(statePtr);
                auto const & state = *stateShared;
                if (!state || state->abandoned)
                {
                    return;
                }
                if (state->sseParser.has_value())
                {
                    state->sseParser->feed(std::string(data, static_cast<std::size_t>(len)));
                }
            }

            EMSCRIPTEN_KEEPALIVE int gladiusIsFetchCancelled(void * statePtr)
            {
                if (statePtr == nullptr)
                {
                    return 1;
                }
                auto * stateShared = static_cast<std::shared_ptr<BrowserFetchState> *>(statePtr);
                auto const & state = *stateShared;
                if (!state || state->abandoned)
                {
                    return 1;
                }
                if (state->cancellation != nullptr && state->cancellation->isCancelled())
                {
                    return 1;
                }
                return 0;
            }

            EMSCRIPTEN_KEEPALIVE void gladiusOnFetchComplete(void * statePtr,
                                                             int statusCode,
                                                             char const * body,
                                                             int bodyLen,
                                                             char const * error,
                                                             int cancelled)
            {
                if (statePtr == nullptr)
                {
                    return;
                }
                std::unique_ptr<std::shared_ptr<BrowserFetchState>> holder(
                  static_cast<std::shared_ptr<BrowserFetchState> *>(statePtr));
                auto state = *holder;
                if (!state || state->abandoned)
                {
                    return;
                }
                if (state->sseParser.has_value())
                {
                    state->sseParser->finish();
                }
                state->response.statusCode = statusCode;
                state->response.cancelled = (cancelled != 0);
                if (body != nullptr && bodyLen > 0)
                {
                    state->response.body.assign(body, static_cast<std::size_t>(bodyLen));
                }
                if (error != nullptr && error[0] != '\0')
                {
                    state->response.error = error;
                }
                if (state->awaiting)
                {
                    auto h = state->awaiting;
                    state->awaiting = {};
                    h.resume();
                }
            }
        } // extern "C"

        EM_JS(void,
              gladiusStartBrowserFetch,
              (void * statePtr,
               char const * method,
               char const * url,
               char const * headersJson,
               char const * body,
               int bodyLen,
               int timeoutMs,
               int isSse),
              {
                  const decoder = new TextDecoder("utf-8");
                  const encoder = new TextEncoder();
                  const readCString = (ptr) => {
                      if (!ptr) return "";
                      let end = ptr;
                      while (HEAPU8[end] !== 0) end++;
                      return decoder.decode(HEAPU8.subarray(ptr, end));
                  };
                  const allocUtf8 = (str) => {
                      const bytes = encoder.encode(str);
                      const ptr = _malloc(bytes.length + 1);
                      HEAPU8.set(bytes, ptr);
                      HEAPU8[ptr + bytes.length] = 0;
                      return { ptr, len: bytes.length };
                  };

                  const methodStr = readCString(method);
                  const urlStr = readCString(url);
                  const headersArr = JSON.parse(readCString(headersJson) || "[]");
                  const hdrs = new Headers();
                  for (const h of headersArr) {
                      const idx = h.indexOf(":");
                      if (idx > 0) {
                          const k = h.slice(0, idx).trim();
                          const v = h.slice(idx + 1).trim();
                          try { hdrs.append(k, v); } catch (e) {}
                      }
                  }

                  const opts = { method: methodStr, headers: hdrs };
                  if (methodStr !== "GET" && body && bodyLen > 0) {
                      opts.body = decoder.decode(HEAPU8.subarray(body, body + bodyLen));
                  }

                  const controller = new AbortController();
                  opts.signal = controller.signal;
                  let timedOut = false;
                  let cancelledByToken = false;
                  const timeoutId = timeoutMs > 0
                      ? setTimeout(() => { timedOut = true; controller.abort(); }, timeoutMs)
                      : 0;
                  const cancelCheckId = setInterval(() => {
                      if (_gladiusIsFetchCancelled(statePtr)) {
                          cancelledByToken = true;
                          controller.abort();
                      }
                  }, 100);

                  let finished = false;
                  const finish = (status, text, err, cancelled) => {
                      if (finished) return;
                      finished = true;
                      if (timeoutId) clearTimeout(timeoutId);
                      clearInterval(cancelCheckId);
                      const b = text ? allocUtf8(text) : { ptr: 0, len: 0 };
                      const e = err ? allocUtf8(err) : { ptr: 0, len: 0 };
                      _gladiusOnFetchComplete(
                          statePtr, status, b.ptr, b.len, e.ptr, cancelled ? 1 : 0);
                      if (b.ptr) _free(b.ptr);
                      if (e.ptr) _free(e.ptr);
                  };

                  fetch(urlStr, opts).then(async (res) => {
                      if (isSse && res.ok && res.body) {
                          const reader = res.body.getReader();
                          const streamDecoder = new TextDecoder("utf-8");
                          while (true) {
                              const { done, value } = await reader.read();
                              if (done) break;
                              if (_gladiusIsFetchCancelled(statePtr)) {
                                  cancelledByToken = true;
                                  await reader.cancel();
                                  finish(res.status, "", "Request cancelled", true);
                                  return;
                              }
                              const chunkText = streamDecoder.decode(value, { stream: true });
                              if (chunkText.length > 0) {
                                  const c = allocUtf8(chunkText);
                                  _gladiusOnFetchChunk(statePtr, c.ptr, c.len);
                                  _free(c.ptr);
                              }
                          }
                          const tail = streamDecoder.decode();
                          if (tail.length > 0) {
                              const c = allocUtf8(tail);
                              _gladiusOnFetchChunk(statePtr, c.ptr, c.len);
                              _free(c.ptr);
                          }
                          finish(res.status, "", "", false);
                      } else {
                          const text = await res.text();
                          finish(res.status, text, "", false);
                      }
                  }).catch((err) => {
                      if (cancelledByToken || _gladiusIsFetchCancelled(statePtr)) {
                          finish(0, "", "Request cancelled", true);
                      } else if (timedOut) {
                          finish(0, "", "Request timed out", false);
                      } else {
                          const msg = (err && err.message) ? err.message : String(err || "Fetch failed");
                          finish(0, "", msg, false);
                      }
                  });
              })

        struct BrowserFetchAwaitable
        {
            std::string method;
            HttpRequest request;
            bool isSse = false;
            std::shared_ptr<BrowserFetchState> state;

            BrowserFetchAwaitable(std::string m,
                                  HttpRequest req,
                                  SseEventCallback onEvent,
                                  bool sse,
                                  io::CancellationToken const & cancellation)
                : method(std::move(m))
                , request(std::move(req))
                , isSse(sse)
                , state(std::make_shared<BrowserFetchState>())
            {
                state->cancellation = &cancellation;
                if (isSse)
                {
                    state->sseParser.emplace(std::move(onEvent));
                }
            }

            ~BrowserFetchAwaitable()
            {
                if (state)
                {
                    state->abandoned = true;
                    state->awaiting = {};
                    state->cancellation = nullptr;
                }
            }

            bool await_ready() const noexcept
            {
                return state->cancellation != nullptr && state->cancellation->isCancelled();
            }

            void await_suspend(std::coroutine_handle<> awaiting)
            {
                state->awaiting = awaiting;
                nlohmann::json hdrs = nlohmann::json::array();
                for (auto const & h : request.headers)
                {
                    hdrs.push_back(h);
                }
                std::string const headersStr = hdrs.dump();
                int const timeoutMs =
                  request.timeoutSeconds > 0 ? request.timeoutSeconds * 1000 : 0;
                auto * rawState = new std::shared_ptr<BrowserFetchState>(state);
                gladiusStartBrowserFetch(rawState,
                                         method.c_str(),
                                         request.url.c_str(),
                                         headersStr.c_str(),
                                         request.body.data(),
                                         static_cast<int>(request.body.size()),
                                         timeoutMs,
                                         isSse ? 1 : 0);
            }

            HttpResponse await_resume()
            {
                if (state->cancellation != nullptr && state->cancellation->isCancelled()
                    && !state->response.cancelled && state->response.statusCode == 0)
                {
                    return HttpResponse{0, "", "Request cancelled", true};
                }
                return std::move(state->response);
            }
        };
    } // namespace

    coro::task<HttpResponse> CurlHttpTransport::getJsonAsync(
      HttpRequest const & request,
      io::CancellationToken const & cancellation)
    {
        co_return co_await BrowserFetchAwaitable{
          "GET", request, nullptr, false, cancellation};
    }

    coro::task<HttpResponse> CurlHttpTransport::postJsonAsync(
      HttpRequest const & request,
      io::CancellationToken const & cancellation)
    {
        co_return co_await BrowserFetchAwaitable{
          "POST", request, nullptr, false, cancellation};
    }

    coro::task<HttpResponse> CurlHttpTransport::postSseAsync(
      HttpRequest const & request,
      SseEventCallback onEvent,
      io::CancellationToken const & cancellation)
    {
        co_return co_await BrowserFetchAwaitable{
          "POST", request, std::move(onEvent), true, cancellation};
    }
#endif
} // namespace gladius::agent
