#pragma once

#include "../agent/AgentEvent.h"
#include "../agent/AgentLoop.h"
#include "../agent/AgentRunControl.h"
#include "../agent/ContextBuilder.h"
#include "../agent/Conversation.h"
#include "../agent/CredentialStore.h"
#include "../agent/KnowledgeStore.h"
#include "../agent/PermissionPolicy.h"
#include "../agent/SkillLoader.h"
#include "../agent/providers/IAiProvider.h"
#include "../agent/providers/ImageProvider.h"
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
#include "PromptInput.h"
#include "AgentConversationView.h"
#include "AgentPanelHeader.h"
#include "AgentWorkflow.h"
#include "AgentWorkflowAnimation.h"
#include "AgentWorkflowView.h"
#include "WorkflowArtifacts.h"

#include <atomic>
#include <array>
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
        void setVisible(bool visible);
        [[nodiscard]] bool isVisible() const noexcept
        {
            return m_visible;
        }
        void toggleVisible();

        /// @brief True while the agent is actively executing and mutating the document
        [[nodiscard]] bool isDocumentLocked() const noexcept
        {
            return m_running.load();
        }

        /// @brief Attach an image file (e.g. from drag-and-drop) to the pending prompt attachments
        bool attachImageFile(std::filesystem::path const & path);

        /// @brief Append a dropped .txt/.md instruction file to the prompt draft
        bool attachPromptTextFile(std::filesystem::path const & path);

        /// @brief Save/load agent state associated with the given .3mf document (sidecar/session)
        void saveSessionState();
        void loadSessionState();
        /// @brief Fills an empty design graph and transcript with the demo session when requested
        void applyDemoSession();
        /// @brief Clears the design state for a document that was just replaced
        void onNewDocument();
        void onDocumentSaved(std::filesystem::path const & docPath);
        void onDocumentOpened(std::filesystem::path const & docPath);

        /// @brief Render the design agent workflow panel and the provider settings panel
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
        /// @brief True when at least one configured provider/model can be called right now
        [[nodiscard]] bool hasUsableModel() const;
        void stopRun();
        [[nodiscard]] agent::DesignContextSnapshot buildContextSnapshot(
          std::string const & knowledgeQuery);
        void loadSettings();
        void saveSettings();
        void releaseTextures();
        unsigned int getOrCreatePreviewTexture(std::string const & key,
                                               std::vector<std::byte> const & pngBytes,
                                               int & outW,
                                               int & outH);

        // --- Workflow panel ---
        [[nodiscard]] WorkflowSignals workflowSignals() const;
        [[nodiscard]] std::string phaseSummary(std::size_t index) const;
        void renderWorkflowPanel();
        void renderConversationSection();
        void renderDiagnosticsSection();
        [[nodiscard]] std::string buildDeveloperTrace() const;

        /// @brief Everything the pinned panel footer needs, measured once per frame
        struct FooterMetrics
        {
            PromptInput::CardMetrics composer;
            float statusLineHeight = 0.f;
            float controlRowsHeight = 0.f;
            float confirmationHeight = 0.f;
            /// @brief Space for the pinned agent reply card, 0 while nothing awaits a reply
            float replyCardHeight = 0.f;
            /// @brief Space for the provider error line, 0 while nothing failed
            float errorLineHeight = 0.f;
            float totalHeight = 0.f;
            std::size_t controlRows = 1u;
        };

        [[nodiscard]] FooterMetrics measureFooter();
        [[nodiscard]] float measureStatusLineHeight();
        /// @brief Derives the composer's font-relative layout from the current UI scale
        void applyComposerMetrics();
        [[nodiscard]] std::size_t measureControlRows();
        [[nodiscard]] float measureControlRowHeight() const;
        /// @brief Widths of the compact idle buttons, in draw order
        [[nodiscard]] std::vector<float> measureIdleButtonWidths() const;
        void renderStatusLine();
        void renderControlRow();
        /// @brief Square icon button that matches the composer toolbar's buttons
        [[nodiscard]] bool iconButton(char const * icon, char const * tooltip);
        void clearSession();
        void renderConfirmation();
        /// @brief Pinned card with the agent's last reply when it is waiting on the user
        void renderReplyCard();
        /// @brief Height the reply card needs, 0 when nothing awaits a reply
        [[nodiscard]] float measureReplyCard() const;
        /// @brief One-line provider failure with a settings shortcut, 0 height when nothing failed
        void renderErrorLine();
        /// @brief The provider failure in user terms, empty when nothing failed
        [[nodiscard]] std::string errorSummary() const;
        /// @brief Right-aligned autonomy chip: shows the mode and switches it
        void renderModeChip();
        /// @brief Continues a run that stopped on a question, optionally after a mode change
        void continueAfterReply();
        void renderComposer(PromptInput::CardMetrics const & metrics);
        void renderComposerToolbar(PromptInput::ToolbarLayout const & layout, int row);
        void renderImageControls(float buttonSize);
        [[nodiscard]] float leadingControlsWidth();
        void renderFooter();
        void attachClipboardImage();
        void insertClipboardOrFileImageIntoDocument();
        void renderPendingAttachments();
        void renderModelCombos(PromptInput::ToolbarLayout const & layout);

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

        bool m_visible = true;
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
        agent::AgentRunControl m_runControl;

        // Workflow rail presentation (never holds authoritative state)
        AgentWorkflowView m_workflowView;
        AgentWorkflowAnimationState m_workflowAnimation;
        AgentPanelHeader m_header;
        AgentConversationView m_conversationView;
        WorkflowArtifacts m_artifacts;
        std::array<bool, kWorkflowPhaseCount> m_phaseExpanded{};
        bool m_conversationExpanded = false;
        /// @brief False keeps the transcript to its newest turns with short bodies
        bool m_transcriptExpanded = false;
        bool m_activityLogExpanded = false;
        bool m_diagnosticsExpanded = false;
        std::size_t m_lastActivePhaseIndex = kWorkflowPhaseCount;

        bool m_followUpRequest = false;
        int m_inspectedImageIndex = -1;

        std::vector<agent::ProviderProfile> m_providerProfiles;
        int m_editingProfileIndex = 0;
        agent::ModelSelection m_primarySelection;
        std::vector<agent::ModelSelection> m_fallbackSelections;
        agent::ImageProviderConfig m_imageProviderConfig;

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
        PromptInput m_prompt;
        std::size_t m_nextAttachmentId = 1;

        std::string m_streamingBuffer;
        std::string m_activeToolName;
        std::chrono::steady_clock::time_point m_runStartTime;
        std::vector<ActivityEntry> m_activityLog;
        std::vector<PendingImageAttachment> m_pendingImages;
        std::unordered_map<std::string, CachedTexture> m_previewTextures;
        std::string m_lastError;
        /// @brief Classified m_lastError, shown in the footer with a settings shortcut
        agent::ProviderError m_lastProviderError;
        /// @brief Last assistant text of a run that stopped without finishing the design
        std::string m_lastAgentMessage;
        /// @brief True while the agent's last reply expects an answer from the user
        bool m_awaitingUserReply = false;
        bool m_replyExpanded = false;

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
