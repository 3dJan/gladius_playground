#pragma once

#include <cstdio>
#include <nlohmann/json.hpp>
#include <string>
#include <unordered_map>
#include <vector>

namespace gladius::agent
{
    /// @brief Wire protocol / API type (distinct from the provider name or host)
    enum class ApiType
    {
        OpenAiCompatible = 0,
        Anthropic = 1
    };

    [[nodiscard]] inline char const * toString(ApiType type) noexcept
    {
        switch (type)
        {
        case ApiType::OpenAiCompatible:
            return "openai";
        case ApiType::Anthropic:
            return "anthropic";
        }
        return "openai";
    }

    [[nodiscard]] inline ApiType apiTypeFromString(std::string const & s) noexcept
    {
        if (s == "anthropic")
        {
            return ApiType::Anthropic;
        }
        return ApiType::OpenAiCompatible;
    }

    [[nodiscard]] inline bool isLocalEndpoint(std::string const & endpoint)
    {
        return endpoint.find("localhost") != std::string::npos
               || endpoint.find("127.0.0.1") != std::string::npos
               || endpoint.find("0.0.0.0") != std::string::npos
               || endpoint.find("[::1]") != std::string::npos
               || endpoint.find(":11434") != std::string::npos
               || endpoint.find(":1234") != std::string::npos
               || endpoint.find(":8080") != std::string::npos;
    }

    [[nodiscard]] inline bool isOpenRouterEndpoint(std::string const & endpoint)
    {
        return endpoint.find("openrouter.ai") != std::string::npos;
    }

    /// @brief Metadata for a discovered or configured LLM model
    struct ModelMetadata
    {
        std::string id;
        std::string name;
        int contextLength = 0;
        double promptPricePerMillion = -1.0;
        double completionPricePerMillion = -1.0;
        bool supportsVision = false;
        bool isFree = false;

        [[nodiscard]] std::string formatPriceSummary() const
        {
            if (isFree || (promptPricePerMillion == 0.0 && completionPricePerMillion == 0.0))
            {
                return "Free";
            }
            if (promptPricePerMillion >= 0.0 && completionPricePerMillion >= 0.0)
            {
                char buf[96];
                std::snprintf(buf,
                              sizeof(buf),
                              "$%.2f in / $%.2f out per 1M",
                              promptPricePerMillion,
                              completionPricePerMillion);
                return buf;
            }
            return "Pricing unknown";
        }

        [[nodiscard]] nlohmann::json toJson() const
        {
            return {
              {"id", id},
              {"name", name},
              {"contextLength", contextLength},
              {"promptPricePerMillion", promptPricePerMillion},
              {"completionPricePerMillion", completionPricePerMillion},
              {"supportsVision", supportsVision},
              {"isFree", isFree}};
        }

        static ModelMetadata fromJson(nlohmann::json const & j)
        {
            ModelMetadata m;
            m.id = j.value("id", "");
            m.name = j.value("name", "");
            m.contextLength = j.value("contextLength", 0);
            m.promptPricePerMillion = j.value("promptPricePerMillion", -1.0);
            m.completionPricePerMillion = j.value("completionPricePerMillion", -1.0);
            m.supportsVision = j.value("supportsVision", false);
            m.isFree = j.value("isFree", false);
            return m;
        }
    };

    /// @brief Named LLM provider profile supporting multiple models
    struct ProviderProfile
    {
        std::string id;
        std::string name;
        ApiType apiType = ApiType::OpenAiCompatible;
        std::string endpoint;
        std::vector<std::string> models;
        std::string apiKey;
        std::unordered_map<std::string, ModelMetadata> modelMetadata;

        /// @brief True if this provider/model can be used right now (has API key, is local, or is OpenRouter free)
        [[nodiscard]] bool isModelAvailable(std::string const & model) const
        {
            if (model.empty())
            {
                return false;
            }
            if (!apiKey.empty() || isLocalEndpoint(endpoint))
            {
                return true;
            }
            if (isOpenRouterEndpoint(endpoint))
            {
                if (model.size() >= 5 && model.substr(model.size() - 5) == ":free")
                {
                    return true;
                }
                auto it = modelMetadata.find(model);
                return it != modelMetadata.end() && it->second.isFree;
            }
            return false;
        }

        [[nodiscard]] nlohmann::json toJson() const
        {
            nlohmann::json metaObj = nlohmann::json::object();
            for (auto const & [mid, meta] : modelMetadata)
            {
                metaObj[mid] = meta.toJson();
            }
            return {
              {"id", id},
              {"name", name},
              {"apiType", toString(apiType)},
              {"endpoint", endpoint},
              {"models", models},
              {"modelMetadata", std::move(metaObj)}};
        }

        static ProviderProfile fromJson(nlohmann::json const & j)
        {
            ProviderProfile p;
            p.id = j.value("id", "");
            p.name = j.value("name", "");
            if (j.contains("apiType"))
            {
                p.apiType = apiTypeFromString(j.value("apiType", "openai"));
            }
            else
            {
                p.apiType = apiTypeFromString(j.value("kind", "openai"));
            }
            p.endpoint = j.value("endpoint", "");
            p.models = j.value("models", std::vector<std::string>{});
            if (j.contains("modelMetadata") && j["modelMetadata"].is_object())
            {
                for (auto const & [k, v] : j["modelMetadata"].items())
                {
                    p.modelMetadata.emplace(k, ModelMetadata::fromJson(v));
                }
            }
            return p;
        }
    };

    /// @brief Identifies a specific (provider profile, model) pair for primary or fallback routing
    struct ModelSelection
    {
        std::string profileId;
        std::string model;

        [[nodiscard]] bool operator==(ModelSelection const & other) const noexcept
        {
            return profileId == other.profileId && model == other.model;
        }

        [[nodiscard]] nlohmann::json toJson() const
        {
            return {{"profileId", profileId}, {"model", model}};
        }

        static ModelSelection fromJson(nlohmann::json const & j)
        {
            return ModelSelection{j.value("profileId", ""), j.value("model", "")};
        }
    };
} // namespace gladius::agent
