#pragma once

#include "../providers/IAiProvider.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace gladius::agent::trace
{
    /**
     * @brief Byte-size composition of a CompletionRequest.
     * @details Token counts only come back from the provider after the call, and only for the whole
     *          prompt. Sizes per component are available before the call and show *which* part of
     *          the context is growing, which is the cheapest thing to fix on a local model.
     */
    struct RequestSizeBreakdown
    {
        std::int64_t systemPromptBytes = 0;
        std::int64_t toolSchemaBytes = 0;
        std::int64_t userTextBytes = 0;
        std::int64_t assistantTextBytes = 0;
        std::int64_t toolCallBytes = 0;
        std::int64_t toolResultBytes = 0;
        std::int64_t imageBytes = 0;
        std::size_t messages = 0;
        std::size_t toolDefinitions = 0;

        [[nodiscard]] nlohmann::json toJson() const;
        [[nodiscard]] std::int64_t totalBytes() const noexcept;
    };

    /// @brief Measures the component sizes of a completion request
    [[nodiscard]] RequestSizeBreakdown breakdownRequest(CompletionRequest const & request);

    /**
     * @brief Records a chronological, machine-readable trace of one agent run.
     *
     * Every record is one JSON object (`run_meta`, `turn`, `tool_call`, `phase`, `note`) kept in
     * memory for aggregation and appended as a JSONL line when a file sink is configured. The same
     * records can be re-emitted as Chrome Trace Event JSON for Perfetto.
     *
     * @note Not thread-safe: a run drives the recorder from the agent thread only.
     */
    class AgentTraceRecorder
    {
      public:
        /// @param outputFile when non-empty, every record is appended to this file as a JSONL line
        explicit AgentTraceRecorder(std::filesystem::path outputFile = {});

        /// @brief Also store the full request JSON on each turn record (large, opt-in)
        void setPromptDumpEnabled(bool enabled) noexcept;
        [[nodiscard]] bool promptDumpEnabled() const noexcept;

        void setRunId(std::string runId);
        /// @brief Emit the `run_meta` record describing the run configuration
        void recordRunMeta(nlohmann::json fields);
        /// @brief Emit one `turn` record for a model completion
        void recordTurn(nlohmann::json fields);
        /// @brief Emit one `tool_call` record for an executed tool call
        void recordToolCall(nlohmann::json fields);
        /// @brief Emit one `note` record (nudges, denials, other events worth marking)
        void recordNote(std::string const & kind, nlohmann::json fields = {});

        /// @brief Monotonic millisecond mark relative to recorder construction
        [[nodiscard]] std::int64_t mark() const noexcept;

        /**
         * @brief Emit a `phase` record spanning `startedMark` up to now.
         * @param name stable phase name, e.g. `seed_document` or `render:isometric`
         */
        void recordPhase(std::string const & name,
                         std::int64_t startedMark,
                         nlohmann::json fields = {});

        [[nodiscard]] std::vector<nlohmann::json> const & records() const noexcept;
        [[nodiscard]] std::size_t recordCount() const noexcept;
        /// @brief JSONL sink path, empty when the recorder only keeps records in memory
        [[nodiscard]] std::filesystem::path const & outputFile() const noexcept
        {
            return m_outputFile;
        }
        /// @brief Conventional sibling path of the Perfetto timeline for this trace
        [[nodiscard]] std::filesystem::path timelinePath() const;

        /// @brief Writes the records as Chrome Trace Event JSON (Perfetto, chrome://tracing)
        bool writeTimeline(std::filesystem::path const & path, std::string & error) const;

      private:
        void emit(nlohmann::json record);

        std::filesystem::path m_outputFile;
        std::ofstream m_stream;
        std::vector<nlohmann::json> m_records;
        std::string m_runId;
        std::chrono::steady_clock::time_point m_start = std::chrono::steady_clock::now();
        bool m_promptDumpEnabled = false;
    };

    /**
     * @brief RAII helper that turns a code region into a `phase` record.
     * @details Accepts a null recorder so optionally-traced code stays readable. Coroutines measure
     *          explicitly instead: a local span object would also be destroyed by an early
     *          `co_return`, which silently mis-reports cancelled runs.
     */
    class ScopedTracePhase
    {
      public:
        ScopedTracePhase(AgentTraceRecorder * recorder, std::string name);
        ~ScopedTracePhase();

        ScopedTracePhase(ScopedTracePhase const &) = delete;
        ScopedTracePhase & operator=(ScopedTracePhase const &) = delete;

        /// @brief Attach extra fields to the record written on destruction
        void addFields(nlohmann::json fields);

      private:
        AgentTraceRecorder * m_recorder;
        std::string m_name;
        std::int64_t m_startedMark = 0;
        nlohmann::json m_fields = nlohmann::json::object();
    };
} // namespace gladius::agent::trace
