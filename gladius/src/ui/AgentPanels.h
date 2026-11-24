#pragma once

#include "../agent/AgentEvent.h"
#include "../agent/AgentLoop.h"
#include "../agent/ContextBuilder.h"
#include "../agent/Conversation.h"
#include "../agent/CredentialStore.h"
#include "../agent/KnowledgeStore.h"
#include "../agent/PermissionPolicy.h"
#include "../agent/SkillLoader.h"
#include "../agent/providers/IAiProvider.h"
#include "../agent/providers/ModelDiscovery.h"
#include "../agent/providers/ProviderProfile.h"
#include "../design/DesignCatalogue.h"
#include "../design/DesignGraph.h"
#include "../design/DesignTools.h"
#include "../design/TodoStore.h"
#include "../io/CancellationToken.h"
#include "../tools/CheckpointManager.h"
#include "../tools/ToolExecutor.h"
#include "../tools/ToolRegistry.h"
#include "../tools/TransactionManager.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coro/coro.hpp>
#include <coroutine>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
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

        void bind(Document * document,
                  ConfigManager * configManager,
                  std::function<void()> onDocumentModified = nullptr);
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

        /// @brief True while the agent is actively executing and mutating the document
        [[nodiscard]] bool isDocumentLocked() const noexcept
        {
            return m_running.load();
        }

        /// @brief Attach an image file (e.g. from drag-and-drop) to the pending prompt attachments
        bool attachImageFile(std::filesystem::path const & path);

        /// @brief Save/load agent state associated with the given .3mf document (sidecar/session)
        void saveSessionState();
        void loadSessionState();
        /// @param documentSeeded True when the caller already replaced the document with a fresh
        ///        volumetric template, so the run must not seed it a second time.
        void onNewDocument(bool documentSeeded = false);
        void onDocumentSaved(std::filesystem::path const & docPath);
        void onDocumentOpened(std::filesystem::path const & docPath);

        /// @brief Render all visible agent, design-tree, variant-comparison, and settings panels
        void render();

      private:
        struct ActivityEntry
        {
            std::string label;
            std::string detail;
            bool success = true;
        };

        struct PendingImageAttachment
        {
            std::string filename;
            std::vector<std::byte> bytes;
            std::string mimeType = "image/png";
            std::string comment;
            std::string previewKey;
        };

        struct CachedTexture
        {
            unsigned int glTextureId = 0;
            int width = 0;
            int height = 0;
            std::size_t sourceBytes = 0;
        };

        void drainEvents();
        void startRun(std::string const & userPrompt);
        void stopRun();
        /// @brief Replaces the session document with the volumetric project template
        void seedVolumetricTemplate();
        [[nodiscard]] agent::DesignContextSnapshot buildContextSnapshot(
          std::string const & knowledgeQuery);
        void loadSettings();
        void saveSettings();
        void releaseTextures();
        unsigned int getOrCreatePreviewTexture(std::string const & key,
                                               std::vector<std::byte> const & pngBytes,
                                               int & outW,
                                               int & outH);

        void renderChatPanel();
        void renderDesignTreePanel();
        void renderVariantComparisonPanel();
        void renderProviderSettingsPanel();
        void startModelFetch(std::string const & profileId,
                             bool freeOnly = false,
                             bool autoAddToProfile = false);
        void pollModelFetch();
#ifdef __EMSCRIPTEN__
        coro::task<void> runModelFetchWeb(agent::ProviderProfile profileCopy,
                                          bool freeOnly,
                                          bool autoAddToProfile);
        coro::task<void> runAgentWeb(std::vector<agent::ModelSelection> selections,
                                     std::vector<agent::ProviderProfile> profilesCopy,
                                     agent::ProviderConfig legacyCfg,
                                     int legacyProviderIdx,
                                     std::string promptForKnowledge);
#endif

        bool m_visible = false;
        bool m_showSettings = false;
        bool m_showDeveloperTrace = false;

        Document * m_document = nullptr;
        ConfigManager * m_configManager = nullptr;
        std::function<void()> m_onDocumentModified;
        std::atomic<bool> m_documentModifiedPending{false};

        design::DesignGraph m_graph;
        design::DesignCatalogue m_catalogue;
        design::TodoStore m_todos;
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
        agent::KnowledgeStore m_knowledge;
        agent::SkillLoader m_skills;
        io::CancellationToken m_cancellation;

        bool m_startFresh = true;
        bool m_documentSeededByCaller = false;
        bool m_followUpRequest = false;
        bool m_promptFocused = false;
        int m_inspectedImageIndex = -1;

        std::vector<agent::ProviderProfile> m_providerProfiles;
        int m_editingProfileIndex = 0;
        agent::ModelSelection m_primarySelection;
        std::vector<agent::ModelSelection> m_fallbackSelections;
        std::vector<std::string> m_selectedRecombineConfigs;

        int m_providerIndex = 0; // legacy fallback
        char m_endpointBuf[512] = "";
        char m_modelBuf[256] = "";
        char m_apiKeyBuf[512] = "";

        std::atomic<bool> m_modelFetchRunning{false};
        std::thread m_modelFetchThread;
        std::mutex m_modelFetchMutex;
        bool m_modelFetchPendingApply = false;
        bool m_modelFetchAutoAdd = false;
        std::string m_modelFetchProfileId;
        agent::ModelDiscoveryResult m_modelFetchResult;
        std::string m_modelFetchStatus;
        std::string m_queriedModelsProfileId;
        std::vector<agent::ModelMetadata> m_queriedModels;
        char m_modelFilterBuf[128] = "";
        std::string m_credentialWarning;
        char m_promptBuf[2048] = "";
        char m_newReqBuf[256] = "";
        char m_newReqCatBuf[64] = "functional";
        bool m_newReqMandatory = true;
        std::size_t m_nextAttachmentId = 1;

        std::string m_streamingBuffer;
        std::string m_activeToolName;
        std::chrono::steady_clock::time_point m_runStartTime;
        std::vector<ActivityEntry> m_activityLog;
        std::vector<PendingImageAttachment> m_pendingImages;
        std::unordered_map<std::string, CachedTexture> m_previewTextures;
        std::string m_lastError;

        std::atomic<bool> m_running{false};
        std::thread m_workerThread;
#ifdef __EMSCRIPTEN__
        std::optional<coro::task<void>> m_webModelFetchTask;
        std::optional<coro::task<void>> m_webRunTask;
        std::coroutine_handle<> m_webConfirmHandle;
#endif

        // Confirmation handshake between worker thread and UI thread
        std::mutex m_confirmMutex;
        std::condition_variable m_confirmCv;
        bool m_awaitingConfirmation = false;
        bool m_confirmationResolved = false;
        bool m_confirmationApproved = false;
        std::string m_pendingConfirmPrompt;
    };
} // namespace gladius::ui
