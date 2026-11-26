#pragma once

#include "HttpClient.h"
#include "ProviderError.h"
#include "ProviderProfile.h"

#include <coro/coro.hpp>
#include <string>
#include <vector>

namespace gladius::agent
{
    /// @brief Result of querying available models from a provider's API
    struct ModelDiscoveryResult
    {
        bool success = false;
        std::vector<std::string> models;
        std::vector<ModelMetadata> metadata;
        /// @brief Raw provider text, kept for diagnostics
        std::string error;
        /// @brief The same failure in terms the user can act on
        ProviderError errorInfo;
    };

    /// @brief Queries available models from OpenRouter, OpenAI-compatible, Ollama, or Anthropic endpoints
    class ModelDiscovery
    {
      public:
        /// @brief Fetch available model IDs and metadata for the given profile.
        /// When freeOnly is true (or OpenRouter without an API key), returns only free models.
        [[nodiscard]] static ModelDiscoveryResult fetchModels(
          ProviderProfile const & profile,
          IHttpTransport & transport,
          io::CancellationToken const & cancellation,
          bool freeOnly = false,
          std::size_t maxFreeModels = 50);

        /// @brief Coroutine variant of fetchModels for non-blocking browser execution.
        [[nodiscard]] static coro::task<ModelDiscoveryResult> fetchModelsAsync(
          ProviderProfile const & profile,
          IHttpTransport & transport,
          io::CancellationToken const & cancellation,
          bool freeOnly = false,
          std::size_t maxFreeModels = 50);
    };
} // namespace gladius::agent
