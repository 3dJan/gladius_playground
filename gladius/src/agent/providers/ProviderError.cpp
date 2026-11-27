#include "ProviderError.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>
#include <string>

namespace gladius::agent
{
    namespace
    {
        /// @brief Diagnostics keep the provider text, but a vendor body can be many kilobytes
        constexpr std::size_t kMaxDetailChars = 240;

        [[nodiscard]] std::string toLower(std::string text)
        {
            std::transform(text.begin(),
                           text.end(),
                           text.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return text;
        }

        [[nodiscard]] bool containsAny(std::string const & lowerText,
                                       std::initializer_list<char const *> needles)
        {
            for (auto const * needle : needles)
            {
                if (lowerText.find(needle) != std::string::npos)
                {
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] std::string trimDetail(std::string const & text)
        {
            std::size_t const start = text.find_first_not_of(" \t\r\n");
            if (start == std::string::npos)
            {
                return {};
            }
            std::string const body = text.substr(start);
            if (body.size() <= kMaxDetailChars)
            {
                return body;
            }
            return body.substr(0, kMaxDetailChars) + "...";
        }

        /// @brief Recovers a status code from text that already reads "HTTP 401: ..."
        [[nodiscard]] long extractStatusCode(std::string const & lowerText)
        {
            std::size_t pos = lowerText.find("http ");
            while (pos != std::string::npos)
            {
                std::size_t const digits = pos + 5;
                if (digits < lowerText.size() && std::isdigit(static_cast<unsigned char>(lowerText[digits])))
                {
                    return std::stol(lowerText.substr(digits, 3));
                }
                pos = lowerText.find("http ", pos + 5);
            }
            return 0;
        }

        [[nodiscard]] std::string labelOr(std::string const & label, char const * fallback)
        {
            return label.empty() ? std::string(fallback) : label;
        }

        [[nodiscard]] ProviderError
        make(ProviderErrorKind kind, std::string message, char const * hint, std::string detail)
        {
            return ProviderError{kind, message, hint, trimDetail(detail)};
        }

        [[nodiscard]] ProviderError classifyText(std::string const & lowerText,
                                                 bool apiKeyEmpty,
                                                 std::string const & label,
                                                 std::string const & detail)
        {
            if (containsAny(lowerText,
                            {"invalid api key",
                             "invalid_api_key",
                             "incorrect api key",
                             "authentication",
                             "unauthorized",
                             "forbidden",
                             "invalid x-api-key"}))
            {
                return make(apiKeyEmpty ? ProviderErrorKind::MissingApiKey
                                        : ProviderErrorKind::InvalidApiKey,
                            apiKeyEmpty ? "No API key is set for the selected provider."
                                        : "The provider rejected the API key.",
                            "Open Settings and paste a current key for this provider.",
                            detail);
            }
            if (containsAny(lowerText,
                            {"insufficient",
                             "quota",
                             "credit",
                             "billing",
                             "payment required",
                             "balance"}))
            {
                return make(ProviderErrorKind::QuotaExceeded,
                            "The account behind this key has no remaining credit.",
                            "Top up the account or pick another provider in the composer.",
                            detail);
            }
            if (containsAny(lowerText, {"rate limit", "too many requests", "slow down", "429"}))
            {
                return make(ProviderErrorKind::RateLimited,
                            "The provider is rate limiting requests.",
                            "Wait a moment, then send the prompt again.",
                            detail);
            }
            if (containsAny(lowerText,
                            {"model_not_found",
                             "model not found",
                             "does not exist",
                             "is not a valid model",
                             "no such model",
                             "unknown model"}))
            {
                return make(ProviderErrorKind::ModelNotFound,
                            "The selected model is not available at this provider.",
                            "Fetch the model list in Settings and pick one that exists.",
                            detail);
            }
            if (containsAny(lowerText,
                            {"timed out",
                             "timeout",
                             "operation timed out",
                             "deadline exceeded"}))
            {
                return make(ProviderErrorKind::Timeout,
                            labelOr(label, "The provider") + " did not answer in time.",
                            "Try again, or choose a smaller model while iterating.",
                            detail);
            }
            if (containsAny(lowerText,
                            {"could not resolve",
                             "couldn't resolve",
                             "name resolution",
                             "connection refused",
                             "could not connect",
                             "couldn't connect",
                             "connection reset",
                             "network is unreachable",
                             "ssl",
                             "tls",
                             "certificate",
                             "curl",
                             "unsupported protocol",
                             "initialize curl"}))
            {
                return make(ProviderErrorKind::Network,
                            "The provider endpoint could not be reached.",
                            "Check the endpoint URL, then your network, VPN or proxy settings.",
                            detail);
            }
            if (containsAny(lowerText,
                            {"invalid json",
                             "malformed",
                             "missing choices",
                             "missing 'data'",
                             "unexpected",
                             "empty response",
                             "parse error"}))
            {
                return make(ProviderErrorKind::MalformedResponse,
                            "The provider answered with something unexpected.",
                            "Try again; if it keeps happening, check the endpoint is the right "
                            "chat-completions URL.",
                            detail);
            }
            if (containsAny(lowerText,
                            {"no ai providers configured",
                             "no provider",
                             "endpoint is empty",
                             "api key is required",
                             "no model selected",
                             "model is required"}))
            {
                return make(ProviderErrorKind::NotConfigured,
                            "No usable provider or model is configured.",
                            "Open Settings, add a provider, then pick a model in the composer.",
                            detail);
            }
            if (containsAny(lowerText, {"cancelled", "canceled"}))
            {
                return make(ProviderErrorKind::Cancelled, "The run was cancelled.", "", detail);
            }
            return {};
        }

        [[nodiscard]] ProviderError classifyByStatus(long statusCode,
                                                     bool apiKeyEmpty,
                                                     std::string const & label,
                                                     std::string const & detail,
                                                     std::string const & lowerText)
        {
            if (auto classified = classifyText(lowerText, apiKeyEmpty, label, detail);
                classified.isSet())
            {
                return classified;
            }

            switch (statusCode)
            {
            case 401:
            case 403:
                return make(apiKeyEmpty ? ProviderErrorKind::MissingApiKey
                                        : ProviderErrorKind::InvalidApiKey,
                            apiKeyEmpty ? "No API key is set for the selected provider."
                                        : "The provider rejected the API key.",
                            "Open Settings and paste a current key for this provider.",
                            detail);
            case 402:
                return make(ProviderErrorKind::QuotaExceeded,
                            "The account behind this key has no remaining credit.",
                            "Top up the account or pick another provider in the composer.",
                            detail);
            case 404:
                return make(ProviderErrorKind::ModelNotFound,
                            "The model or endpoint was not found at this provider.",
                            "Check the endpoint URL and fetch the model list in Settings.",
                            detail);
            case 429:
                return make(ProviderErrorKind::RateLimited,
                            "The provider is rate limiting requests.",
                            "Wait a moment, then send the prompt again.",
                            detail);
            default:
                break;
            }

            if (statusCode >= 500)
            {
                return make(ProviderErrorKind::Network,
                            labelOr(label, "The provider") + " reported a server error.",
                            "The provider is likely down; try again or switch provider.",
                            detail);
            }
            if (statusCode != 0)
            {
                std::string const message
                  = "The provider rejected the request (HTTP " + std::to_string(statusCode) + ").";
                return make(ProviderErrorKind::Unknown,
                            message.c_str(),
                            "See Developer diagnostics for the provider response.",
                            detail);
            }
            return {};
        }
    } // namespace

    std::string trimProviderBody(std::string const & body)
    {
        return trimDetail(body);
    }

    char const * toString(ProviderErrorKind kind) noexcept
    {
        switch (kind)
        {
        case ProviderErrorKind::None:
            return "none";
        case ProviderErrorKind::MissingApiKey:
            return "missing-api-key";
        case ProviderErrorKind::InvalidApiKey:
            return "invalid-api-key";
        case ProviderErrorKind::ModelNotFound:
            return "model-not-found";
        case ProviderErrorKind::RateLimited:
            return "rate-limited";
        case ProviderErrorKind::QuotaExceeded:
            return "quota-exceeded";
        case ProviderErrorKind::Network:
            return "network";
        case ProviderErrorKind::Timeout:
            return "timeout";
        case ProviderErrorKind::MalformedResponse:
            return "malformed-response";
        case ProviderErrorKind::NotConfigured:
            return "not-configured";
        case ProviderErrorKind::Cancelled:
            return "cancelled";
        case ProviderErrorKind::Unknown:
            return "unknown";
        }
        return "unknown";
    }

    ProviderError classifyHttpError(long statusCode,
                                    std::string const & providerText,
                                    bool apiKeyEmpty,
                                    std::string const & providerLabel)
    {
        std::string const lowerText = toLower(providerText);
        if (auto classified = classifyByStatus(
              statusCode, apiKeyEmpty, providerLabel, providerText, lowerText);
            classified.isSet())
        {
            return classified;
        }
        if (lowerText.empty())
        {
            return make(ProviderErrorKind::Network,
                        labelOr(providerLabel, "The provider") + " could not be reached.",
                        "Check the endpoint URL and your network connection.",
                        providerText);
        }
        return make(ProviderErrorKind::Unknown,
                    "The provider call failed.",
                    "See Developer diagnostics for the provider response.",
                    providerText);
    }

    ProviderError classifyErrorText(std::string const & errorText,
                                    bool apiKeyEmpty,
                                    bool cancelled,
                                    std::string const & providerLabel)
    {
        if (cancelled)
        {
            return make(ProviderErrorKind::Cancelled, "The run was cancelled.", "", errorText);
        }
        std::string const lowerText = toLower(errorText);
        if (auto classified = classifyText(lowerText, apiKeyEmpty, providerLabel, errorText);
            classified.isSet())
        {
            return classified;
        }
        long const statusCode = extractStatusCode(lowerText);
        if (auto classified = classifyByStatus(
              statusCode, apiKeyEmpty, providerLabel, errorText, lowerText);
            classified.isSet())
        {
            return classified;
        }
        if (errorText.empty())
        {
            return make(ProviderErrorKind::Unknown,
                        labelOr(providerLabel, "The provider") + " failed without a message.",
                        "See Developer diagnostics for details.",
                        errorText);
        }
        return make(ProviderErrorKind::Unknown,
                    "The provider call failed.",
                    "See Developer diagnostics for the provider response.",
                    errorText);
    }

    ProviderError describeEmptyCompletion(std::string const & stopReason,
                                          std::string const & providerLabel)
    {
        bool const truncated = stopReason == "max_tokens" || stopReason == "length";
        return make(ProviderErrorKind::MalformedResponse,
                    labelOr(providerLabel, "The model")
                      + (truncated ? " stopped at its output limit without an answer."
                                   : " answered with nothing."),
                    truncated
                      ? "Raise the agent's max output tokens in Settings, or ask for a smaller step."
                      : "Try again; if it keeps happening, pick another model in Settings.",
                    stopReason);
    }
} // namespace gladius::agent
