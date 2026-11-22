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

    ModelDiscoveryResult ModelDiscovery::fetchModels(ProviderProfile const & profile,
                                                     IHttpTransport & transport,
                                                     io::CancellationToken const & cancellation,
                                                     bool freeOnly,
                                                     std::size_t maxFreeModels)
    {
        ModelDiscoveryResult result;
        std::string const base = trimTrailingSlash(profile.endpoint);
        if (base.empty())
        {
            result.error = "Endpoint is empty";
            return result;
        }

        HttpRequest req;
        req.timeoutSeconds = 20;
        req.headers.emplace_back("Accept: application/json");

        if (profile.apiType == ApiType::Anthropic)
        {
            if (profile.apiKey.empty())
            {
                result.error = "API key is required to query Anthropic models";
                return result;
            }
            req.url = base + "/v1/models";
            req.headers.emplace_back("x-api-key: " + profile.apiKey);
            req.headers.emplace_back("anthropic-version: 2023-06-01");
        }
        else
        {
            if (base.size() >= 7 && base.substr(base.size() - 7) == "/models")
            {
                req.url = base;
            }
            else
            {
                req.url = base + "/models";
            }
            if (!profile.apiKey.empty())
            {
                req.headers.emplace_back("Authorization: Bearer " + profile.apiKey);
            }
            if (isOpenRouterEndpoint(base))
            {
                req.headers.emplace_back("HTTP-Referer: https://github.com/3MFConsortium/gladius");
                req.headers.emplace_back("X-Title: Gladius Design Agent");
            }
        }

        HttpResponse const httpRes = transport.getJson(req, cancellation);
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
} // namespace gladius::agent
