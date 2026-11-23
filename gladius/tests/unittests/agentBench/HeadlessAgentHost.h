#pragma once

#include "AgentBenchTask.h"
#include "agent/AgentEvent.h"
#include "agent/AgentLoop.h"
#include "agent/ContextBuilder.h"
#include "agent/Conversation.h"
#include "agent/KnowledgeStore.h"
#include "agent/PermissionPolicy.h"
#include "agent/SkillLoader.h"
#include "agent/providers/IAiProvider.h"
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

        /// @brief Builds the provider configured from the environment (anthropic or openai wire format)
        [[nodiscard]] std::unique_ptr<IAiProvider> makeDefaultProvider() const;

      private:
        [[nodiscard]] std::vector<std::byte> captureViewport(int width,
                                                             int height,
                                                             std::string const & preset) const;

        /// @brief Loads the shipped template so the document has a renderable root function
        void seedTemplateDocument();

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
        tools::ToolRegistry m_registry;
        tools::ToolExecutor m_executor;
        agent::ContextBuilder m_contextBuilder;
        agent::PermissionPolicy m_policy;
        agent::AgentEventQueue m_events;
        agent::Conversation m_conversation;
        io::CancellationToken m_cancellation;
        std::string m_initializeError;
    };
} // namespace gladius::agent::bench
