#pragma once

#include "AgentBenchTask.h"
#include "agent/providers/IAiProvider.h"
#include "agent/trace/AgentTrace.h"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace gladius::agent::bench
{
    /// @brief Vision-judge verdict for one produced model
    struct JudgeVerdict
    {
        /// @brief False when the judge could not be reached or its answer was unusable
        bool available = false;
        bool passes = false;
        double confidence = 0.0;
        std::string observed;
        std::vector<std::string> issues;
        std::string error;
        TokenUsage usage;

        [[nodiscard]] nlohmann::json toJson() const;
    };

    /// @brief Asks the benchmark model, in a fresh toolless conversation, whether the rendered
    ///        views of the produced model satisfy the task rubric.
    class AgentBenchJudge
    {
      public:
        AgentBenchJudge(std::unique_ptr<IAiProvider> provider, std::uint32_t imageSize)
            : m_provider(std::move(provider))
            , m_imageSize(imageSize)
        {
        }

        [[nodiscard]] JudgeVerdict judge(AgentBenchTask const & task,
                                         std::vector<std::vector<std::byte>> const & views);

        /// @brief Records the judge completion as a `phase: judge` record when set.
        /// @details The judge is measurement cost, not agent cost, so it is recorded as a harness
        ///          phase (with its own usage) instead of a `turn`.
        void setTraceRecorder(agent::trace::AgentTraceRecorder * recorder) noexcept
        {
            m_trace = recorder;
        }

        [[nodiscard]] TokenUsage totalUsage() const noexcept
        {
            return m_usage;
        }

        /// @brief Extracts the first JSON object from a model answer; null json when absent
        [[nodiscard]] static nlohmann::json extractJson(std::string const & answer);

      private:
        std::unique_ptr<IAiProvider> m_provider;
        std::uint32_t m_imageSize = 512;
        TokenUsage m_usage;
        agent::trace::AgentTraceRecorder * m_trace = nullptr;
    };
} // namespace gladius::agent::bench
