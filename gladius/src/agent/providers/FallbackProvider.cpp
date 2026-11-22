#include "FallbackProvider.h"

namespace gladius::agent
{
    FallbackProvider::FallbackProvider(std::vector<Candidate> candidates)
        : m_candidates(std::move(candidates))
    {
    }

    std::string FallbackProvider::name() const
    {
        if (m_candidates.empty())
        {
            return "fallback(empty)";
        }
        return m_candidates.front().label;
    }

    coro::task<CompletionResponse>
    FallbackProvider::complete(CompletionRequest const & request,
                               TextDeltaCallback onDelta,
                               io::CancellationToken const & cancellation)
    {
        if (m_candidates.empty())
        {
            co_return CompletionResponse{{}, {}, "", false, "No AI providers configured"};
        }

        std::string combinedErrors;
        for (std::size_t i = 0; i < m_candidates.size(); ++i)
        {
            auto & cand = m_candidates[i];
            if (!cand.provider)
            {
                continue;
            }
            if (cancellation.isCancelled())
            {
                co_return CompletionResponse{{}, {}, "", false, "Cancelled"};
            }

            std::string bufferedDelta;
            bool emittedDelta = false;
            TextDeltaCallback guardedDelta = [&](std::string const & delta) {
                emittedDelta = true;
                bufferedDelta += delta;
                if (onDelta)
                {
                    onDelta(delta);
                }
            };

            CompletionResponse resp = co_await cand.provider->complete(request, guardedDelta, cancellation);
            if (resp.success || cancellation.isCancelled())
            {
                co_return resp;
            }

            if (!combinedErrors.empty())
            {
                combinedErrors += " | ";
            }
            combinedErrors += cand.label + ": " + resp.error;

            if (emittedDelta && onDelta && i + 1 < m_candidates.size())
            {
                onDelta("\n[Provider failed, switching to fallback...]\n");
            }
        }

        co_return CompletionResponse{{}, {}, "", false, combinedErrors};
    }
} // namespace gladius::agent
