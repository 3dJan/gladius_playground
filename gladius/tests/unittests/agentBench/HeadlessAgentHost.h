#pragma once

#include "AgentBenchTask.h"
#include "agent/AgentEvent.h"
#include "agent/AgentLoop.h"
#include "agent/ContextBuilder.h"
#include "agent/Conversation.h"
#include "agent/KnowledgeStore.h"
#include "agent/PermissionPolicy.h"
#include "agent/SkillLoader.h"
#include "agent/UserDecision.h"
#include "agent/providers/IAiProvider.h"
#include "agent/trace/AgentTrace.h"
#include "compute/ApplicationComputeRuntime.h"
#include "design/DesignCatalogue.h"
#include "design/DesignGraph.h"
#include "design/DesignTools.h"
#include "design/TodoStore.h"
#include "io/CancellationToken.h"
#include "tools/CheckpointManager.h"
#include "tools/ToolExecutor.h"
#include "tools/ToolRegistry.h"
#include "tools/TransactionManager.h"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace gladius
{
    class Document;

    namespace events
    {
        class Logger;
    }
}

namespace gladius::agent::bench
{
    /// @brief Runtime/provider configuration of one benchmark run, read from the environment
    struct BenchRunConfig
    {
        std::string endpoint = "http://localhost:47831/v1/messages";
        std::string model = "iq3_s";
        std::string apiKey;
        /// @brief Wire format: "anthropic" or "openai"
        std::string api = "anthropic";
        bool stream = true;
        int maxModelTurns = 40;
        std::size_t maxToolCalls = 200;
        /// @brief Safety net only; the model-turn budget is the budget that shapes the metrics
        std::chrono::milliseconds maxWallTime{std::chrono::minutes(30)};
        std::uint32_t renderWidth = 512;
        std::uint32_t renderHeight = 512;
        /// @brief HTTP timeout per completion; a local reasoning model can take minutes per turn
        int providerTimeoutSeconds = 600;
        /// @brief Completion budget per model turn; a whole program snippet has to fit in here
        int providerMaxTokens = 8192;
        /// @brief Hidden-reasoning budget per model turn; 0 leaves it to the provider
        /// @details Reasoning tokens cost the same as visible output and are thrown away, and on this
        ///          local endpoint they dominated every turn, so the benchmark caps them by default.
        int providerThinkingTokens = 1024;
        /// @brief Explicit prompt-cache breakpoints: "none", "tools" or "all"
        std::string cacheBreakpoints = "tools";
        /// @brief Tool surface the model sees: "full", "geometry", or a role name
        /// @details Every definition is re-sent on every turn, so the ~38-tool list is a fixed per-turn
        ///          cost. A role name selects both the tool surface and the role prompt for it.
        std::string toolSurface = "full";
        /// @brief Design node to scope the context to; empty means the whole graph (orchestrator view)
        /// @details Simulates one component agent of the hierarchical topology: requirements, todos and
        ///          sibling branches are withheld from the prompt.
        std::string scopeNodeId;
        /// @brief Starting document: "template" (shipped volumetric scaffold) or "blank"
        /// @details The scaffold is what a user gets from File > New. "blank" starts from an empty
        ///          3MF instead, which is how the harness behaves when nothing wires the program into
        ///          a level set: it measures whether the agent notices and repairs that on its own.
        std::string documentSeed = "template";
        /// @brief Let finish_design derive the design-graph bookkeeping instead of demanding it
        bool autoCompleteDesignGraph = false;
        /// @brief Viewport images the run may request; 0 is unlimited
        /// @details An image costs a fixed share of every following turn, so the budget matters more
        ///          than the image size.
        int maxViewportReviews = 0;
        /// @brief Answers to `ask_user` questions, consumed in question order
        /// @details A headless run has no human, so decisions are scripted: an option index or option
        ///          text per question. Once the script is exhausted the recommended option is assumed,
        ///          which keeps every run reproducible while still exercising decision mode.
        std::vector<std::string> decisionScript;

        /// @brief Reads `GLADIUS_AGENT_BENCH_*` overrides with strata defaults
        [[nodiscard]] static BenchRunConfig fromEnvironment();
    };

    /// @brief Availability of the resources a benchmark run needs
    struct BenchAvailability
    {
        bool available = false;
        std::string reason;
    };

    /**
     * @brief Redirects the user library to a sandbox directory for the lifetime of the object.
     *
     * Keeps benchmark exports countable and the developer's real library untouched.
     */
    class UserLibrarySandbox
    {
      public:
        explicit UserLibrarySandbox(std::filesystem::path directory);
        ~UserLibrarySandbox();

        UserLibrarySandbox(UserLibrarySandbox const &) = delete;
        UserLibrarySandbox & operator=(UserLibrarySandbox const &) = delete;

        [[nodiscard]] std::filesystem::path const & directory() const noexcept
        {
            return m_directory;
        }

      private:
        std::filesystem::path m_directory;
        bool m_hadPrevious = false;
        std::string m_previous;
    };

    /**
     * @brief Headless equivalent of the design-agent UI host.
     *
     * Owns a coreless WebGPU document plus the full native agent wiring (design graph, tools,
     * permission policy, conversation) so the real agent loop can be driven without ImGui.
     */
    class HeadlessAgentHost
    {
      public:
        explicit HeadlessAgentHost(BenchRunConfig config);
        ~HeadlessAgentHost();

