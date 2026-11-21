#include "AgentPanels.h"

#include "../ConfigManager.h"
#include "../Document.h"
#include "../agent/providers/AnthropicProvider.h"
#include "../agent/providers/OpenAiProvider.h"
#if defined(GLADIUS_ENABLE_OPENCL)
#include "../compute/ComputeCore.h"
#endif

#include <IconsFontAwesome5.h>
#include <imgui.h>
#include <lodepng.h>
#include <tinyfiledialogs/tinyfiledialogs.h>
#if defined(GLADIUS_UI_BACKEND_OPENGL)
#include <glad/glad.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace gladius::ui
{
    namespace
    {
        constexpr char const * kCredentialService = "gladius.design_agent";

        std::vector<std::byte> readBinaryFile(std::string const & path)
        {
            std::ifstream in(path, std::ios::binary | std::ios::ate);
            if (!in)
            {
                return {};
            }
            auto const size = in.tellg();
            if (size <= 0)
            {
                return {};
            }
            std::vector<std::byte> bytes(static_cast<std::size_t>(size));
            in.seekg(0, std::ios::beg);
            in.read(reinterpret_cast<char *>(bytes.data()), size);
            return bytes;
        }

        std::string mimeTypeForPath(std::string const & path)
        {
            std::string ext = std::filesystem::path(path).extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            if (ext == ".jpg" || ext == ".jpeg")
            {
                return "image/jpeg";
            }
            if (ext == ".webp")
            {
                return "image/webp";
            }
            return "image/png";
        }
    } // namespace

    AgentPanels::AgentPanels()
        : m_executor(m_registry,
                     [this](tools::ToolDefinition const & def, nlohmann::json const & args) {
                         return m_policy.evaluateTool(def, args);
                     })
    {
        m_toolCtx = std::make_unique<design::DesignToolContext>(design::DesignToolContext{
          m_graph,
          m_catalogue,
          m_transactions,
          m_checkpoints,
          nullptr,
          [this](int w, int h, std::string const & /*cam*/) -> std::vector<std::byte> {
              if (!m_document)
              {
                  return {};
              }
              try
              {
#if defined(GLADIUS_ENABLE_OPENCL)
                  if (auto core = m_document->getCore())
                  {
                      (void) core->prepareImageRendering();
                  }
#endif
                  auto const clampedW = static_cast<std::uint32_t>(std::clamp(w, 64, 1024));
                  auto const clampedH = static_cast<std::uint32_t>(std::clamp(h, 64, 1024));
                  auto const png = m_document->createThumbnailPng(clampedW, clampedH);
                  std::vector<std::byte> out(png.data.size());
                  std::transform(png.data.begin(), png.data.end(), out.begin(), [](unsigned char b) {
                      return static_cast<std::byte>(b);
                  });
                  return out;
              }
              catch (...)
              {
                  return {};
              }
          },
          nullptr,
          [this]() { m_documentModifiedPending.store(true); }});
        design::registerDesignTools(m_registry, *m_toolCtx);
    }

    AgentPanels::~AgentPanels()
    {
        stopRun();
        releaseTextures();
    }

    void AgentPanels::releaseTextures()
    {
#if defined(GLADIUS_UI_BACKEND_OPENGL)
        for (auto & [key, tex] : m_previewTextures)
        {
            if (tex.glTextureId != 0)
            {
                GLuint id = tex.glTextureId;
                glDeleteTextures(1, &id);
                tex.glTextureId = 0;
            }
        }
#endif
        m_previewTextures.clear();
    }

    unsigned int AgentPanels::getOrCreatePreviewTexture(std::string const & key,
                                                        std::vector<std::byte> const & pngBytes,
                                                        int & outW,
                                                        int & outH)
    {
        outW = 0;
        outH = 0;
        if (pngBytes.empty())
        {
            return 0;
        }
#if defined(GLADIUS_UI_BACKEND_OPENGL)
        auto it = m_previewTextures.find(key);
        if (it != m_previewTextures.end() && it->second.sourceBytes == pngBytes.size()
            && it->second.glTextureId != 0)
        {
            outW = it->second.width;
            outH = it->second.height;
            return it->second.glTextureId;
        }

        std::vector<unsigned char> rgba;
        unsigned w = 0;
        unsigned h = 0;
        unsigned const err = lodepng::decode(
          rgba, w, h, reinterpret_cast<unsigned char const *>(pngBytes.data()), pngBytes.size());
        if (err != 0 || w == 0 || h == 0)
        {
            return 0;
        }

        GLuint texId = 0;
        if (it != m_previewTextures.end() && it->second.glTextureId != 0)
        {
            texId = it->second.glTextureId;
        }
        else
        {
            glGenTextures(1, &texId);
        }

        glBindTexture(GL_TEXTURE_2D, texId);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexImage2D(GL_TEXTURE_2D,
                     0,
                     GL_RGBA,
                     static_cast<GLsizei>(w),
                     static_cast<GLsizei>(h),
                     0,
                     GL_RGBA,
                     GL_UNSIGNED_BYTE,
                     rgba.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        CachedTexture entry{texId, static_cast<int>(w), static_cast<int>(h), pngBytes.size()};
        m_previewTextures[key] = entry;
        outW = entry.width;
        outH = entry.height;
        return texId;
#else
        (void) key;
        return 0;
#endif
    }

    void AgentPanels::bind(Document * document,
                           ConfigManager * configManager,
                           std::function<void()> onDocumentModified)
    {
        m_document = document;
        m_configManager = configManager;
        m_onDocumentModified = std::move(onDocumentModified);
        if (m_toolCtx)
        {
            m_toolCtx->document = document;
        }
        loadSettings();
    }

    void AgentPanels::loadSettings()
    {
        if (m_configManager == nullptr)
        {
            return;
        }
        m_providerIndex = m_configManager->getValue<int>("Agent", "providerIndex", 0);
        int const autonomyInt = std::clamp(
          m_configManager->getValue<int>(
            "Agent", "autonomy", static_cast<int>(agent::AutonomyLevel::AskAtMajorDecisions)),
          0,
          2);
        m_policy.setAutonomy(static_cast<agent::AutonomyLevel>(autonomyInt));
        std::string const ep =
          m_configManager->getValue<std::string>("Agent", "endpoint", "https://api.openai.com/v1");
        std::string const mdl = m_configManager->getValue<std::string>("Agent", "model", "gpt-4o");
        std::snprintf(m_endpointBuf, sizeof(m_endpointBuf), "%s", ep.c_str());
        std::snprintf(m_modelBuf, sizeof(m_modelBuf), "%s", mdl.c_str());

        auto privacy = m_policy.privacy();
        privacy.allowConversationText =
          m_configManager->getValue<bool>("AgentPrivacy", "allowConversationText", true);
        privacy.allowModelMetadata =
          m_configManager->getValue<bool>("AgentPrivacy", "allowModelMetadata", true);
        privacy.allowValidationResults =
          m_configManager->getValue<bool>("AgentPrivacy", "allowValidationResults", true);
        privacy.allowViewportImages =
          m_configManager->getValue<bool>("AgentPrivacy", "allowViewportImages", true);
        privacy.allowReferenceImages =
          m_configManager->getValue<bool>("AgentPrivacy", "allowReferenceImages", true);
        privacy.allowRawGeometry =
          m_configManager->getValue<bool>("AgentPrivacy", "allowRawGeometry", false);
        m_policy.setPrivacy(privacy);

        std::string const account = (m_providerIndex == 1) ? "anthropic" : "openai";
        if (auto key = m_credentialStore.loadSecret(kCredentialService, account))
        {
            std::snprintf(m_apiKeyBuf, sizeof(m_apiKeyBuf), "%s", key->c_str());
        }
    }

    void AgentPanels::saveSettings()
    {
        if (m_configManager != nullptr)
        {
            m_configManager->setValue<int>("Agent", "providerIndex", m_providerIndex);
            m_configManager->setValue<int>(
              "Agent", "autonomy", static_cast<int>(m_policy.autonomy()));
            m_configManager->setValue<std::string>("Agent", "endpoint", m_endpointBuf);
            m_configManager->setValue<std::string>("Agent", "model", m_modelBuf);

            auto const & privacy = m_policy.privacy();
            m_configManager->setValue<bool>("AgentPrivacy", "allowConversationText", privacy.allowConversationText);
            m_configManager->setValue<bool>("AgentPrivacy", "allowModelMetadata", privacy.allowModelMetadata);
            m_configManager->setValue<bool>("AgentPrivacy", "allowValidationResults", privacy.allowValidationResults);
            m_configManager->setValue<bool>("AgentPrivacy", "allowViewportImages", privacy.allowViewportImages);
            m_configManager->setValue<bool>("AgentPrivacy", "allowReferenceImages", privacy.allowReferenceImages);
            m_configManager->setValue<bool>("AgentPrivacy", "allowRawGeometry", privacy.allowRawGeometry);
            m_configManager->save();
        }
        std::string const account = (m_providerIndex == 1) ? "anthropic" : "openai";
        if (m_apiKeyBuf[0] != '\0')
        {
            m_credentialStore.storeSecret(kCredentialService, account, m_apiKeyBuf);
        }
    }

    void AgentPanels::drainEvents()
    {
        if (m_documentModifiedPending.exchange(false) && m_onDocumentModified)
        {
            m_onDocumentModified();
        }

        for (auto const & ev : m_eventQueue.drain())
        {
            if (auto const * td = std::get_if<agent::TextDeltaEvent>(&ev))
            {
                m_streamingBuffer += td->text;
            }
            else if (auto const * ts = std::get_if<agent::ToolCallStartedEvent>(&ev))
            {
                m_activeToolName = ts->name;
                m_activityLog.push_back({"Running " + ts->name, ts->arguments.dump(), true});
            }
            else if (auto const * tc = std::get_if<agent::ToolCallCompletedEvent>(&ev))
            {
                m_activeToolName.clear();
                m_activityLog.push_back(
                  {(tc->success ? "Completed " : "Failed ") + tc->name, tc->result.dump(), tc->success});
            }
            else if (auto const * ud = std::get_if<agent::UserDecisionRequiredEvent>(&ev))
            {
                std::lock_guard<std::mutex> lock(m_confirmMutex);
                m_pendingConfirmPrompt = ud->prompt;
                m_awaitingConfirmation = true;
                m_confirmationResolved = false;
            }
            else if (std::holds_alternative<agent::AgentCompletedEvent>(ev))
            {
                m_activeToolName.clear();
                m_streamingBuffer.clear();
            }
            else if (auto const * af = std::get_if<agent::AgentFailedEvent>(&ev))
            {
                m_activeToolName.clear();
                m_lastError = af->error;
                m_streamingBuffer.clear();
            }
        }
    }

    void AgentPanels::startRun(std::string const & userPrompt)
    {
        if (m_running.load() || (userPrompt.empty() && m_pendingImages.empty()))
        {
            return;
        }
        if (m_workerThread.joinable())
        {
            m_workerThread.join();
        }

        m_lastError.clear();
        m_streamingBuffer.clear();
        m_activeToolName.clear();
        m_runStartTime = std::chrono::steady_clock::now();
        m_cancellation.reset();

        agent::Message msg;
        msg.role = agent::MessageRole::User;
        if (!userPrompt.empty())
        {
            msg.parts.emplace_back(agent::TextContent{userPrompt});
        }
        for (auto & img : m_pendingImages)
        {
            msg.parts.emplace_back(agent::ImageContent{std::move(img.bytes), std::move(img.mimeType)});
        }
        m_pendingImages.clear();
        m_conversation.append(std::move(msg));
        m_running.store(true);

        agent::ProviderConfig cfg;
        cfg.endpoint = m_endpointBuf;
        cfg.model = m_modelBuf;
        cfg.apiKey = m_apiKeyBuf;
        cfg.stream = true;

        int const providerIdx = m_providerIndex;
        m_workerThread = std::thread([this, cfg, providerIdx]() {
            std::unique_ptr<agent::IAiProvider> provider;
            if (providerIdx == 1)
            {
                provider = std::make_unique<agent::AnthropicProvider>(cfg);
            }
            else
            {
                provider = std::make_unique<agent::OpenAiProvider>(cfg);
            }

            agent::AgentLoop loop;
            (void) coro::sync_wait(loop.run(
              m_conversation,
              *provider,
              m_registry,
              m_executor,
              m_contextBuilder,
              m_policy,
              m_eventQueue,
              m_limits,
              m_cancellation,
              [this]() { return m_graph.phase() == design::DesignPhase::Complete; },
              [this](tools::ToolDefinition const &, nlohmann::json const &) {
                  std::unique_lock<std::mutex> lock(m_confirmMutex);
                  m_confirmCv.wait(lock, [this]() {
                      return m_confirmationResolved || m_cancellation.isCancelled();
                  });
                  bool const ok = m_confirmationApproved && !m_cancellation.isCancelled();
                  m_awaitingConfirmation = false;
                  m_confirmationResolved = false;
                  return ok;
              },
              [this]() {
                  agent::DesignContextSnapshot snap;
                  snap.designPhase = design::toString(m_graph.phase());
                  snap.designGraphSummary = m_graph.summarize();
                  return snap;
              }));
            m_running.store(false);
        });
    }

    void AgentPanels::stopRun()
    {
        m_cancellation.requestCancellation();
        {
            std::lock_guard<std::mutex> lock(m_confirmMutex);
            m_confirmationResolved = true;
            m_confirmationApproved = false;
        }
        m_confirmCv.notify_all();
        if (m_workerThread.joinable())
        {
            m_workerThread.join();
        }
        m_running.store(false);
    }

    void AgentPanels::render()
    {
        if (!m_visible)
        {
            return;
        }
        drainEvents();
        renderDesignTreePanel();
        renderVariantComparisonPanel();
        renderChatPanel();
        if (m_showSettings)
        {
            renderProviderSettingsPanel();
        }
    }

    void AgentPanels::renderChatPanel()
    {
        ImGuiViewport const * vp = ImGui::GetMainViewport();
        float const rightWidth = std::clamp(vp->WorkSize.x * 0.28f, 340.f, 480.f);
        float const rightX = vp->WorkPos.x + vp->WorkSize.x - rightWidth;
        float const chatY = vp->WorkPos.y + vp->WorkSize.y * 0.52f;
        float const chatH = vp->WorkSize.y * 0.48f;
        ImGui::SetNextWindowPos(ImVec2(rightX, chatY), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(ImVec2(rightWidth, chatH), ImGuiCond_FirstUseEver);

        if (!ImGui::Begin("Design Agent", &m_visible))
        {
            ImGui::End();
            return;
        }

        if (ImGui::Button("Settings"))
        {
            m_showSettings = !m_showSettings;
        }
        ImGui::SameLine();
        ImGui::Checkbox("Developer trace", &m_showDeveloperTrace);
        ImGui::SameLine();
        if (ImGui::Button("Copy Trace"))
        {
            std::string trace;
            for (auto const & entry : m_activityLog)
            {
                trace += entry.label + "\n";
                if (!entry.detail.empty())
                {
                    trace += entry.detail + "\n";
                }
            }
            for (auto const & msg : m_conversation.messages())
            {
                char const * role = "User";
                if (msg.role == agent::MessageRole::Assistant)
                    role = "Agent";
                else if (msg.role == agent::MessageRole::Tool)
                    role = "Tool";
                for (auto const & part : msg.parts)
                {
                    if (auto const * t = std::get_if<agent::TextContent>(&part))
                    {
                        trace += std::string("[") + role + "] " + t->text + "\n";
                    }
                    else if (auto const * tc = std::get_if<agent::ToolCall>(&part))
                    {
                        trace += "[ToolCall] " + tc->name + ": " + tc->arguments.dump() + "\n";
                    }
                    else if (auto const * tr = std::get_if<agent::ToolResult>(&part))
                    {
                        trace += "[ToolResult] " + tr->callId + ": " + tr->value.dump() + "\n";
                    }
                }
            }
            if (!m_lastError.empty())
            {
                trace += "[Error] " + m_lastError + "\n";
            }
            ImGui::SetClipboardText(trace.c_str());
        }
        ImGui::SameLine();
        if (m_running.load())
        {
            if (ImGui::Button("Stop"))
            {
                stopRun();
            }
        }
        else if (ImGui::Button("Clear"))
        {
            m_conversation.clear();
            m_activityLog.clear();
            m_lastError.clear();
        }

        // Status indicator: working vs waiting for input vs complete
        if (m_running.load())
        {
            static constexpr char const * kSpinnerFrames[] = {"|", "/", "-", "\\"};
            int const frameIdx = static_cast<int>(ImGui::GetTime() * 8.0) % 4;
            auto const elapsedSec = std::chrono::duration_cast<std::chrono::seconds>(
                                      std::chrono::steady_clock::now() - m_runStartTime)
                                      .count();
            if (!m_activeToolName.empty())
            {
                ImGui::TextColored(ImVec4(0.35f, 0.8f, 1.0f, 1.f),
                                   "[%s] Working: running %s (%llds)...",
                                   kSpinnerFrames[frameIdx],
                                   m_activeToolName.c_str(),
                                   static_cast<long long>(elapsedSec));
            }
            else
            {
                ImGui::TextColored(ImVec4(0.35f, 0.8f, 1.0f, 1.f),
                                   "[%s] Agent is thinking (%llds)...",
                                   kSpinnerFrames[frameIdx],
                                   static_cast<long long>(elapsedSec));
            }
        }
        else
        {
            bool awaiting = false;
            {
                std::lock_guard<std::mutex> lock(m_confirmMutex);
                awaiting = m_awaitingConfirmation && !m_confirmationResolved;
            }
            if (awaiting)
            {
                ImGui::TextColored(
                  ImVec4(1.0f, 0.8f, 0.2f, 1.f), "Action required: Approve or Decline below");
            }
            else if (m_graph.phase() == design::DesignPhase::Complete)
            {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.f), "Design complete");
            }
            else if (!m_conversation.empty())
            {
                ImGui::TextColored(ImVec4(0.4f, 0.85f, 1.0f, 1.f),
                                   "Waiting for your input - reply to continue or refine");
            }
            else
            {
                ImGui::TextDisabled("Ready - describe what you want to design");
            }
        }

        ImGui::Separator();

        // Collapsed routine tool summary
        if (!m_activityLog.empty())
        {
            if (ImGui::CollapsingHeader(("Activity (" + std::to_string(m_activityLog.size()) + " steps)").c_str()))
            {
                for (std::size_t idx = 0; idx < m_activityLog.size(); ++idx)
                {
                    auto const & entry = m_activityLog[idx];
                    ImGui::PushID(static_cast<int>(idx));
                    ImGui::TextUnformatted(entry.label.c_str());
                    if (m_showDeveloperTrace && !entry.detail.empty())
                    {
                        ImGui::SameLine();
                        if (ImGui::SmallButton("Copy"))
                        {
                            std::string const text = entry.label + "\n" + entry.detail;
                            ImGui::SetClipboardText(text.c_str());
                        }
                        std::string mutableDetail = entry.detail;
                        ImGui::InputTextMultiline("##detail",
                                                  mutableDetail.data(),
                                                  mutableDetail.size() + 1,
                                                  ImVec2(-1.f, 54.f),
                                                  ImGuiInputTextFlags_ReadOnly);
                    }
                    ImGui::PopID();
                }
            }
        }

        float const footerHeight = m_pendingImages.empty() ? 104.f : 132.f;
        ImGui::BeginChild("AgentMessages", ImVec2(0, -footerHeight), true);
        for (auto const & msg : m_conversation.messages())
        {
            char const * roleLabel = "User";
            if (msg.role == agent::MessageRole::Assistant)
            {
                roleLabel = "Agent";
            }
            else if (msg.role == agent::MessageRole::Tool)
            {
                if (!m_showDeveloperTrace)
                {
                    continue;
                }
                roleLabel = "Tool";
            }

            for (auto const & part : msg.parts)
            {
                if (auto const * t = std::get_if<agent::TextContent>(&part))
                {
                    ImGui::TextWrapped("[%s] %s", roleLabel, t->text.c_str());
                }
                else if (m_showDeveloperTrace)
                {
                    if (auto const * tc = std::get_if<agent::ToolCall>(&part))
                    {
                        ImGui::TextWrapped("[ToolCall] %s %s", tc->name.c_str(), tc->arguments.dump().c_str());
                    }
                    else if (auto const * tr = std::get_if<agent::ToolResult>(&part))
                    {
                        ImGui::TextWrapped("[ToolResult] %s", tr->value.dump().c_str());
                    }
                }
                else if (std::holds_alternative<agent::ImageContent>(part))
                {
                    ImGui::TextDisabled("[%s attached an image]", roleLabel);
                }
            }
        }
        if (!m_streamingBuffer.empty())
        {
            ImGui::TextWrapped("[Agent] %s", m_streamingBuffer.c_str());
        }
        if (!m_lastError.empty())
        {
            ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "Error: %s", m_lastError.c_str());
        }
        ImGui::EndChild();

        // Confirmation prompt if waiting for user decision
        {
            std::lock_guard<std::mutex> lock(m_confirmMutex);
            if (m_awaitingConfirmation && !m_confirmationResolved)
            {
                ImGui::TextWrapped("%s", m_pendingConfirmPrompt.c_str());
                if (ImGui::Button("Approve"))
                {
                    m_confirmationApproved = true;
                    m_confirmationResolved = true;
                    m_confirmCv.notify_all();
                }
                ImGui::SameLine();
                if (ImGui::Button("Decline"))
                {
                    m_confirmationApproved = false;
                    m_confirmationResolved = true;
                    m_confirmCv.notify_all();
                }
            }
        }

        // Image attachment and document insertion controls
        if (ImGui::Button(reinterpret_cast<char const *>(ICON_FA_IMAGE " Attach Image")))
        {
            char const * filters[] = {"*.png", "*.jpg", "*.jpeg"};
            char const * selected =
              tinyfd_openFileDialog("Attach Reference Image", "", 3, filters, "Image files", 0);
            if (selected != nullptr)
            {
                auto bytes = readBinaryFile(selected);
                if (!bytes.empty())
                {
                    m_pendingImages.push_back(PendingImageAttachment{
                      std::filesystem::path(selected).filename().string(),
                      std::move(bytes),
                      mimeTypeForPath(selected)});
                }
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Insert Image to Document"))
        {
            if (m_document != nullptr)
            {
                char const * filters[] = {"*.png", "*.jpg", "*.jpeg"};
                char const * selected = tinyfd_openFileDialog(
                  "Insert Image Into 3MF Document", "", 3, filters, "Image files", 0);
                if (selected != nullptr)
                {
                    try
                    {
                        (void) m_document->addImageStackResource(selected);
                        m_document->markFileAsChanged();
                        if (m_onDocumentModified)
                        {
                            m_onDocumentModified();
                        }
                    }
                    catch (std::exception const & ex)
                    {
                        m_lastError = std::string("Failed to insert image: ") + ex.what();
                    }
                }
            }
        }

        if (!m_pendingImages.empty())
        {
            for (std::size_t i = 0; i < m_pendingImages.size(); ++i)
            {
                if (i > 0)
                {
                    ImGui::SameLine();
                }
                ImGui::Text("[%s]", m_pendingImages[i].filename.c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton(("x##img" + std::to_string(i)).c_str()))
                {
                    m_pendingImages.erase(m_pendingImages.begin() + static_cast<long>(i));
                    break;
                }
            }
        }

        ImGui::InputTextMultiline("##AgentPrompt", m_promptBuf, sizeof(m_promptBuf), ImVec2(-75.f, 56.f));
        ImGui::SameLine();
        ImGui::BeginDisabled(m_running.load());
        if (ImGui::Button("Send", ImVec2(65.f, 56.f)))
        {
            std::string prompt(m_promptBuf);
            m_promptBuf[0] = '\0';
            startRun(prompt);
        }
        ImGui::EndDisabled();

        ImGui::End();
    }

    void AgentPanels::renderDesignTreePanel()
    {
        ImGuiViewport const * vp = ImGui::GetMainViewport();
        float const rightWidth = std::clamp(vp->WorkSize.x * 0.28f, 340.f, 480.f);
        float const rightX = vp->WorkPos.x + vp->WorkSize.x - rightWidth;
        ImGui::SetNextWindowPos(ImVec2(rightX, vp->WorkPos.y), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(
          ImVec2(rightWidth, vp->WorkSize.y * 0.28f), ImGuiCond_FirstUseEver);

        if (!ImGui::Begin("Design Tree"))
        {
            ImGui::End();
            return;
        }
        ImGui::Text("Phase: %s", design::toString(m_graph.phase()));
        ImGui::Separator();

        if (ImGui::TreeNode("Requirements"))
        {
            for (auto const & req : m_graph.requirements())
            {
                ImGui::BulletText("[%s] %s", req.category.c_str(), req.description.c_str());
            }
            ImGui::TreePop();
        }

        for (auto const & [id, node] : m_graph.nodes())
        {
            bool frozen = (node.status == design::DesignStatus::Frozen);
            std::string const label = node.name + (frozen ? " [Frozen]" : "");
            if (ImGui::TreeNode(id.c_str(), "%s", label.c_str()))
            {
                if (ImGui::Checkbox(("Freeze##" + id).c_str(), &frozen))
                {
                    if (frozen)
                    {
                        m_graph.freezeNode(id);
                    }
                    else
                    {
                        m_graph.unfreezeNode(id);
                    }
                }
                for (auto const & vid : node.variantIds)
                {
                    auto vit = m_graph.variants().find(vid);
                    if (vit != m_graph.variants().end())
                    {
                        ImGui::BulletText("%s (%s)", vit->second.name.c_str(),
                                          design::toString(vit->second.fidelity));
                    }
                }
                ImGui::TreePop();
            }
        }
        ImGui::End();
    }

    void AgentPanels::renderVariantComparisonPanel()
    {
        ImGuiViewport const * vp = ImGui::GetMainViewport();
        float const rightWidth = std::clamp(vp->WorkSize.x * 0.28f, 340.f, 480.f);
        float const rightX = vp->WorkPos.x + vp->WorkSize.x - rightWidth;
        ImGui::SetNextWindowPos(
          ImVec2(rightX, vp->WorkPos.y + vp->WorkSize.y * 0.28f), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSize(
          ImVec2(rightWidth, vp->WorkSize.y * 0.24f), ImGuiCond_FirstUseEver);

        if (!ImGui::Begin("Variant Comparison"))
        {
            ImGui::End();
            return;
        }
        if (m_graph.configurations().empty() && m_checkpoints.listCheckpoints().empty())
        {
            ImGui::TextDisabled("No system configurations or snapshots generated yet.");
            ImGui::End();
            return;
        }

        for (auto const & [cid, cfg] : m_graph.configurations())
        {
            ImGui::PushID(cid.c_str());
            int w = 0;
            int h = 0;
            unsigned int const texId =
              getOrCreatePreviewTexture("cfg_" + cid, cfg.previewPng, w, h);
            if (texId != 0)
            {
                ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(texId)),
                             ImVec2(64.f, 64.f));
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            ImGui::Text("%s %s", cfg.name.c_str(), cfg.active ? "[Active]" : "");
            if (!cfg.active && ImGui::Button("Activate"))
            {
                m_graph.activateConfiguration(cid);
            }
            for (auto const & p : cfg.evaluation.deterministicPassed)
            {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.f), "OK: %s", p.c_str());
            }
            for (auto const & wMsg : cfg.evaluation.deterministicWarnings)
            {
                ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f), "WARN: %s", wMsg.c_str());
            }
            ImGui::EndGroup();
            ImGui::Separator();
            ImGui::PopID();
        }

        auto const checkpoints = m_checkpoints.listCheckpoints();
        if (!checkpoints.empty())
        {
            ImGui::Text("Iteration Snapshots");
            for (auto const & cp : checkpoints)
            {
                ImGui::PushID(cp.id.c_str());
                int w = 0;
                int h = 0;
                unsigned int const texId =
                  getOrCreatePreviewTexture("cp_" + cp.id, cp.previewPng, w, h);
                if (texId != 0)
                {
                    ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(texId)),
                                 ImVec2(48.f, 48.f));
                    ImGui::SameLine();
                }
                ImGui::Text("%s (%zu funcs)", cp.label.c_str(), cp.functionCount);
                ImGui::SameLine();
                if (ImGui::SmallButton("Restore") && m_document && m_document->getAssembly())
                {
                    if (m_checkpoints.restoreCheckpoint(cp.id, *m_document->getAssembly()))
                    {
                        m_document->update3mfModel();
                        m_document->rebuildResourceDependencyGraph();
                        m_document->markFileAsChanged();
                        if (m_onDocumentModified)
                        {
                            m_onDocumentModified();
                        }
                    }
                }
                ImGui::PopID();
            }
        }
        ImGui::End();
    }

    void AgentPanels::renderProviderSettingsPanel()
    {
        if (!ImGui::Begin("Agent Provider Settings", &m_showSettings))
        {
            ImGui::End();
            return;
        }

        char const * providers[] = {"OpenAI-compatible", "Anthropic Messages"};
        ImGui::Combo("Provider", &m_providerIndex, providers, 2);
        ImGui::InputText("Endpoint", m_endpointBuf, sizeof(m_endpointBuf));
        ImGui::InputText("Model", m_modelBuf, sizeof(m_modelBuf));
        ImGui::InputText("API Key", m_apiKeyBuf, sizeof(m_apiKeyBuf), ImGuiInputTextFlags_Password);

        int autonomy = static_cast<int>(m_policy.autonomy());
        char const * autonomyLabels[] = {
            "Ask frequently", "Ask at major design decisions", "Work autonomously"};
        if (ImGui::Combo("Autonomy", &autonomy, autonomyLabels, 3))
        {
            m_policy.setAutonomy(static_cast<agent::AutonomyLevel>(autonomy));
            if (m_configManager != nullptr)
            {
                m_configManager->setValue<int>("Agent", "autonomy", autonomy);
                m_configManager->save();
            }
        }

        ImGui::Separator();
        ImGui::Text("Privacy Controls");
        auto privacy = m_policy.privacy();
        bool changed = false;
        changed |= ImGui::Checkbox("Send conversation text", &privacy.allowConversationText);
        changed |= ImGui::Checkbox("Send model metadata", &privacy.allowModelMetadata);
        changed |= ImGui::Checkbox("Send deterministic validation results", &privacy.allowValidationResults);
        changed |= ImGui::Checkbox("Send viewport images", &privacy.allowViewportImages);
        changed |= ImGui::Checkbox("Send reference images", &privacy.allowReferenceImages);
        changed |= ImGui::Checkbox("Send raw geometry", &privacy.allowRawGeometry);
        if (changed)
        {
            m_policy.setPrivacy(privacy);
        }

        if (ImGui::Button("Save Settings"))
        {
            saveSettings();
        }
        ImGui::End();
    }
} // namespace gladius::ui
