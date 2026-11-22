#pragma once

#include "IAiProvider.h"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace gladius::agent
{
    /// @brief Composite IAiProvider that tries a primary provider and falls back sequentially on errors
    class FallbackProvider final : public IAiProvider
    {
      public:
        struct Candidate
        {
            std::string label;
            std::unique_ptr<IAiProvider> provider;
        };

        explicit FallbackProvider(std::vector<Candidate> candidates);

        [[nodiscard]] std::string name() const override;

        [[nodiscard]] coro::task<CompletionResponse>
        complete(CompletionRequest const & request,
                 TextDeltaCallback onDelta,
                 io::CancellationToken const & cancellation) override;

      private:
        std::vector<Candidate> m_candidates;
    };
} // namespace gladius::agent
