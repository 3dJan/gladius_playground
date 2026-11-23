#include "agentBench/AgentBenchReport.h"
#include "agentBench/AgentBenchTask.h"

#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>

namespace gladius::agent::bench::tests
{
    namespace
    {
        [[nodiscard]] bool isAgentBenchEnabled()
        {
            char const * const value = std::getenv("GLADIUS_RUN_AGENT_BENCH");
            return value != nullptr && std::string{value} != "0";
        }

        /// @brief Finds a task by id in the default task table
        [[nodiscard]] AgentBenchTask findTask(std::string const & id)
        {
            for (auto const & task : defaultBenchTasks())
            {
                if (task.id == id)
                {
                    return task;
                }
            }
            return {};
        }

        /// @brief Runs one task. The record is only written when the task really ran.
        /// @return false when the test body must return without asserting
        [[nodiscard]] bool benchmarkAvailable(std::string & skipReason)
        {
            if (!isAgentBenchEnabled())
            {
                skipReason = "Disabled; set GLADIUS_RUN_AGENT_BENCH=1 to run the design-agent "
                             "benchmark (requires the strata endpoint and a WebGPU runtime)";
                return false;
            }
            auto const availability = BenchSession::instance().ensureHost();
            if (!availability.available)
            {
                skipReason = availability.reason;
                return false;
            }
            return true;
        }

        void runTask(std::string const & taskId, BenchRunRecord & out)
        {
            auto const task = findTask(taskId);
            ASSERT_FALSE(task.id.empty()) << "Unknown task id: " << taskId;
            out = BenchSession::instance().runTask(task);
        }

        /// @brief Streams benchmark diagnostics to the test output
        [[nodiscard]] std::ostream & benchLog()
        {
            return std::cout << "[agent-bench] ";
        }

        void publishRecord(BenchRunRecord const & record)
        {
            auto const & m = record.metrics;
            benchLog() << "turns=" << m.modelTurns << " toolCalls=" << m.toolCalls
                       << " toolFailures=" << m.toolFailures
                       << " libraryImports=" << m.libraryImports
                       << " libraryExports=" << m.libraryExports
                       << " promptTokens=" << m.usage.totalPromptTokens()
                       << " completionTokens=" << m.usage.outputTokens
                       << " wallMs=" << m.wallMs << "\n";
            benchLog() << "agentCompleted=" << m.agentCompleted
                       << " geometryPassed=" << record.geometryPassed()
                       << " visualPassed=" << (record.judge.available && record.judge.passes)
                       << " taskPassed=" << record.taskPassed() << "\n";
            if (record.judge.available)
            {
                benchLog() << "judge: " << record.judge.observed << "\n";
            }
            else
            {
                benchLog() << "judge unavailable: " << record.judge.error << "\n";
            }
            for (auto const & [tool, counter] : m.perTool)
            {
                if (counter.failures > 0u)
                {
                    benchLog() << "tool failure: " << tool << " " << counter.failures << "/"
                               << counter.calls << "\n";
                }
            }
            for (std::size_t i = 0; i < std::min<std::size_t>(m.failureDetails.size(), 5u); ++i)
            {
                benchLog() << "failure[" << i << "]: " << m.failureDetails[i] << "\n";
            }
            if (!m.agentError.empty())
            {
                benchLog() << "agent error: " << m.agentError << "\n";
            }
        }

        /**
         * @brief Validates that the harness measured the run, not that the agent solved the task.
         *
         * Whether a task passes is the comparer's job (see scripts/agent_bench_compare.py); a task
         * failure must stay visible in results.json without turning the suite red, otherwise a
         * harness regression and a model-capability limit are indistinguishable here.
         */
        void expectRunWasMeasured(BenchRunRecord const & record)
        {
            EXPECT_GT(record.metrics.modelTurns, 0u) << record.metrics.agentError;
            EXPECT_GT(record.metrics.toolCalls, 0u);
            EXPECT_FALSE(record.geometryChecks.empty());
            EXPECT_FALSE(record.artifacts.empty());
            if (!record.metrics.agentCompleted)
            {
                // A model the agent never finished can be unrenderable (dangling call, empty
                // graph), and that is a task failure the comparer reports, not a harness bug.
                benchLog() << "task not completed by the agent (recorded, not a harness failure): "
                           << record.metrics.agentError << "\n";
                if (!record.geometry.error.empty())
                {
                    benchLog() << "model not measurable: " << record.geometry.error << "\n";
                }
                return;
            }
            EXPECT_TRUE(record.geometry.error.empty())
              << "The finished model could not be measured: " << record.geometry.error;
        }
    } // namespace

    TEST(DesignAgentBench, SphereR5_ProducesMeasuredModel)
    {
        std::string skipReason;
        if (!benchmarkAvailable(skipReason))
        {
            GTEST_SKIP() << skipReason;
        }

        BenchRunRecord record;
        runTask("sphere_r5", record);
        publishRecord(record);
        expectRunWasMeasured(record);
    }

    TEST(DesignAgentBench, SphereGyroid_ProducesMeasuredModel)
    {
        std::string skipReason;
        if (!benchmarkAvailable(skipReason))
        {
            GTEST_SKIP() << skipReason;
        }

        BenchRunRecord record;
        runTask("sphere_gyroid", record);
        publishRecord(record);
        expectRunWasMeasured(record);
    }

    TEST(DesignAgentBench, LegoBrick3x4_ProducesMeasuredModel)
    {
        std::string skipReason;
        if (!benchmarkAvailable(skipReason))
        {
            GTEST_SKIP() << skipReason;
        }

        BenchRunRecord record;
        runTask("lego_brick_3x4", record);
        publishRecord(record);
        expectRunWasMeasured(record);
    }

    /// @brief Writes the aggregated report once all requested tasks have run
    TEST(DesignAgentBench, Report_WritesAggregatedResultsJson)
    {
        auto & session = BenchSession::instance();
        if (session.report().records().empty())
        {
            GTEST_SKIP() << "No task ran (benchmark disabled or runtime unavailable)";
        }
        auto const path = session.report().write();
        EXPECT_TRUE(std::filesystem::exists(path)) << path.string();
        benchLog() << "report=" << path.string()
                   << " passed=" << session.report().passedCount() << "/"
                   << session.report().records().size() << "\n";
    }
} // namespace gladius::agent::bench::tests
