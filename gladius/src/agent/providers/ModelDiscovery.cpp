#include "ModelDiscovery.h"

#include <algorithm>
#include <nlohmann/json.hpp>

namespace gladius::agent
{
    namespace
    {
        std::string trimTrailingSlash(std::string url)
        {
            while (!url.empty() && url.back() == '/')
            {
                url.pop_back();
            }
            return url;
        }

        double parsePricePerMillion(nlohmann::json const & val)
        {
            try
            {
                double perToken = -1.0;
                if (val.is_string())
                {
                    std::string const s = val.get<std::string>();
                    if (!s.empty())
                    {
                        perToken = std::stod(s);
                    }
                }
                else if (val.is_number())
                {
                    perToken = val.get<double>();
                }
                if (perToken >= 0.0)
                {
                    return perToken * 1000000.0;
                }
            }
            catch (...)
            {
            }
            return -1.0;
        }

        ModelMetadata parseModelMetadata(nlohmann::json const & entry, bool localEndpoint)
        {
            ModelMetadata meta;
            meta.id = entry.value("id", "");
            meta.name = entry.value("name", meta.id);
            meta.contextLength = entry.value("context_length", 0);
            if (meta.contextLength <= 0)
            {
                meta.contextLength = entry.value("max_context_length", 0);
            }

            if (entry.contains("pricing") && entry["pricing"].is_object())
            {
                auto const & p = entry["pricing"];
                if (p.contains("prompt"))
                {
                    meta.promptPricePerMillion = parsePricePerMillion(p["prompt"]);
                }
                if (p.contains("completion"))
                {
                    meta.completionPricePerMillion = parsePricePerMillion(p["completion"]);
                }
            }

            if (entry.contains("architecture") && entry["architecture"].is_object())
            {
                auto const & arch = entry["architecture"];
                if (arch.contains("input_modalities") && arch["input_modalities"].is_array())
                {
                    for (auto const & mod : arch["input_modalities"])
                    {
                        if (mod.is_string() && mod.get<std::string>() == "image")
                        {
                            meta.supportsVision = true;
                            break;
                        }
                    }
                }
                std::string const modality = arch.value("modality", "");
                if (modality.find("image") != std::string::npos)
                {
                    meta.supportsVision = true;
                }
            }

            bool const hasFreeSuffix =
              meta.id.size() >= 5 && meta.id.substr(meta.id.size() - 5) == ":free";
            bool const zeroPricing =
              (meta.promptPricePerMillion == 0.0 && meta.completionPricePerMillion == 0.0);
            meta.isFree = hasFreeSuffix || zeroPricing || localEndpoint;
            if (localEndpoint && meta.promptPricePerMillion < 0.0)
            {
                meta.promptPricePerMillion = 0.0;
                meta.completionPricePerMillion = 0.0;
            }
            return meta;
        }
    } // namespace

    namespace
    {
        bool buildModelsRequest(ProviderProfile const & profile,
                                HttpRequest & outReq,
                                std::string & outBase,
                                std::string & outError)
        {
            outBase = trimTrailingSlash(profile.endpoint);
            if (outBase.empty())
            {
                outError = "Endpoint is empty";
                return false;
            }

            outReq.timeoutSeconds = 20;
            outReq.headers.emplace_back("Accept: application/json");

            if (profile.apiType == ApiType::Anthropic)
            {
                if (profile.apiKey.empty())
                {
                    outError = "API key is required to query Anthropic models";
                    return false;
                }
                outReq.url = outBase + "/v1/models";
                outReq.headers.emplace_back("x-api-key: " + profile.apiKey);
                outReq.headers.emplace_back("anthropic-version: 2023-06-01");
            }
            else
            {
                if (outBase.size() >= 7 && outBase.substr(outBase.size() - 7) == "/models")
                {
                    outReq.url = outBase;
                }
                else
                {
                    outReq.url = outBase + "/models";
                }
                if (!profile.apiKey.empty())
                {
                    outReq.headers.emplace_back("Authorization: Bearer " + profile.apiKey);
                }
                if (isOpenRouterEndpoint(outBase))
                {
                    outReq.headers.emplace_back("HTTP-Referer: https://github.com/3MFConsortium/gladius");
                    outReq.headers.emplace_back("X-Title: Gladius Design Agent");
                }
            }
            return true;
        }

        ModelDiscoveryResult parseModelsResponse(ProviderProfile const & profile,
                                                 std::string const & base,
                                                 HttpResponse const & httpRes,
                                                 bool freeOnly,
                                                 std::size_t maxFreeModels)
        {
            ModelDiscoveryResult result;
            if (httpRes.cancelled)
            {
                result.error = "Cancelled";
                return result;
            }
            if (!httpRes.error.empty())
            {
                result.error = httpRes.error;
                return result;
            }
            if (httpRes.statusCode < 200 || httpRes.statusCode >= 300)
            {
                result.error = "HTTP " + std::to_string(httpRes.statusCode);
                return result;
            }

            auto parsed = nlohmann::json::parse(httpRes.body, nullptr, false);
            if (parsed.is_discarded() || !parsed.is_object())
            {
                result.error = "Invalid JSON response from models endpoint";
                return result;
            }

            if (!parsed.contains("data") || !parsed["data"].is_array())
            {
                result.error = "Response missing 'data' array";
                return result;
            }

            bool const localEndpoint = isLocalEndpoint(base);
            bool const filterFreeOnly =
              freeOnly || (isOpenRouterEndpoint(base) && profile.apiKey.empty());
            for (auto const & entry : parsed["data"])
            {
                if (!entry.is_object())
                {
                    continue;
                }
                ModelMetadata meta = parseModelMetadata(entry, localEndpoint);
                if (meta.id.empty())
                {
                    continue;
                }
                if (filterFreeOnly && !meta.isFree)
                {
                    continue;
                }
                result.models.push_back(meta.id);
                result.metadata.push_back(std::move(meta));
                if (filterFreeOnly && result.models.size() >= maxFreeModels)
                {
                    break;
                }
            }

            result.success = true;
            return result;
        }
    } // namespace

    ModelDiscoveryResult ModelDiscovery::fetchModels(ProviderProfile const & profile,
                                                     IHttpTransport & transport,
                                                     io::CancellationToken const & cancellation,
                                                     bool freeOnly,
                                                     std::size_t maxFreeModels)
    {
        HttpRequest req;
        std::string base;
        std::string err;
        if (!buildModelsRequest(profile, req, base, err))
        {
            ModelDiscoveryResult result;
            result.error = std::move(err);
            return result;
        }
        HttpResponse const httpRes = transport.getJson(req, cancellation);
        return parseModelsResponse(profile, base, httpRes, freeOnly, maxFreeModels);
    }

    coro::task<ModelDiscoveryResult> ModelDiscovery::fetchModelsAsync(
      ProviderProfile const & profile,
      IHttpTransport & transport,
      io::CancellationToken const & cancellation,
      bool freeOnly,
      std::size_t maxFreeModels)
    {
        HttpRequest req;
        std::string base;
        std::string err;
        if (!buildModelsRequest(profile, req, base, err))
        {
            ModelDiscoveryResult result;
            result.error = std::move(err);
            co_return result;
        }
        HttpResponse const httpRes = co_await transport.getJsonAsync(req, cancellation);
        co_return parseModelsResponse(profile, base, httpRes, freeOnly, maxFreeModels);
    }
} // namespace gladius::agent
