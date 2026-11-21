#include "AgentPanels.h"

#include "../ConfigManager.h"
#include "../Document.h"
#include "../agent/providers/AnthropicProvider.h"
#include "../agent/providers/OpenAiProvider.h"

#include <imgui.h>
#include <algorithm>
#include <cstring>

namespace gladius::ui
{
    namespace
    {
        constexpr char const * kCredentialService = "gladius.design_agent";
    } // namespace

    AgentPanels::AgentPanels()
        : m_executor(m_registry,
                     [this](tools::ToolDefinition const & def, nlohmann::json const & args) {
                         return m_policy.evaluateTool(def, args);
                     })
    {
        m_toolCtx = std::make_unique<design::DesignToolContext>(
          design::DesignToolContext{m_graph, m_catalogue, m_transactions, m_checkpoints, nullptr, nullptr, nullptr});
        design::registerDesignTools(m_registry, *m_toolCtx);
    }

    AgentPanels::~AgentPanels()
    {
        stopRun();
    }

    void AgentPanels::bind(Document * document, ConfigManager * configManager)
    {
        m_document = document;
        m_configManager = configManager;
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
        for (auto const & ev : m_eventQueue.drain())
        {
            if (auto const * td = std::get_if<agent::TextDeltaEvent>(&ev))
            {
                m_streamingBuffer += td->text;
            }
            else if (auto const * ts = std::get_if<agent::ToolCallStartedEvent>(&ev))
            {
                m_activityLog.push_back({"Running " + ts->name, ts->arguments.dump(), true});
            }
            else if (auto const * tc = std::get_if<agent::ToolCallCompletedEvent>(&ev))
            {
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
                m_streamingBuffer.clear();
            }
            else if (auto const * af = std::get_if<agent::AgentFailedEvent>(&ev))
            {
                m_lastError = af->error;
                m_streamingBuffer.clear();
            }
        }
    }

    void AgentPanels::startRun(std::string const & userPrompt)
    {
        if (m_running.load() || userPrompt.empty())
        {
            return;
        }
        if (m_workerThread.joinable())
        {
            m_workerThread.join();
        }

        m_lastError.clear();
        m_streamingBuffer.clear();
        m_cancellation.reset();
        m_conversation.appendUserText(userPrompt);
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
        renderChatPanel();
        renderDesignTreePanel();
        renderVariantComparisonPanel();
        if (m_showSettings)
        {
            renderProviderSettingsPanel();
        }
    }

    void AgentPanels::renderChatPanel()
    {
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

        ImGui::Separator();

        // Collapsed routine tool summary
        if (!m_activityLog.empty())
        {
            if (ImGui::CollapsingHeader(("Activity (" + std::to_string(m_activityLog.size()) + " steps)").c_str()))
            {
                for (auto const & entry : m_activityLog)
                {
                    ImGui::TextUnformatted(entry.label.c_str());
                    if (m_showDeveloperTrace && !entry.detail.empty())
                    {
                        ImGui::TextWrapped("%s", entry.detail.c_str());
                    }
                }
            }
        }

        ImGui::BeginChild("AgentMessages", ImVec2(0, -80.f), true);
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
                    ImGui::Text("[%s] %s", roleLabel, t->text.c_str());
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
        if (!ImGui::Begin("Variant Comparison"))
        {
            ImGui::End();
            return;
        }
        if (m_graph.configurations().empty())
        {
            ImGui::TextDisabled("No system configurations generated yet.");
            ImGui::End();
            return;
        }

        for (auto const & [cid, cfg] : m_graph.configurations())
        {
            ImGui::PushID(cid.c_str());
            ImGui::Text("%s %s", cfg.name.c_str(), cfg.active ? "[Active]" : "");
            ImGui::SameLine();
            if (!cfg.active && ImGui::Button("Activate"))
            {
                m_graph.activateConfiguration(cid);
            }
            for (auto const & p : cfg.evaluation.deterministicPassed)
            {
                ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.f), "OK: %s", p.c_str());
            }
            for (auto const & w : cfg.evaluation.deterministicWarnings)
            {
                ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f), "WARN: %s", w.c_str());
            }
            ImGui::Separator();
            ImGui::PopID();
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
