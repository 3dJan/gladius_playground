#pragma once

#include "AgentBenchGeometry.h"
#include "AgentBenchJudge.h"
#include "AgentBenchMetrics.h"
#include "AgentBenchTask.h"
#include "HeadlessAgentHost.h"

#include <chrono>
#include <filesystem>
#include <functional>
#include <memory>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace gladius::agent::bench
{
    /// @brief Everything the benchmark learned about one task run
    struct BenchRunRecord
    {
        AgentBenchTask task;
        AgentBenchMetrics metrics;
        GeometryFacts geometry;
        std::vector<GeometryCheck> geometryChecks;
        JudgeVerdict judge;
        std::vector<std::string> artifacts;

        [[nodiscard]] bool geometryPassed() const noexcept
        {
            return bench::geometryPassed(geometryChecks);
        }

        /// @brief Task success requires both the vision judge and the deterministic geometry gates
        [[nodiscard]] bool taskPassed() const noexcept
        {
            return judge.available && judge.passes && geometryPassed();
        }

        [[nodiscard]] nlohmann::json toJson() const;
    };

    /// @brief Accumulates run records and writes the JSON report consumed by the comparer script
    class AgentBenchReport
    {
      public:
        AgentBenchReport(BenchRunConfig const & config, std::filesystem::path outputDir);

        /// @brief Runs one task end-to-end: agent, geometry gates, vision judge, artifacts
        [[nodiscard]] BenchRunRecord runTask(HeadlessAgentHost & host,
                                             AgentBenchTask const & task,
                                             std::unique_ptr<IAiProvider> agentProvider,
                                             std::unique_ptr<IAiProvider> judgeProvider = nullptr);

        void add(BenchRunRecord record);

        [[nodiscard]] std::vector<BenchRunRecord> const & records() const noexcept
        {
            return m_records;
        }

        [[nodiscard]] std::size_t passedCount() const;

        [[nodiscard]] std::size_t
        countIf(std::function<bool(BenchRunRecord const &)> const & predicate) const;

        /// @brief Writes the report JSON into the output dir and returns its path
        [[nodiscard]] std::filesystem::path write(std::string const & fileName = "results.json") const;

        [[nodiscard]] nlohmann::json toJson() const;
        [[nodiscard]] std::filesystem::path const & outputDir() const noexcept
        {
            return m_outputDir;
        }

      private:
        [[nodiscard]] nlohmann::json runMetadata() const;

        BenchRunConfig m_config;
        std::filesystem::path m_outputDir;
        std::vector<BenchRunRecord> m_records;
        std::chrono::steady_clock::time_point m_startTime = std::chrono::steady_clock::now();
    };

    /// @brief Output directory from `GLADIUS_AGENT_BENCH_OUTPUT`, relative to the test working dir
    [[nodiscard]] std::filesystem::path benchOutputDir();

    /**
     * @brief Process-wide benchmark session shared by the per-task tests.
     *
     * Each per-task test runs one task against the same initialized host and appends its record to
     * the shared report, which the aggregate test then writes once.
     */
    class BenchSession
    {
      public:
        [[nodiscard]] static BenchSession & instance();

        /// @brief Creates the host once; returns the reason when the runtime is unavailable
        [[nodiscard]] BenchAvailability ensureHost();

        /// @brief Runs one task end-to-end with the configured provider and records the outcome
        [[nodiscard]] BenchRunRecord runTask(AgentBenchTask const & task);

        [[nodiscard]] AgentBenchReport & report()
        {
            return *m_report;
        }

        [[nodiscard]] BenchRunConfig const & config() const noexcept
        {
            return m_config;
        }

      private:
        BenchSession();

        BenchRunConfig m_config;
        std::filesystem::path m_outputDir;
        std::optional<UserLibrarySandbox> m_librarySandbox;
        std::unique_ptr<HeadlessAgentHost> m_host;
        std::unique_ptr<AgentBenchReport> m_report;
        BenchAvailability m_availability;
    };
} // namespace gladius::agent::bench
