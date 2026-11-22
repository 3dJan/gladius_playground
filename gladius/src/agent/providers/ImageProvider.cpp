#include "ImageProvider.h"

#include "../Conversation.h"

#include <array>

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
} // namespace gladius::agent