        HeadlessAgentHost(HeadlessAgentHost const &) = delete;
        HeadlessAgentHost & operator=(HeadlessAgentHost const &) = delete;

        /// @brief Creates the WebGPU document and registers the design tools
        [[nodiscard]] BenchAvailability initialize();

        /// @brief Runs the agent loop for one task prompt on a fresh document
        [[nodiscard]] AgentRunResult runTask(AgentBenchTask const & task,
                                             std::unique_ptr<IAiProvider> provider);

        /// @brief Redirects the per-task JSONL trace + Perfetto timeline into `directory`
        /// @details Call before `runTask`; without it no trace files are written.
        void setTraceDirectory(std::filesystem::path directory);

        /// @brief Trace of the task currently being measured, null when tracing is off
        [[nodiscard]] agent::trace::AgentTraceRecorder * trace() noexcept
        {
            return m_trace.get();
        }

        /// @brief Forwarded mark of now, for measuring a phase that runs outside the agent loop
        [[nodiscard]] std::int64_t traceMark() const;

        /// @brief Forwarded `phase` record; used by the report for its own measurement steps
        void tracePhase(std::string const & name,
                        std::int64_t startedMark,
                        nlohmann::json fields = {});

        /// @brief Writes the Perfetto timeline of the current task trace; empty path when off
        [[nodiscard]] std::filesystem::path writeTaskTimeline() const;

        /// @brief Drains the agent event queue accumulated since the last drain
        [[nodiscard]] std::vector<AgentEvent> drainEvents();

        /// @brief Renders one named camera preset of the current model as PNG bytes
        [[nodiscard]] std::vector<std::byte> renderView(std::string const & view,
                                                        std::uint32_t size) const;

        /// @brief Exports the current model as binary STL; false plus `error` on failure
        /// @brief Validation issues and error-log entries the current document produced
        [[nodiscard]] std::vector<std::string> modelErrors() const;

        [[nodiscard]] bool exportStl(std::filesystem::path const & path, std::string & error) const;

        /// @brief Saves the current document as 3MF; false plus `error` on failure
        [[nodiscard]] bool saveDocument(std::filesystem::path const & path, std::string & error) const;

        /// @brief Path of the document the benchmark starts from (shipped template)
        [[nodiscard]] std::filesystem::path templatePath() const;

        [[nodiscard]] design::DesignGraph const & designGraph() const noexcept
        {
            return m_graph;
        }

        [[nodiscard]] std::string designSummary() const
        {
            return m_graph.summarize();
        }

        [[nodiscard]] std::size_t toolCount() const;
        [[nodiscard]] bool hasTool(std::string const & name) const;

        /// @brief Decisions resolved during the task currently being measured, oldest first
        [[nodiscard]] std::vector<agent::ScriptedDecision> decisions() const;

        /// @brief `ask_user` calls made by the model in the task currently being measured
        [[nodiscard]] std::size_t userQuestionsAsked() const;

        /// @brief Builds the provider configured from the environment (anthropic or openai wire format)
        [[nodiscard]] std::unique_ptr<IAiProvider> makeDefaultProvider() const;

      private:
        [[nodiscard]] std::vector<std::byte> captureViewport(int width,
                                                             int height,
                                                             std::string const & preset) const;

        /// @brief (Re)creates the per-task recorder when a trace directory is configured
        void beginTaskTrace(AgentBenchTask const & task);

        /// @brief Loads the shipped template, or leaves the document blank per the seed mode
        void seedDocument();

        /// @brief Frames a named camera preset around `bounds` so the whole model is visible
        [[nodiscard]] static compute::RenderCamera frameCamera(std::string const & view,
                                                                std::uint32_t size,
                                                                BoundingBox const & bounds);

        BenchRunConfig m_config;
        std::unique_ptr<compute::IBackendRuntime> m_runtime;
        std::shared_ptr<events::Logger> m_logger;
        std::unique_ptr<Document> m_document;
        design::DesignGraph m_graph;
        design::DesignCatalogue m_catalogue;
        tools::TransactionManager m_transactions;
        tools::CheckpointManager m_checkpoints;
        agent::SkillLoader m_skills;
        design::TodoStore m_todos;
        agent::KnowledgeStore m_knowledge;
        std::unique_ptr<design::DesignToolContext> m_toolCtx;
        /// @brief Decisions resolved by the scripted callback of the current task
        std::vector<agent::ScriptedDecision> m_decisions;
        tools::ToolRegistry m_registry;
        tools::ToolExecutor m_executor;
        agent::ContextBuilder m_contextBuilder;
        agent::PermissionPolicy m_policy;
        agent::AgentEventQueue m_events;
        agent::Conversation m_conversation;
        io::CancellationToken m_cancellation;
        std::string m_initializeError;
        bool m_geometryOnlySurface = false;
        /// @brief Tool surface / role pair resolved from `BenchRunConfig::toolSurface`
        design::DesignToolSurface m_toolSurface = design::DesignToolSurface::Full;
        AgentRole m_role = AgentRole::Default;
        std::filesystem::path m_traceDir;
        std::unique_ptr<agent::trace::AgentTraceRecorder> m_trace;
    };
} // namespace gladius::agent::bench
