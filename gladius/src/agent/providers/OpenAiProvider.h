#pragma once

#include "HttpClient.h"
#include "IAiProvider.h"

#include <memory>

namespace gladius::agent
{
    /// @brief OpenAI and OpenAI-compatible Chat Completions provider (OpenAI, Azure, Ollama, vLLM, OpenRouter)
    class OpenAiProvider final : public IAiProvider
    {
      public:
        explicit OpenAiProvider(ProviderConfig config,
                                std::shared_ptr<IHttpTransport> transport = nullptr);

        [[nodiscard]] std::string name() const override
        {
            return "openai";
        }

        [[nodiscard]] coro::task<CompletionResponse>
        complete(CompletionRequest const & request,
                 TextDeltaCallback onDelta,
                 io::CancellationToken const & cancellation) override;

        /// @brief Build OpenAI Chat Completions JSON payload (exposed for unit testing)
        [[nodiscard]] nlohmann::json buildPayload(CompletionRequest const & request) const;

        /// @brief Parse a non-streaming OpenAI Chat Completions JSON response
        [[nodiscard]] static CompletionResponse parseResponseJson(nlohmann::json const & body);

      private:
        ProviderConfig m_config;
        std::shared_ptr<IHttpTransport> m_transport;
    };
} // namespace gladius::agent
