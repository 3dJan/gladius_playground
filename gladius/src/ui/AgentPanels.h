#pragma once

#include "../agent/AgentEvent.h"
#include "../agent/AgentLoop.h"
#include "../agent/ContextBuilder.h"
#include "../agent/Conversation.h"
#include "../agent/CredentialStore.h"
#include "../agent/PermissionPolicy.h"
#include "../agent/providers/IAiProvider.h"
#include "../design/DesignCatalogue.h"
#include "../design/DesignGraph.h"
#include "../design/DesignTools.h"
#include "../io/CancellationToken.h"
#include "../tools/CheckpointManager.h"
#include "../tools/ToolExecutor.h"
#include "../tools/ToolRegistry.h"
#include "../tools/TransactionManager.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace gladius
{
    class ConfigManager;
    class Document;
}

namespace gladius::ui
{
    /// @brief Hosts the embedded Dear ImGui panels for the Gladius Native Design Agent
    class AgentPanels
    {
      public:
        AgentPanels();
        ~AgentPanels();

        AgentPanels(AgentPanels const &) = delete;
        AgentPanels & operator=(AgentPanels const &) = delete;

        void bind(Document * document, ConfigManager * configManager);
        void setVisible(bool visible) noexcept
        {
            m_visible = visible;
        }
        [[nodiscard]] bool isVisible() const noexcept
        {
            return m_visible;
        }
        void toggleVisible() noexcept
        {
            m_visible = !m_visible;
        }

        /// @brief Render all visible agent, design-tree, variant-comparison, and settings panels
        void render();

      private:
        struct ActivityEntry
        {
            std::string label;
            std::string detail;
            bool success = true;
        };

        void drainEvents();
        void startRun(std::string const & userPrompt);
        void stopRun();
        void loadSettings();
        void saveSettings();

        void renderChatPanel();
        void renderDesignTreePanel();
        void renderVariantComparisonPanel();
        void renderProviderSettingsPanel();

        bool m_visible = false;
        bool m_showSettings = false;
        bool m_showDeveloperTrace = false;

        Document * m_document = nullptr;
        ConfigManager * m_configManager = nullptr;

        design::DesignGraph m_graph;
        design::DesignCatalogue m_catalogue;
        tools::TransactionManager m_transactions;
        tools::CheckpointManager m_checkpoints;
        tools::ToolRegistry m_registry;
        tools::ToolExecutor m_executor;
        std::unique_ptr<design::DesignToolContext> m_toolCtx;

        agent::Conversation m_conversation;
        agent::ContextBuilder m_contextBuilder;
        agent::PermissionPolicy m_policy;
        agent::AgentEventQueue m_eventQueue;
        agent::AgentLimits m_limits;
        agent::OsCredentialStore m_credentialStore;
        io::CancellationToken m_cancellation;

        int m_providerIndex = 0; // 0 = OpenAI-compatible, 1 = Anthropic
        char m_endpointBuf[512] = "https://api.openai.com/v1";
        char m_modelBuf[256] = "gpt-4o";
        char m_apiKeyBuf[512] = "";
        char m_promptBuf[2048] = "";

        std::string m_streamingBuffer;
        std::vector<ActivityEntry> m_activityLog;
        std::string m_lastError;

        std::atomic<bool> m_running{false};
        std::thread m_workerThread;

        // Confirmation handshake between worker thread and UI thread
        std::mutex m_confirmMutex;
        std::condition_variable m_confirmCv;
        bool m_awaitingConfirmation = false;
        bool m_confirmationResolved = false;
        bool m_confirmationApproved = false;
        std::string m_pendingConfirmPrompt;
    };
} // namespace gladius::ui
