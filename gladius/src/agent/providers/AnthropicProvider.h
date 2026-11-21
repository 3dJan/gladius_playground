#pragma once

#include "HttpClient.h"
#include "IAiProvider.h"

#include <memory>

namespace gladius::agent
{
    /// @brief Anthropic Messages API provider (supporting streaming, tool_use, and vision)
    class AnthropicProvider final : public IAiProvider
    {
      public:
        explicit AnthropicProvider(ProviderConfig config,
                                   std::shared_ptr<IHttpTransport> transport = nullptr);

        [[nodiscard]] std::string name() const override
        {
            return "anthropic";
        }

        [[nodiscard]] coro::task<CompletionResponse>
        complete(CompletionRequest const & request,
                 TextDeltaCallback onDelta,
                 io::CancellationToken const & cancellation) override;

        /// @brief Build Anthropic Messages JSON payload (exposed for unit testing)
        [[nodiscard]] nlohmann::json buildPayload(CompletionRequest const & request) const;

        /// @brief Parse a non-streaming Anthropic Messages JSON response
        [[nodiscard]] static CompletionResponse parseResponseJson(nlohmann::json const & body);

      private:
        ProviderConfig m_config;
        std::shared_ptr<IHttpTransport> m_transport;
    };
} // namespace gladius::agent
