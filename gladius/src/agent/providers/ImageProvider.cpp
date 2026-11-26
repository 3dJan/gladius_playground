#include "ImageProvider.h"

#include "../Conversation.h"

#include <algorithm>
#include <array>
#include <utility>

namespace gladius::agent
{
    namespace
    {
        std::string resolveImageUrl(std::string endpoint)
        {
            if (endpoint.empty())
            {
                return "https://api.openai.com/v1/images/generations";
            }
            while (!endpoint.empty() && endpoint.back() == '/')
            {
                endpoint.pop_back();
            }
            if (endpoint.size() >= 19 && endpoint.substr(endpoint.size() - 19) == "/images/generations")
            {
                return endpoint;
            }
            return endpoint + "/images/generations";
        }
    } // namespace

    OpenAiImageProvider::OpenAiImageProvider(ProviderConfig config,
                                             std::shared_ptr<IHttpTransport> transport)
        : m_config(std::move(config))
        , m_transport(transport ? std::move(transport) : std::make_shared<CurlHttpTransport>())
    {
    }

    coro::task<ImageGenerationResponse>
    OpenAiImageProvider::generate(ImageGenerationRequest const & request,
                                  io::CancellationToken const & cancellation)
    {
        HttpRequest httpReq;
        httpReq.url = resolveImageUrl(m_config.endpoint);
        httpReq.headers.emplace_back("Content-Type: application/json");
        if (!m_config.apiKey.empty())
        {
            httpReq.headers.emplace_back("Authorization: Bearer " + m_config.apiKey);
        }

        nlohmann::json payload = {
            {"model", m_config.model.empty() ? "gpt-image-1" : m_config.model},
            {"prompt", request.prompt},
            {"size", request.size},
            {"response_format", "b64_json"}
        };
        httpReq.body = payload.dump();

        HttpResponse const httpRes = co_await m_transport->postJsonAsync(httpReq, cancellation);
        if (httpRes.cancelled || !httpRes.error.empty())
        {
            co_return ImageGenerationResponse{false, {}, httpRes.error};
        }
        if (httpRes.statusCode < 200 || httpRes.statusCode >= 300)
        {
            co_return ImageGenerationResponse{
                false, {}, "HTTP " + std::to_string(httpRes.statusCode) + ": " + httpRes.body};
        }

        auto parsed = nlohmann::json::parse(httpRes.body, nullptr, false);
        if (parsed.is_discarded() || !parsed.contains("data") || !parsed["data"].is_array()
            || parsed["data"].empty())
        {
            co_return ImageGenerationResponse{false, {}, "Invalid image generation response"};
        }

        std::string const b64 = parsed["data"][0].value("b64_json", "");
        if (b64.empty())
        {
            co_return ImageGenerationResponse{false, {}, "Missing b64_json in response"};
        }

        ImageGenerationResponse resp;
        resp.success = true;
        resp.image.mimeType = "image/png";
        resp.image.data = decodeBase64(b64);
        co_return resp;
    }

    namespace
    {
        std::pair<int, int> parseSize(std::string const & sizeStr, int defaultW, int defaultH)
        {
            auto const pos = sizeStr.find('x');
            if (pos == std::string::npos)
            {
                return {defaultW, defaultH};
            }
            try
            {
                int const w = std::clamp(std::stoi(sizeStr.substr(0, pos)), 64, 4096);
                int const h = std::clamp(std::stoi(sizeStr.substr(pos + 1)), 64, 4096);
                return {w, h};
            }
            catch (...)
            {
                return {defaultW, defaultH};
            }
        }

        std::string stripDataUrlPrefix(std::string const & b64)
        {
            auto const comma = b64.find("base64,");
            if (comma != std::string::npos)
            {
                return b64.substr(comma + 7);
            }
            return b64;
        }

