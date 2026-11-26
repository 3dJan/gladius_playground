#pragma once

#include <string>

namespace gladius::agent
{
    /// @brief Why a provider call failed, in terms the user can act on
    enum class ProviderErrorKind
    {
        None,
        MissingApiKey,
        InvalidApiKey,
        ModelNotFound,
        RateLimited,
        QuotaExceeded,
        Network,
        Timeout,
        MalformedResponse,
        NotConfigured,
        Cancelled,
        Unknown
    };

    /**
     * @brief A classified provider failure.
     * @details Providers hand back raw transport strings and vendor JSON, which is useless in a
     *          design tool. The message and hint are what the panel shows; the detail keeps the
     *          original text for the developer diagnostics section so nothing is lost.
     */
    struct ProviderError
    {
        ProviderErrorKind kind = ProviderErrorKind::None;
        /// @brief One short line for the status line
        std::string message;
        /// @brief What the user can do about it
        std::string hint;
        /// @brief Truncated original provider text, diagnostics only
        std::string detail;

        [[nodiscard]] bool isSet() const noexcept
        {
            return kind != ProviderErrorKind::None;
        }

        /// @brief True when opening the provider settings can plausibly fix the problem
        [[nodiscard]] bool isSettingsFixable() const noexcept
        {
            return kind == ProviderErrorKind::MissingApiKey
                || kind == ProviderErrorKind::InvalidApiKey
                || kind == ProviderErrorKind::ModelNotFound
                || kind == ProviderErrorKind::NotConfigured
                || kind == ProviderErrorKind::Network;
        }

        /// @brief Message and hint on one line
        [[nodiscard]] std::string summarised() const
        {
            if (hint.empty())
            {
                return message;
            }
            return message + (message.empty() ? "" : " ") + hint;
        }
    };

    /// @brief Name of the kind, for trace records and tests
    [[nodiscard]] char const * toString(ProviderErrorKind kind) noexcept;

    /// @brief Keeps a vendor error body short enough to show in a log line
    [[nodiscard]] std::string trimProviderBody(std::string const & body);

    /**
     * @brief Classifies an HTTP failure when the caller still has the status code.
     * @param statusCode HTTP status (0 when the request never completed)
     * @param providerText Provider error body or transport error string
     * @param apiKeyEmpty True when the profile carries no key at all
     * @param providerLabel Provider/model label used in the message, may be empty
     */
    [[nodiscard]] ProviderError classifyHttpError(long statusCode,
                                                  std::string const & providerText,
                                                  bool apiKeyEmpty,
                                                  std::string const & providerLabel = {});

    /**
     * @brief Classifies an error string that may already carry an "HTTP <code>" prefix.
     * @details A run reports one joined string that can hold several providers, so the classifier
     *          has to recover the status from the text itself.
     */
    [[nodiscard]] ProviderError classifyErrorText(std::string const & errorText,
                                                  bool apiKeyEmpty,
                                                  bool cancelled = false,
                                                  std::string const & providerLabel = {});
} // namespace gladius::agent