        std::string resolveSdWebUiUrl(std::string endpoint)
        {
            if (endpoint.empty())
            {
                return "http://127.0.0.1:7860/sdapi/v1/txt2img";
            }
            while (!endpoint.empty() && endpoint.back() == '/')
            {
                endpoint.pop_back();
            }
            std::string const suffix = "/sdapi/v1/txt2img";
            if (endpoint.size() >= suffix.size()
                && endpoint.substr(endpoint.size() - suffix.size()) == suffix)
            {
                return endpoint;
            }
            return endpoint + suffix;
        }

        std::string resolveGeminiImageUrl(std::string endpoint, std::string const & model)
        {
            std::string const effectiveModel = model.empty() ? "imagen-3.0-generate-002" : model;
            if (endpoint.empty())
            {
                return "https://generativelanguage.googleapis.com/v1beta/models/" + effectiveModel + ":predict";
            }
            while (!endpoint.empty() && endpoint.back() == '/')
            {
                endpoint.pop_back();
            }
            if (endpoint.find(":predict") != std::string::npos
                || endpoint.find(":generateContent") != std::string::npos)
            {
                return endpoint;
            }
            return endpoint + "/models/" + effectiveModel + ":predict";
        }
    } // namespace

    SdWebUiImageProvider::SdWebUiImageProvider(ImageProviderConfig config,
                                               std::shared_ptr<IHttpTransport> transport)
        : m_config(std::move(config))
        , m_transport(transport ? std::move(transport) : std::make_shared<CurlHttpTransport>())
    {
    }

    coro::task<ImageGenerationResponse>
    SdWebUiImageProvider::generate(ImageGenerationRequest const & request,
                                   io::CancellationToken const & cancellation)
    {
        auto const [w, h] = parseSize(request.size.empty() ? m_config.defaultSize : request.size, 512, 512);
        HttpRequest httpReq;
        httpReq.url = resolveSdWebUiUrl(m_config.endpoint);
        httpReq.headers.emplace_back("Content-Type: application/json");
        if (!m_config.apiKey.empty())
        {
            httpReq.headers.emplace_back("Authorization: Bearer " + m_config.apiKey);
        }

        nlohmann::json payload = {
            {"prompt", request.prompt},
            {"width", w},
            {"height", h},
            {"steps", 20}
        };
        if (!m_config.model.empty())
        {
            payload["override_settings"] = {{"sd_model_checkpoint", m_config.model}};
        }
        httpReq.body = payload.dump();

        HttpResponse const httpRes = co_await m_transport->postJsonAsync(httpReq, cancellation);
        if (httpRes.cancelled || !httpRes.error.empty())
        {
            co_return ImageGenerationResponse{false, {}, httpRes.error};
        }
        if (httpRes.statusCode < 200 || httpRes.statusCode >= 300)
        {
            co_return ImageGenerationResponse{
                false, {}, "HTTP " + std::to_string(httpRes.statusCode) + ": " + httpRes.body};
        }

        auto parsed = nlohmann::json::parse(httpRes.body, nullptr, false);
        if (parsed.is_discarded() || !parsed.contains("images") || !parsed["images"].is_array()
            || parsed["images"].empty() || !parsed["images"][0].is_string())
        {
            co_return ImageGenerationResponse{false, {}, "Invalid SD WebUI image response"};
        }

        std::string const b64 = stripDataUrlPrefix(parsed["images"][0].get<std::string>());
        if (b64.empty())
        {
            co_return ImageGenerationResponse{false, {}, "Empty image payload in SD WebUI response"};
        }

        ImageGenerationResponse resp;
        resp.success = true;
        resp.image.mimeType = "image/png";
        resp.image.data = decodeBase64(b64);
        co_return resp;
    }

    GeminiImageProvider::GeminiImageProvider(ImageProviderConfig config,
                                             std::shared_ptr<IHttpTransport> transport)
        : m_config(std::move(config))
        , m_transport(transport ? std::move(transport) : std::make_shared<CurlHttpTransport>())
    {
    }

    coro::task<ImageGenerationResponse>
    GeminiImageProvider::generate(ImageGenerationRequest const & request,
                                  io::CancellationToken const & cancellation)
    {
        HttpRequest httpReq;
        httpReq.url = resolveGeminiImageUrl(m_config.endpoint, m_config.model);
        httpReq.headers.emplace_back("Content-Type: application/json");
        if (!m_config.apiKey.empty())
        {
            httpReq.headers.emplace_back("x-goog-api-key: " + m_config.apiKey);
        }

        nlohmann::json payload;
        if (httpReq.url.find(":generateContent") != std::string::npos)
        {
            payload = {
                {"contents", {{{"parts", {{{"text", request.prompt}}}}}}},
                {"generationConfig", {{"responseModalities", {"IMAGE"}}}}
            };
        }
        else
        {
            payload = {
                {"instances", {{{"prompt", request.prompt}}}},
                {"parameters", {{"sampleCount", 1}}}
            };
        }
        httpReq.body = payload.dump();

        HttpResponse const httpRes = co_await m_transport->postJsonAsync(httpReq, cancellation);
        if (httpRes.cancelled || !httpRes.error.empty())
        {
            co_return ImageGenerationResponse{false, {}, httpRes.error};
        }
        if (httpRes.statusCode < 200 || httpRes.statusCode >= 300)
        {
            co_return ImageGenerationResponse{
                false, {}, "HTTP " + std::to_string(httpRes.statusCode) + ": " + httpRes.body};
        }

        auto parsed = nlohmann::json::parse(httpRes.body, nullptr, false);
        if (parsed.is_discarded())
        {
            co_return ImageGenerationResponse{false, {}, "Invalid Gemini image response"};
        }

        std::string b64;
        std::string mime = "image/png";
        if (parsed.contains("predictions") && parsed["predictions"].is_array()
            && !parsed["predictions"].empty())
        {
            b64 = parsed["predictions"][0].value("bytesBase64Encoded", "");
            mime = parsed["predictions"][0].value("mimeType", "image/png");
        }
        else if (parsed.contains("candidates") && parsed["candidates"].is_array()
                 && !parsed["candidates"].empty())
        {
            auto const & cand = parsed["candidates"][0];
            if (cand.contains("content") && cand["content"].contains("parts")
                && cand["content"]["parts"].is_array())
            {
                for (auto const & part : cand["content"]["parts"])
                {
                    if (part.contains("inlineData") && part["inlineData"].is_object())
                    {
                        b64 = part["inlineData"].value("data", "");
                        mime = part["inlineData"].value("mimeType", "image/png");
                        if (!b64.empty())
                        {
                            break;
                        }
                    }
                }
            }
        }

        if (b64.empty())
        {
            co_return ImageGenerationResponse{false, {}, "Missing image data in Gemini response"};
        }

        ImageGenerationResponse resp;
        resp.success = true;
        resp.image.mimeType = mime;
        resp.image.data = decodeBase64(b64);
        co_return resp;
    }

    std::unique_ptr<IImageProvider> createImageProvider(ImageProviderConfig const & config,
                                                        std::shared_ptr<IHttpTransport> transport)
    {
        switch (config.apiType)
        {
        case ImageApiType::SdWebUi:
            return std::make_unique<SdWebUiImageProvider>(config, std::move(transport));
        case ImageApiType::GeminiImagen:
            return std::make_unique<GeminiImageProvider>(config, std::move(transport));
        case ImageApiType::OpenAiCompatible:
        default:
        {
            ProviderConfig cfg;
            cfg.endpoint = config.endpoint;
            cfg.apiKey = config.apiKey;
            cfg.model = config.model.empty() ? "gpt-image-1" : config.model;
            return std::make_unique<OpenAiImageProvider>(std::move(cfg), std::move(transport));
        }
        }
    }
} // namespace gladius::agent
