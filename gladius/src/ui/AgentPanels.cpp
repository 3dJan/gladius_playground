#include "AgentPanels.h"
#include "../design/ManufacturingValidator.h"

#include "ClipboardImageHelper.h"
#include "../ConfigManager.h"
#include "../Document.h"
#include "../ExpressionParser.h"
#include "../ExpressionToGraphConverter.h"
#include "../ProgramSnippetApplier.h"
#include "../agent/providers/AnthropicProvider.h"
#include "../agent/providers/FallbackProvider.h"
#include "../agent/providers/OpenAiProvider.h"
#include "../agent/trace/AgentTrace.h"
#include "../agent/trace/AgentTraceProfile.h"
#include "../design/Renderability.h"
#if defined(GLADIUS_ENABLE_OPENCL)
#include "../compute/ComputeCore.h"
#endif

#ifndef __EMSCRIPTEN__
#include <sago/platform_folders.h>
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
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace gladius::ui
{
    namespace
    {
        constexpr char const * kCredentialService = "gladius.design_agent";
        constexpr int kDefaultWallTimeMinutes = 15;
        constexpr int kDefaultMaxModelTurns = 64;

        std::chrono::milliseconds wallTimeFromMinutes(int minutes)
        {
            return std::chrono::milliseconds(std::max(0, minutes) * 60000);
        }

        int minutesFromWallTime(std::chrono::milliseconds const duration)
        {
            return static_cast<int>(duration.count() / 60000);
        }

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
        m_knowledge.setStoragePath(agent::KnowledgeStore::defaultStoragePath());
        (void) m_knowledge.load();
        (void) m_catalogue.loadDefaultCatalogue();

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
          design::ManufacturingValidator::makeValidationCallback(m_toolCtx->document, m_catalogue),
          [this]() { m_documentModifiedPending.store(true); },
          &m_skills,
          &m_todos,
          &m_knowledge,
          false,
          0,
          0,
          [this](agent::ImageGenerationRequest const & req,
                 io::CancellationToken const & cancellation)
            -> coro::task<agent::ImageGenerationResponse> {
              auto provider = agent::createImageProvider(m_imageProviderConfig);
              auto resp = co_await provider->generate(req, cancellation);
              if (resp.success && !resp.image.data.empty())
              {
                  m_eventQueue.push(agent::ImageGeneratedEvent{resp.image, req.prompt});
              }
              co_return resp;
          },
          // Autonomy can change between runs, so the policy is read when a question arrives.
          [this](agent::UserQuestion const & question, io::CancellationToken const & cancellation)
            -> coro::task<agent::UserDecisionResult> {
              co_return co_await agent::makeAskUserCallback(m_userDecisions, decisionPolicy())(
                question, cancellation);
          },
          0});
        design::registerDesignTools(m_registry, *m_toolCtx);
    }

    AgentPanels::~AgentPanels()
    {
        stopRun();
#ifdef __EMSCRIPTEN__
        m_webModelFetchTask.reset();
#else
        if (m_modelFetchThread.joinable())
        {
            m_modelFetchThread.join();
        }
#endif
        saveSessionState();
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

    namespace
    {
        std::filesystem::path sessionStatePath()
        {
#ifdef __EMSCRIPTEN__
            return std::filesystem::path{"/gladius"} / "agent_session.json";
#else
            return std::filesystem::path{sago::getConfigHome()} / "gladius" / "agent_session.json";
#endif
        }

        std::filesystem::path sidecarStatePath(std::filesystem::path const & docPath)
        {
            return std::filesystem::path(docPath.string() + ".gladius_agent.json");
        }

        /**
         * @brief Creates a run recorder when `GLADIUS_AGENT_TRACE` names an output file.
         * @details Lets an interactive session produce the same trace the benchmark writes, so a
         *          harness problem seen live can be profiled without re-running it as a test.
         */
#ifndef __EMSCRIPTEN__
        std::unique_ptr<agent::trace::AgentTraceRecorder> makeSessionTraceRecorder()
        {
            char const * const path = std::getenv("GLADIUS_AGENT_TRACE");
            if (path == nullptr || *path == '\0')
            {
                return nullptr;
            }
            auto recorder =
              std::make_unique<agent::trace::AgentTraceRecorder>(std::filesystem::path{path});
            if (char const * const dump = std::getenv("GLADIUS_AGENT_TRACE_PROMPT");
                dump != nullptr && *dump == '1')
            {
                recorder->setPromptDumpEnabled(true);
            }
            recorder->setRunId("interactive");
            recorder->recordRunMeta({{"source", "interactive"}});
            return recorder;
        }

        /// @brief Writes the Perfetto timeline and logs the profile of an interactive session trace
        void finishSessionTrace(agent::trace::AgentTraceRecorder & recorder)
        {
            auto const profile = agent::trace::analyzeTrace(recorder.records());
            std::string timelineError;
            recorder.writeTimeline(recorder.timelinePath(), timelineError);
            std::clog << "[agent-trace] " << agent::trace::formatProfileSummary(profile) << '\n';
            if (!timelineError.empty())
            {
                std::clog << "[agent-trace] " << timelineError << '\n';
            }
        }
#endif
    } // namespace

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
        m_artifacts.bind(m_graph,
                         m_todos,
                         m_checkpoints,
                         document,
                         [this]()
                         {
                             if (m_onDocumentModified)
                             {
                                 m_onDocumentModified();
                             }
                         },
                         [this](std::string const & key,
                                std::vector<std::byte> const & pngBytes,
                                int & outW,
                                int & outH)
                         { return getOrCreatePreviewTexture(key, pngBytes, outW, outH); });
        loadSettings();
        loadSessionState();
    }

    bool AgentPanels::attachImageFile(std::filesystem::path const & path)
    {
        auto bytes = readBinaryFile(path.string());
        if (bytes.empty())
        {
            return false;
        }
        std::string const key = "att_" + std::to_string(m_nextAttachmentId++);
        m_pendingImages.push_back(PendingImageAttachment{
          path.filename().string(),
          std::move(bytes),
          mimeTypeForPath(path.string()),
          "",
          key});
        return true;
    }

    bool AgentPanels::attachPromptTextFile(std::filesystem::path const & path)
    {
        std::string error;
        if (!m_prompt.appendTextFile(path, error))
        {
            m_lastError = error;
            return false;
        }
        m_lastError.clear();
        return true;
    }

    void AgentPanels::saveSessionState()
    {
        try
        {
            auto const path = sessionStatePath();
            std::error_code ec;
            if (!path.parent_path().empty())
            {
                std::filesystem::create_directories(path.parent_path(), ec);
            }
            nlohmann::json state;
            state["version"] = 1;
            state["designGraph"] = m_graph.toJson();
            state["conversation"] = m_conversation.toJson();
            state["todos"] = m_todos.toJson();
            if (m_document && m_document->getAssembly())
            {
                state["programSnippet"] =
                  ExpressionToGraphConverter::convertProgramToSnippet(*m_document->getAssembly());
                if (auto const docFile = m_document->getCurrentAssemblyFilename())
                {
                    state["documentPath"] = docFile->string();
                }
            }
            std::ofstream out(path, std::ios::trunc);
            if (out)
            {
                out << state.dump(2);
            }
        }
        catch (...)
        {
        }
    }

    void AgentPanels::onNewDocument()
    {
        m_graph.clear();
        m_conversation.clear();
        m_todos.clear();
        m_checkpoints.clear();
        m_activityLog.clear();
        m_lastError.clear();
        m_lastProviderError = {};
        m_lastAgentMessage.clear();
        m_awaitingUserReply = false;
        m_replyExpanded = false;
        m_streamingBuffer.clear();
    }

    void AgentPanels::loadSessionState()
    {
        try
        {
            if (!m_document || !m_document->getCurrentAssemblyFilename().has_value())
            {
                return;
            }
            std::string const currentDocPath =
              m_document->getCurrentAssemblyFilename()->string();
            if (currentDocPath.empty())
            {
                return;
            }
            auto const path = sessionStatePath();
            if (!std::filesystem::exists(path))
            {
                return;
            }
            std::ifstream in(path);
            if (!in)
            {
                return;
            }
            nlohmann::json state;
            in >> state;
            if (!state.is_object())
            {
                return;
            }
            if (state.value("documentPath", "") != currentDocPath)
            {
                return;
            }
            if (state.contains("designGraph"))
            {
                (void) m_graph.fromJson(state["designGraph"]);
            }
            if (state.contains("conversation"))
            {
                (void) m_conversation.fromJson(state["conversation"]);
            }
            if (state.contains("todos"))
            {
                (void) m_todos.fromJson(state["todos"]);
            }
        }
        catch (...)
        {
        }
    }

    void AgentPanels::onDocumentSaved(std::filesystem::path const & docPath)
    {
        if (docPath.empty())
        {
            return;
        }
        try
        {
            nlohmann::json state = {
              {"version", 1},
              {"designGraph", m_graph.toJson()},
              {"conversation", m_conversation.toJson()},
              {"todos", m_todos.toJson()}};
            std::ofstream out(sidecarStatePath(docPath), std::ios::trunc);
            if (out)
            {
                out << state.dump(2);
            }
        }
        catch (...)
        {
        }
        saveSessionState();
    }

    void AgentPanels::onDocumentOpened(std::filesystem::path const & docPath)
    {
        onNewDocument();
        if (docPath.empty())
        {
            return;
        }
        try
        {
            auto const sidecar = sidecarStatePath(docPath);
            if (std::filesystem::exists(sidecar))
            {
                std::ifstream in(sidecar);
                if (in)
                {
                    nlohmann::json state;
                    in >> state;
                    if (state.contains("designGraph"))
                    {
                        (void) m_graph.fromJson(state["designGraph"]);
                    }
                    if (state.contains("conversation"))
                    {
                        (void) m_conversation.fromJson(state["conversation"]);
                    }
                    if (state.contains("todos"))
                    {
                        (void) m_todos.fromJson(state["todos"]);
                    }
                    return;
                }
            }

            // Fallback: restore from last session only if it matches the opened document path
            auto const sessPath = sessionStatePath();
            if (std::filesystem::exists(sessPath))
            {
                std::ifstream in(sessPath);
                if (in)
                {
                    nlohmann::json state;
                    in >> state;
                    if (state.is_object() && state.value("documentPath", "") == docPath.string())
                    {
                        if (state.contains("designGraph"))
                        {
                            (void) m_graph.fromJson(state["designGraph"]);
                        }
                        if (state.contains("conversation"))
                        {
                            (void) m_conversation.fromJson(state["conversation"]);
                        }
                        if (state.contains("todos"))
                        {
                            (void) m_todos.fromJson(state["todos"]);
                        }
                    }
                }
            }
        }
        catch (...)
        {
        }
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
          m_configManager->getValue<std::string>("Agent", "endpoint", "");
        std::string const mdl = m_configManager->getValue<std::string>("Agent", "model", "");
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

        m_limits.maxWallTime =
          wallTimeFromMinutes(m_configManager->getValue<int>(
            "Agent", "maxWallTimeMinutes", kDefaultWallTimeMinutes));
        m_limits.maxModelTurns =
          std::max(0, m_configManager->getValue<int>("Agent", "maxModelTurns", kDefaultMaxModelTurns));

        m_maxOutputTokens = std::max(1,
          m_configManager->getValue<int>("Agent", "maxOutputTokens", kDefaultMaxOutputTokens));
        m_thinkingTokens =
          m_configManager->getValue<int>("Agent", "thinkingTokens", kDefaultThinkingTokens);
        m_requestTimeoutSeconds = std::max(1,
          m_configManager->getValue<int>(
            "Agent", "requestTimeoutSeconds", kDefaultRequestTimeoutSeconds));
        m_cacheBreakpointMode = m_configManager->getValue<std::string>("Agent", "cacheBreakpoints", "auto");

        std::string const account = (m_providerIndex == 1) ? "anthropic" : "openai";
        if (auto key = m_credentialStore.loadSecret(kCredentialService, account))
        {
            std::snprintf(m_apiKeyBuf, sizeof(m_apiKeyBuf), "%s", key->c_str());
        }

        m_imageProviderConfig.apiType = static_cast<agent::ImageApiType>(std::clamp(
          m_configManager->getValue<int>("AgentImageProvider", "apiType", 0), 0, 2));
        m_imageProviderConfig.endpoint =
          m_configManager->getValue<std::string>("AgentImageProvider", "endpoint", "");
        m_imageProviderConfig.model =
          m_configManager->getValue<std::string>("AgentImageProvider", "model", "");
        m_imageProviderConfig.defaultSize =
          m_configManager->getValue<std::string>("AgentImageProvider", "defaultSize", "1024x1024");
        if (auto imgKey = m_credentialStore.loadSecret(kCredentialService, "image_provider"))
        {
            m_imageProviderConfig.apiKey = *imgKey;
        }

        // Load multi-provider profiles (or seed defaults from legacy config)
        m_providerProfiles.clear();
        auto const profilesStr =
          m_configManager->getValue<std::string>("Agent", "providerProfiles", "");
        if (!profilesStr.empty())
        {
            auto parsed = nlohmann::json::parse(profilesStr, nullptr, false);
            if (parsed.is_array())
            {
                for (auto const & pj : parsed)
                {
                    auto prof = agent::ProviderProfile::fromJson(pj);
                    if (!prof.id.empty())
                    {
                        if (auto k = m_credentialStore.loadSecret(
                              kCredentialService, "profile_" + prof.id))
                        {
                            prof.apiKey = *k;
                        }
                        m_providerProfiles.push_back(std::move(prof));
                    }
                }
            }
        }
        if (!m_providerProfiles.empty()
            && !m_configManager->getValue<bool>("Agent", "legacySeedModelsCleaned", false))
        {
            static constexpr char const * kLegacySeedModels[] = {
              "gpt-4.1",
              "o3-mini",
              "claude-sonnet-4-20250514",
              "claude-3-7-sonnet-latest",
              "anthropic/claude-sonnet-4",
              "openai/gpt-4o",
              "google/gemini-2.5-pro"};
            for (auto & prof : m_providerProfiles)
            {
                if (prof.id == "openai_default" || prof.id == "anthropic_default"
                    || prof.id == "openrouter_default")
                {
                    prof.models.erase(
                      std::remove_if(prof.models.begin(),
                                     prof.models.end(),
                                     [](std::string const & m) {
                                         for (auto const * legacy : kLegacySeedModels)
                                         {
                                             if (m == legacy)
                                             {
                                                 return true;
                                             }
                                         }
                                         return false;
                                     }),
                      prof.models.end());
                }
            }
            m_configManager->setValue<bool>("Agent", "legacySeedModelsCleaned", true);
            saveSettings();
        }
        if (m_providerProfiles.empty())
        {
            if (!ep.empty() || !mdl.empty() || m_apiKeyBuf[0] != '\0')
            {
                std::string const migratedName =
                  agent::isLocalEndpoint(ep)
                    ? "Local"
                    : (agent::isOpenRouterEndpoint(ep) ? "OpenRouter"
                                                       : ((m_providerIndex == 1) ? "Anthropic"
                                                                                 : "Custom Provider"));
                std::vector<std::string> migratedModels;
                if (!mdl.empty())
                {
                    migratedModels.push_back(mdl);
                }
                m_providerProfiles.push_back(agent::ProviderProfile{
                  "migrated_default",
                  migratedName,
                  (m_providerIndex == 1) ? agent::ApiType::Anthropic
                                         : agent::ApiType::OpenAiCompatible,
                  ep,
                  std::move(migratedModels),
                  std::string(m_apiKeyBuf)});
            }
            // No provider is seeded by default: every provider needs an API key anyway, so an empty
            // list is an honest state. The composer offers "Configure providers" to fill it.
        }

        auto const primStr = m_configManager->getValue<std::string>("Agent", "primarySelection", "");
        if (!primStr.empty())
        {
            auto pj = nlohmann::json::parse(primStr, nullptr, false);
            if (pj.is_object())
            {
                m_primarySelection = agent::ModelSelection::fromJson(pj);
            }
        }
        bool hasAnyAvailableModel = false;
        for (auto const & p : m_providerProfiles)
        {
            for (auto const & m : p.models)
            {
                if (p.isModelAvailable(m))
                {
                    hasAnyAvailableModel = true;
                    if (m_primarySelection.profileId.empty())
                    {
                        m_primarySelection = agent::ModelSelection{p.id, m};
                    }
                    break;
                }
            }
            if (hasAnyAvailableModel && !m_primarySelection.profileId.empty())
            {
                break;
            }
        }

        m_fallbackSelections.clear();
        auto const fbStr = m_configManager->getValue<std::string>("Agent", "fallbackSelections", "");
        if (!fbStr.empty())
        {
            auto fj = nlohmann::json::parse(fbStr, nullptr, false);
            if (fj.is_array())
            {
                for (auto const & item : fj)
                {
                    auto sel = agent::ModelSelection::fromJson(item);
                    if (!sel.profileId.empty() && !sel.model.empty())
                    {
                        m_fallbackSelections.push_back(std::move(sel));
                    }
                }
            }
        }
    }

    namespace
    {
        /// @brief Copies the panel's per-request tunables onto a provider config
        void applyRequestTuning(agent::ProviderConfig & config,
                                agent::ApiType apiType,
                                int maxOutputTokens,
                                int thinkingTokens,
                                int timeoutSeconds,
                                std::string const & cacheBreakpointMode)
        {
            config.maxTokens = maxOutputTokens;
            // Anthropic only emits `thinking` while the budget is below the completion limit, so a
            // higher cap would silently switch reasoning off instead of widening it.
            config.thinkingBudgetTokens = std::min(thinkingTokens, maxOutputTokens - 1);
            config.timeoutSeconds = timeoutSeconds;
            if (cacheBreakpointMode == "all")
            {
                config.cacheBreakpoints = agent::CacheBreakpointPlacement::CacheAll;
            }
            else if (cacheBreakpointMode == "tools")
            {
                config.cacheBreakpoints = agent::CacheBreakpointPlacement::Tools;
            }
            else if (cacheBreakpointMode == "none")
            {
                config.cacheBreakpoints = agent::CacheBreakpointPlacement::None;
            }
            else // "auto": only pay for a breakpoint where the API honours it
            {
                config.cacheBreakpoints = apiType == agent::ApiType::Anthropic
                                            ? agent::CacheBreakpointPlacement::Tools
                                            : agent::CacheBreakpointPlacement::None;
            }
        }
    } // namespace

    void AgentPanels::saveSettings()
    {
        if (m_configManager != nullptr)
        {
            m_configManager->setValue<int>("Agent", "providerIndex", m_providerIndex);
            m_configManager->setValue<int>(
              "Agent", "autonomy", static_cast<int>(m_policy.autonomy()));
            m_configManager->setValue<std::string>("Agent", "endpoint", m_endpointBuf);
            m_configManager->setValue<std::string>("Agent", "model", m_modelBuf);
            m_configManager->setValue<int>(
              "Agent", "maxWallTimeMinutes", minutesFromWallTime(m_limits.maxWallTime));
            m_configManager->setValue<int>("Agent", "maxModelTurns", m_limits.maxModelTurns);
            m_configManager->setValue<int>("Agent", "maxOutputTokens", m_maxOutputTokens);
            m_configManager->setValue<int>("Agent", "thinkingTokens", m_thinkingTokens);
            m_configManager->setValue<int>(
              "Agent", "requestTimeoutSeconds", m_requestTimeoutSeconds);
            m_configManager->setValue<std::string>(
              "Agent", "cacheBreakpoints", m_cacheBreakpointMode);

            nlohmann::json profilesArr = nlohmann::json::array();
            m_credentialWarning.clear();
            for (auto const & prof : m_providerProfiles)
            {
                profilesArr.push_back(prof.toJson());
                if (!prof.apiKey.empty())
                {
                    if (!m_credentialStore.storeSecret(
                          kCredentialService, "profile_" + prof.id, prof.apiKey))
                    {
                        m_credentialWarning =
                          "Warning: OS credential store (" + m_credentialStore.backendName()
                          + ") failed to persist key for " + prof.name;
                    }
                }
                else
                {
                    (void) m_credentialStore.deleteSecret(
                      kCredentialService, "profile_" + prof.id);
                }
            }
            m_configManager->setValue<std::string>(
              "Agent", "providerProfiles", profilesArr.dump());
            m_configManager->setValue<std::string>(
              "Agent", "primarySelection", m_primarySelection.toJson().dump());
            nlohmann::json fbArr = nlohmann::json::array();
            for (auto const & fb : m_fallbackSelections)
            {
                fbArr.push_back(fb.toJson());
            }
            m_configManager->setValue<std::string>("Agent", "fallbackSelections", fbArr.dump());

            auto const & privacy = m_policy.privacy();
            m_configManager->setValue<bool>("AgentPrivacy", "allowConversationText", privacy.allowConversationText);
            m_configManager->setValue<bool>("AgentPrivacy", "allowModelMetadata", privacy.allowModelMetadata);
            m_configManager->setValue<bool>("AgentPrivacy", "allowValidationResults", privacy.allowValidationResults);
            m_configManager->setValue<bool>("AgentPrivacy", "allowViewportImages", privacy.allowViewportImages);
            m_configManager->setValue<bool>("AgentPrivacy", "allowReferenceImages", privacy.allowReferenceImages);
            m_configManager->setValue<bool>("AgentPrivacy", "allowRawGeometry", privacy.allowRawGeometry);

            m_configManager->setValue<int>(
              "AgentImageProvider", "apiType", static_cast<int>(m_imageProviderConfig.apiType));
            m_configManager->setValue<std::string>(
              "AgentImageProvider", "endpoint", m_imageProviderConfig.endpoint);
            m_configManager->setValue<std::string>(
              "AgentImageProvider", "model", m_imageProviderConfig.model);
            m_configManager->setValue<std::string>(
              "AgentImageProvider", "defaultSize", m_imageProviderConfig.defaultSize);
            if (!m_imageProviderConfig.apiKey.empty())
            {
                (void) m_credentialStore.storeSecret(
                  kCredentialService, "image_provider", m_imageProviderConfig.apiKey);
            }
            else
            {
                (void) m_credentialStore.deleteSecret(kCredentialService, "image_provider");
            }

            m_configManager->save();
        }
        std::string const account = (m_providerIndex == 1) ? "anthropic" : "openai";
        if (m_apiKeyBuf[0] != '\0')
        {
            m_credentialStore.storeSecret(kCredentialService, account, m_apiKeyBuf);
        }
    }

    void AgentPanels::startModelFetch(std::string const & profileId,
                                    bool freeOnly,
                                    bool autoAddToProfile)
    {
        if (m_modelFetchRunning.load())
        {
            return;
        }
        auto pit = std::find_if(m_providerProfiles.begin(),
                                m_providerProfiles.end(),
                                [&](agent::ProviderProfile const & p) { return p.id == profileId; });
        if (pit == m_providerProfiles.end())
        {
            return;
        }
#ifndef __EMSCRIPTEN__
        if (m_modelFetchThread.joinable())
        {
            m_modelFetchThread.join();
        }
#else
        m_webModelFetchTask.reset();
#endif
        agent::ProviderProfile profileCopy = *pit;
        m_modelFetchStatus = "Fetching models for " + profileCopy.name + "...";
        m_modelFetchRunning.store(true);
#ifdef __EMSCRIPTEN__
        m_webModelFetchTask = runModelFetchWeb(std::move(profileCopy), freeOnly, autoAddToProfile);
        m_webModelFetchTask->resume();
#else
        m_modelFetchThread = std::thread([this, profileCopy, freeOnly, autoAddToProfile]() {
            agent::CurlHttpTransport transport;
            io::CancellationToken cancel;
            auto res = agent::ModelDiscovery::fetchModels(profileCopy, transport, cancel, freeOnly);
            {
                std::lock_guard<std::mutex> lock(m_modelFetchMutex);
                m_modelFetchProfileId = profileCopy.id;
                m_modelFetchAutoAdd = autoAddToProfile;
                m_modelFetchResult = std::move(res);
                m_modelFetchPendingApply = true;
            }
            m_modelFetchRunning.store(false);
        });
#endif
    }

#ifdef __EMSCRIPTEN__
    coro::task<void> AgentPanels::runModelFetchWeb(agent::ProviderProfile profileCopy,
                                                   bool freeOnly,
                                                   bool autoAddToProfile)
    {
        agent::CurlHttpTransport transport;
        io::CancellationToken cancel;
        auto res = co_await agent::ModelDiscovery::fetchModelsAsync(
          profileCopy, transport, cancel, freeOnly);
        {
            std::lock_guard<std::mutex> lock(m_modelFetchMutex);
            m_modelFetchProfileId = profileCopy.id;
            m_modelFetchAutoAdd = autoAddToProfile;
            m_modelFetchResult = std::move(res);
            m_modelFetchPendingApply = true;
        }
        m_modelFetchRunning.store(false);
        co_return;
    }
#endif

    void AgentPanels::pollModelFetch()
    {
#ifdef __EMSCRIPTEN__
        if (m_webModelFetchTask && m_webModelFetchTask->is_ready())
        {
            m_webModelFetchTask.reset();
        }
#endif
        bool apply = false;
        bool autoAdd = false;
        std::string targetId;
        agent::ModelDiscoveryResult res;
        {
            std::lock_guard<std::mutex> lock(m_modelFetchMutex);
            if (m_modelFetchPendingApply)
            {
                apply = true;
                autoAdd = m_modelFetchAutoAdd;
                targetId = m_modelFetchProfileId;
                res = std::move(m_modelFetchResult);
                m_modelFetchPendingApply = false;
            }
        }
        if (!apply)
        {
            return;
        }
        if (res.success)
        {
            auto pit = std::find_if(m_providerProfiles.begin(),
                                    m_providerProfiles.end(),
                                    [&](agent::ProviderProfile const & p) { return p.id == targetId; });
            if (pit != m_providerProfiles.end())
            {
                for (auto const & meta : res.metadata)
                {
                    pit->modelMetadata[meta.id] = meta;
                }
                m_queriedModelsProfileId = targetId;
                m_queriedModels = res.metadata;

                if (autoAdd)
                {
                    for (auto const & m : res.models)
                    {
                        if (std::find(pit->models.begin(), pit->models.end(), m) == pit->models.end())
                        {
                            pit->models.push_back(m);
                        }
                    }
                    if (m_primarySelection.profileId.empty() && !pit->models.empty())
                    {
                        for (auto const & m : pit->models)
                        {
                            if (pit->isModelAvailable(m))
                            {
                                m_primarySelection = agent::ModelSelection{pit->id, m};
                                break;
                            }
                        }
                    }
                }
                saveSettings();
                m_modelFetchStatus = "Queried " + std::to_string(res.metadata.size())
                                     + " models for " + pit->name
                                     + (autoAdd ? " (added)" : " (select below to add)");
            }
        }
        else
        {
            m_modelFetchStatus = "Model fetch failed: " + res.error;
        }
    }

    void AgentPanels::drainEvents()
    {
        pollModelFetch();
#ifdef __EMSCRIPTEN__
        if (m_webRunTask && m_webRunTask->is_ready())
        {
            m_webRunTask.reset();
        }
        if (m_webConfirmHandle && m_confirmationResolved)
        {
            auto h = std::exchange(m_webConfirmHandle, {});
            h.resume();
        }
        // A answered design question parks the run coroutine on this thread too.
        if (auto h = m_userDecisions.takeResumable())
        {
            h.resume();
        }
        {
            ClipboardImageResult webClip;
            if (ClipboardImageHelper::consumePendingWebImage(webClip) && webClip.success)
            {
                std::string const key = "att_" + std::to_string(m_nextAttachmentId++);
                m_pendingImages.push_back(PendingImageAttachment{
                  std::move(webClip.filename),
                  std::move(webClip.pngBytes),
                  std::move(webClip.mimeType),
                  "",
                  key});
            }
        }
#endif
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
            else if (auto const * ig = std::get_if<agent::ImageGeneratedEvent>(&ev))
            {
                m_activityLog.push_back({"Generated reference image", ig->caption, true});
            }
            else if (auto const * ud = std::get_if<agent::UserDecisionRequiredEvent>(&ev))
            {
                std::lock_guard<std::mutex> lock(m_confirmMutex);
                m_pendingConfirmPrompt = ud->prompt;
                m_awaitingConfirmation = true;
                m_confirmationResolved = false;
            }
            else if (auto const * ac = std::get_if<agent::AgentCompletedEvent>(&ev))
            {
                m_activeToolName.clear();
                m_streamingBuffer.clear();
                m_lastAgentMessage = ac->summary;
                m_awaitingUserReply = ac->awaitingUserReply;
                saveSessionState();
            }
            else if (auto const * af = std::get_if<agent::AgentFailedEvent>(&ev))
            {
                m_activeToolName.clear();
                m_lastError = af->error;
                m_lastProviderError = af->errorInfo;
                m_streamingBuffer.clear();
                saveSessionState();
            }
        }
    }

    agent::DecisionPolicy AgentPanels::decisionPolicy() const
    {
        // Questions are only useful when a human is watching the panel: a run that works on its own
        // must take the recommendation instead of parking forever.
        return m_policy.autonomy() == agent::AutonomyLevel::WorkAutonomously
                 ? agent::DecisionPolicy::UseRecommendation
                 : agent::DecisionPolicy::AskUser;
    }

    agent::DesignContextSnapshot AgentPanels::buildContextSnapshot(std::string const & knowledgeQuery)
    {
        agent::DesignContextSnapshot snap;
        snap.autonomy = m_policy.autonomy();
        snap.isIncremental = (m_conversation.size() > 1)
                             || (m_document && m_document->getAssembly()
                                 && m_document->getAssembly()->getFunctions().size() > 1);
        snap.isFollowUpRequest = m_followUpRequest && m_graph.phase() != design::DesignPhase::Complete;
        if (snap.isIncremental && m_document && m_document->getAssembly())
        {
            std::string funcList;
            for (auto const & [fid, model] : m_document->getAssembly()->getFunctions())
            {
                if (model)
                {
                    funcList += "- " + model->getDisplayName().value_or(std::to_string(fid))
                                + " (id=" + std::to_string(fid) + ")\n";
                }
            }
            snap.existingModelSummary = std::move(funcList);
        }
        for (auto const & req : m_graph.requirements())
        {
            snap.requirementsSummary += "- " + req.id + " [" + req.category
                                        + (req.mandatory ? ", mandatory" : ", optional") + "] \""
                                        + req.description + "\"\n";
        }
        for (auto const & ch : m_graph.consumePendingRequirementChanges())
        {
            snap.requirementChangesSummary += "- " + ch + "\n";
        }
        snap.designPhase = design::toString(m_graph.phase());
        if (m_document != nullptr)
        {
            // Cheaper for the model than discovering it at finish_design or in a blank viewport
            // screenshot: the reason is stated before the next snippet is written.
            auto const facts = design::collectRenderabilityFacts(m_document);
            if (!facts.isRenderable())
            {
                snap.renderabilitySummary = facts.blockersAsText();
            }
        }
        snap.designGraphSummary = m_graph.summarize();
        snap.availableSkillsSummary = m_skills.summarizeForPrompt();
        snap.todoSummary = m_todos.summarize();
        snap.knowledgeSummary = m_knowledge.summarizeForPrompt(knowledgeQuery, 5);
        return snap;
    }

    bool AgentPanels::hasUsableModel() const
    {
        // The legacy single-provider config still counts: a run falls back to it when the profile
        // list has nothing usable.
        if (m_modelBuf[0] != '\0')
        {
            return true;
        }
        for (auto const & prof : m_providerProfiles)
        {
            for (auto const & model : prof.models)
            {
                if (prof.isModelAvailable(model))
                {
                    return true;
                }
            }
        }
        return false;
    }

    void AgentPanels::startRun(std::string const & userPrompt)
    {
        if (m_running.load() || (userPrompt.empty() && m_pendingImages.empty()))
        {
            return;
        }
#ifndef __EMSCRIPTEN__
        if (m_workerThread.joinable())
        {
            m_workerThread.join();
        }
#else
        m_webConfirmHandle = {};
        m_webRunTask.reset();
#endif

        // Refuse before anything is consumed when no provider can answer: the prompt stays in the
        // field and the panel shows why nothing started.
        if (!hasUsableModel())
        {
            std::string const text = "No provider or model is ready to answer";
            m_lastError = text;
            m_lastProviderError = agent::classifyErrorText(text, true);
            return;
        }

        m_lastError.clear();
        m_lastProviderError = {};
        m_lastAgentMessage.clear();
        m_awaitingUserReply = false;
        m_replyExpanded = false;
        m_streamingBuffer.clear();
        m_activeToolName.clear();
        m_runStartTime = std::chrono::steady_clock::now();
        m_cancellation.reset();
        m_runControl.reset();
        m_workflowAnimation.reset();
        m_userDecisions.abandon("Previous run ended");
        m_userDecisionText.clear();
        if (m_toolCtx)
        {
            m_toolCtx->userQuestionsAsked = 0;
        }

        // A prompt that arrives after the design was finished extends the requirements: re-open the
        // design so the new request is iterated on instead of being reported as already done.
        m_followUpRequest = m_graph.reopenForFollowUpRequest(userPrompt);

        agent::Message msg;
        msg.role = agent::MessageRole::User;
        if (!userPrompt.empty())
        {
            msg.parts.emplace_back(agent::TextContent{userPrompt});
        }
        for (auto & img : m_pendingImages)
        {
            if (!img.comment.empty())
            {
                msg.parts.emplace_back(agent::TextContent{
                  "[Attached image '" + img.filename + "' comment: " + img.comment + "]"});
            }
            msg.parts.emplace_back(agent::ImageContent{std::move(img.bytes), std::move(img.mimeType)});
        }
        m_pendingImages.clear();
        m_inspectedImageIndex = -1;
        m_conversation.append(std::move(msg));
        m_running.store(true);

        // Build ordered list of provider candidates: primary first, then fallbacks
        std::vector<agent::ModelSelection> selections;
        if (!m_primarySelection.profileId.empty() && !m_primarySelection.model.empty())
        {
            selections.push_back(m_primarySelection);
        }
        for (auto const & fb : m_fallbackSelections)
        {
            if (std::find(selections.begin(), selections.end(), fb) == selections.end())
            {
                selections.push_back(fb);
            }
        }

        auto const profilesCopy = m_providerProfiles;
        int const legacyProviderIdx = m_providerIndex;
        // Snapshot the tunables: the worker thread must not read panel fields the UI can still change.
        int const maxOutputTokens = m_maxOutputTokens;
        int const thinkingTokens = m_thinkingTokens;
        int const requestTimeoutSeconds = m_requestTimeoutSeconds;
        std::string const cacheBreakpointMode = m_cacheBreakpointMode;
        auto tune = [maxOutputTokens,
                     thinkingTokens,
                     requestTimeoutSeconds,
                     cacheBreakpointMode](agent::ProviderConfig & config, agent::ApiType apiType) {
            applyRequestTuning(config,
                               apiType,
                               maxOutputTokens,
                               thinkingTokens,
                               requestTimeoutSeconds,
                               cacheBreakpointMode);
        };
        agent::ProviderConfig legacyCfg;
        legacyCfg.endpoint = m_endpointBuf;
        legacyCfg.model = m_modelBuf;
        legacyCfg.apiKey = m_apiKeyBuf;
        legacyCfg.stream = true;
        tune(legacyCfg, legacyProviderIdx == 1 ? agent::ApiType::Anthropic
                                               : agent::ApiType::OpenAiCompatible);
        std::string const promptForKnowledge = userPrompt;

#ifdef __EMSCRIPTEN__
        m_webRunTask = runAgentWeb(std::move(selections),
                                   std::move(profilesCopy),
                                   std::move(legacyCfg),
                                   legacyProviderIdx,
                                   promptForKnowledge,
                                   m_maxOutputTokens,
                                   m_thinkingTokens,
                                   m_requestTimeoutSeconds,
                                   m_cacheBreakpointMode);
        m_webRunTask->resume();
#else
        m_workerThread = std::thread(
          [this, selections, profilesCopy, legacyCfg, legacyProviderIdx, promptForKnowledge, tune]() {
              auto makeProvider = [](agent::ApiType apiType, agent::ProviderConfig const & c)
                -> std::unique_ptr<agent::IAiProvider> {
                  if (apiType == agent::ApiType::Anthropic)
                  {
                      return std::make_unique<agent::AnthropicProvider>(c);
                  }
                  return std::make_unique<agent::OpenAiProvider>(c);
              };

              std::vector<agent::FallbackProvider::Candidate> candidates;
              for (auto const & sel : selections)
              {
                  auto pit = std::find_if(profilesCopy.begin(),
                                          profilesCopy.end(),
                                          [&](agent::ProviderProfile const & p) {
                                              return p.id == sel.profileId;
                                          });
                  if (pit == profilesCopy.end() || !pit->isModelAvailable(sel.model))
                  {
                      continue;
                  }
                  agent::ProviderConfig cfg;
                  cfg.endpoint = pit->endpoint;
                  cfg.apiKey = pit->apiKey;
                  cfg.model = sel.model;
                  cfg.stream = true;
                  tune(cfg, pit->apiType);
                  candidates.push_back(agent::FallbackProvider::Candidate{
                    pit->name + "/" + sel.model, makeProvider(pit->apiType, cfg)});
              }
              if (candidates.empty())
              {
                  auto apiType = (legacyProviderIdx == 1) ? agent::ApiType::Anthropic
                                                          : agent::ApiType::OpenAiCompatible;
                  candidates.push_back(agent::FallbackProvider::Candidate{
                    legacyCfg.model, makeProvider(apiType, legacyCfg)});
              }

              agent::FallbackProvider provider(std::move(candidates));
              agent::AgentLoop loop;
              auto trace = makeSessionTraceRecorder();
              (void) coro::sync_wait(loop.run(
                m_conversation,
                provider,
                m_registry,
                m_executor,
                m_contextBuilder,
                m_policy,
                m_eventQueue,
                m_limits,
                m_cancellation,
                [this]() { return m_graph.phase() == design::DesignPhase::Complete; },
                [this](tools::ToolDefinition const &,
                       nlohmann::json const &) -> coro::task<bool> {
                    std::unique_lock<std::mutex> lock(m_confirmMutex);
                    m_confirmCv.wait(lock, [this]() {
                        return m_confirmationResolved || m_cancellation.isCancelled();
                    });
                    bool const ok = m_confirmationApproved && !m_cancellation.isCancelled();
                    m_awaitingConfirmation = false;
                    m_confirmationResolved = false;
                    co_return ok;
                },
                [this, promptForKnowledge]() { return buildContextSnapshot(promptForKnowledge); },
                trace.get(),
                &m_runControl));
              if (trace != nullptr)
              {
                  finishSessionTrace(*trace);
              }
              m_running.store(false);
          });
#endif
    }

#ifdef __EMSCRIPTEN__
    coro::task<void> AgentPanels::runAgentWeb(std::vector<agent::ModelSelection> selections,
                                              std::vector<agent::ProviderProfile> profilesCopy,
                                              agent::ProviderConfig legacyCfg,
                                              int legacyProviderIdx,
                                              std::string promptForKnowledge,
                                              int maxOutputTokens,
                                              int thinkingTokens,
                                              int requestTimeoutSeconds,
                                              std::string cacheBreakpointMode)
    {
        auto makeProvider = [](agent::ApiType apiType, agent::ProviderConfig const & c)
          -> std::unique_ptr<agent::IAiProvider> {
            if (apiType == agent::ApiType::Anthropic)
            {
                return std::make_unique<agent::AnthropicProvider>(c);
            }
            return std::make_unique<agent::OpenAiProvider>(c);
        };

        std::vector<agent::FallbackProvider::Candidate> candidates;
        for (auto const & sel : selections)
        {
            auto pit = std::find_if(profilesCopy.begin(),
                                    profilesCopy.end(),
                                    [&](agent::ProviderProfile const & p) {
                                        return p.id == sel.profileId;
                                    });
            if (pit == profilesCopy.end() || !pit->isModelAvailable(sel.model))
            {
                continue;
            }
            agent::ProviderConfig cfg;
            cfg.endpoint = pit->endpoint;
            cfg.apiKey = pit->apiKey;
            cfg.model = sel.model;
            cfg.stream = true;
            applyRequestTuning(cfg, pit->apiType, maxOutputTokens, thinkingTokens,
                               requestTimeoutSeconds, cacheBreakpointMode);
            candidates.push_back(agent::FallbackProvider::Candidate{
              pit->name + "/" + sel.model, makeProvider(pit->apiType, cfg)});
        }
        if (candidates.empty())
        {
            auto apiType = (legacyProviderIdx == 1) ? agent::ApiType::Anthropic
                                                    : agent::ApiType::OpenAiCompatible;
            candidates.push_back(agent::FallbackProvider::Candidate{
              legacyCfg.model, makeProvider(apiType, legacyCfg)});
        }

        struct WebConfirmAwaitable
        {
            AgentPanels * self = nullptr;

            bool await_ready() const noexcept
            {
                return self->m_confirmationResolved || self->m_cancellation.isCancelled();
            }

            void await_suspend(std::coroutine_handle<> h) noexcept
            {
                self->m_webConfirmHandle = h;
            }

            bool await_resume() noexcept
            {
                bool const ok = self->m_confirmationApproved && !self->m_cancellation.isCancelled();
                self->m_awaitingConfirmation = false;
                self->m_confirmationResolved = false;
                return ok;
            }
        };

        agent::FallbackProvider provider(std::move(candidates));
        agent::AgentLoop loop;
        (void) co_await loop.run(
          m_conversation,
          provider,
          m_registry,
          m_executor,
          m_contextBuilder,
          m_policy,
          m_eventQueue,
          m_limits,
          m_cancellation,
          [this]() { return m_graph.phase() == design::DesignPhase::Complete; },
          [this](tools::ToolDefinition const &, nlohmann::json const &) -> coro::task<bool> {
              co_return co_await WebConfirmAwaitable{this};
          },
          [this, promptForKnowledge]() { return buildContextSnapshot(promptForKnowledge); },
          nullptr,
          &m_runControl);
        m_running.store(false);
        co_return;
    }
#endif

    void AgentPanels::stopRun()
    {
        m_cancellation.requestCancellation();
        // A paused loop parks on the run thread; release it before joining or cancelling.
        m_runControl.resume();
        {
            std::lock_guard<std::mutex> lock(m_confirmMutex);
            m_confirmationResolved = true;
            m_confirmationApproved = false;
        }
        m_confirmCv.notify_all();
        // A question on screen would otherwise park the run thread forever.
        m_userDecisions.abandon("Run stopped");
#ifdef __EMSCRIPTEN__
        m_webConfirmHandle = {};
        m_webRunTask.reset();
#else
        if (m_workerThread.joinable())
        {
            m_workerThread.join();
        }
#endif
        m_running.store(false);
    }

    void AgentPanels::renderProviderSettingsPanel()
    {
        if (!ImGui::Begin("Agent Provider Settings", &m_showSettings))
        {
            ImGui::End();
            return;
        }

        if (!m_providerProfiles.empty())
        {
            m_editingProfileIndex =
              std::clamp(m_editingProfileIndex, 0, static_cast<int>(m_providerProfiles.size()) - 1);
            std::string const curName =
              m_providerProfiles[static_cast<std::size_t>(m_editingProfileIndex)].name;
            if (ImGui::BeginCombo("Profile", curName.c_str()))
            {
                for (int i = 0; i < static_cast<int>(m_providerProfiles.size()); ++i)
                {
                    bool const sel = (i == m_editingProfileIndex);
                    if (ImGui::Selectable(m_providerProfiles[static_cast<std::size_t>(i)].name.c_str(),
                                          sel))
                    {
                        m_editingProfileIndex = i;
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("+ Profile"))
            {
                std::string const newId =
                  "custom_" + std::to_string(m_providerProfiles.size() + 1);
                m_providerProfiles.push_back(agent::ProviderProfile{
                  newId,
                  "Local / Custom " + std::to_string(m_providerProfiles.size() + 1),
                  agent::ApiType::OpenAiCompatible,
                  "http://localhost:11434/v1",
                  {},
                  ""});
                m_editingProfileIndex = static_cast<int>(m_providerProfiles.size()) - 1;
            }
            if (m_providerProfiles.size() > 1)
            {
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove"))
                {
                    m_providerProfiles.erase(m_providerProfiles.begin() + m_editingProfileIndex);
                    m_editingProfileIndex = std::max(0, m_editingProfileIndex - 1);
                }
            }

            auto & prof = m_providerProfiles[static_cast<std::size_t>(m_editingProfileIndex)];
            char nameBuf[128];
            std::snprintf(nameBuf, sizeof(nameBuf), "%s", prof.name.c_str());
            if (ImGui::InputText("Provider Name", nameBuf, sizeof(nameBuf)))
            {
                prof.name = nameBuf;
            }

            int apiTypeInt = static_cast<int>(prof.apiType);
            char const * apiTypes[] = {"OpenAI-compatible", "Anthropic Messages"};
            if (ImGui::Combo("API Type", &apiTypeInt, apiTypes, 2))
            {
                prof.apiType = static_cast<agent::ApiType>(apiTypeInt);
            }

            char epBuf[512];
            std::snprintf(epBuf, sizeof(epBuf), "%s", prof.endpoint.c_str());
            if (ImGui::InputText("Endpoint", epBuf, sizeof(epBuf)))
            {
                prof.endpoint = epBuf;
            }

            char keyBuf[512];
            std::snprintf(keyBuf, sizeof(keyBuf), "%s", prof.apiKey.c_str());
            if (ImGui::InputText(
                  "API Key", keyBuf, sizeof(keyBuf), ImGuiInputTextFlags_Password))
            {
                prof.apiKey = keyBuf;
            }
            ImGui::TextDisabled("Credential storage: %s (never written to settings.json)",
                                m_credentialStore.backendName().c_str());
            if (!m_credentialWarning.empty())
            {
                ImGui::TextColored(ImVec4(1.f, 0.4f, 0.4f, 1.f), "%s", m_credentialWarning.c_str());
            }

            ImGui::BeginDisabled(m_modelFetchRunning.load() || prof.endpoint.empty());
            if (ImGui::Button("Fetch Free Models"))
            {
                startModelFetch(prof.id, true, false);
            }
            ImGui::SameLine();
            if (ImGui::Button("Fetch All Models"))
            {
                startModelFetch(prof.id, false, false);
            }
            ImGui::EndDisabled();
            if (!m_modelFetchStatus.empty())
            {
                ImGui::TextDisabled("%s", m_modelFetchStatus.c_str());
            }

            if (m_queriedModelsProfileId == prof.id && !m_queriedModels.empty())
            {
                ImGui::Text("Queried Models (%zu) - check to add to profile:",
                            m_queriedModels.size());
                ImGui::InputTextWithHint(
                  "##ModelFilter", "Filter queried models...", m_modelFilterBuf, sizeof(m_modelFilterBuf));
                ImGui::BeginChild("QueriedModelsList", ImVec2(0, 140.f), true);
                std::string filterLower(m_modelFilterBuf);
                std::transform(filterLower.begin(),
                               filterLower.end(),
                               filterLower.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                for (auto const & qm : m_queriedModels)
                {
                    if (!filterLower.empty())
                    {
                        std::string idLow = qm.id;
                        std::transform(idLow.begin(),
                                       idLow.end(),
                                       idLow.begin(),
                                       [](unsigned char c) {
                                           return static_cast<char>(std::tolower(c));
                                       });
                        if (idLow.find(filterLower) == std::string::npos)
                        {
                            continue;
                        }
                    }
                    auto it = std::find(prof.models.begin(), prof.models.end(), qm.id);
                    bool inProfile = (it != prof.models.end());
                    std::string label = qm.id + " [" + qm.formatPriceSummary();
                    if (qm.contextLength > 0)
                    {
                        label += ", " + std::to_string(qm.contextLength / 1000) + "k";
                    }
                    if (qm.supportsVision)
                    {
                        label += ", Vision";
                    }
                    label += "]";
                    if (ImGui::Checkbox(label.c_str(), &inProfile))
                    {
                        if (inProfile && it == prof.models.end())
                        {
                            prof.models.push_back(qm.id);
                        }
                        else if (!inProfile && it != prof.models.end())
                        {
                            prof.models.erase(it);
                        }
                        saveSettings();
                    }
                }
                ImGui::EndChild();
            }

            std::string modelsJoined;
            for (std::size_t i = 0; i < prof.models.size(); ++i)
            {
                if (i > 0)
                {
                    modelsJoined += ", ";
                }
                modelsJoined += prof.models[i];
            }
            char modelsBuf[4096];
            std::snprintf(modelsBuf, sizeof(modelsBuf), "%s", modelsJoined.c_str());
            if (ImGui::InputText("Models (comma-separated)", modelsBuf, sizeof(modelsBuf)))
            {
                prof.models.clear();
                std::string s(modelsBuf);
                std::size_t pos = 0;
                while (pos < s.size())
                {
                    auto const comma = s.find(',', pos);
                    std::string tok =
                      (comma == std::string::npos) ? s.substr(pos) : s.substr(pos, comma - pos);
                    auto const first = tok.find_first_not_of(" \t\r\n");
                    auto const last = tok.find_last_not_of(" \t\r\n");
                    if (first != std::string::npos)
                    {
                        prof.models.push_back(tok.substr(first, last - first + 1));
                    }
                    if (comma == std::string::npos)
                    {
                        break;
                    }
                    pos = comma + 1;
                }
            }
        }

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
        ImGui::Text("Execution Limits");
        bool unlimitedTurns = m_limits.maxModelTurns <= 0;
        if (ImGui::Checkbox("Unlimited model turns", &unlimitedTurns))
        {
            m_limits.maxModelTurns = unlimitedTurns ? 0 : kDefaultMaxModelTurns;
            if (m_configManager != nullptr)
            {
                m_configManager->setValue<int>("Agent", "maxModelTurns", m_limits.maxModelTurns);
                m_configManager->save();
            }
        }
        if (!unlimitedTurns)
        {
            int maxTurns = m_limits.maxModelTurns;
            if (ImGui::SliderInt("Max model turns", &maxTurns, 1, 1024))
            {
                m_limits.maxModelTurns = maxTurns;
                if (m_configManager != nullptr)
                {
                    m_configManager->setValue<int>("Agent", "maxModelTurns", maxTurns);
                    m_configManager->save();
                }
            }
        }
        bool unlimitedWallTime = m_limits.maxWallTime.count() <= 0;
        if (ImGui::Checkbox("Unlimited wall time", &unlimitedWallTime))
        {
            m_limits.maxWallTime =
              unlimitedWallTime ? std::chrono::milliseconds(0)
                                : wallTimeFromMinutes(kDefaultWallTimeMinutes);
            if (m_configManager != nullptr)
            {
                m_configManager->setValue<int>(
                  "Agent", "maxWallTimeMinutes", minutesFromWallTime(m_limits.maxWallTime));
                m_configManager->save();
            }
        }
        if (!unlimitedWallTime)
        {
            int wallTimeMinutes = minutesFromWallTime(m_limits.maxWallTime);
            if (ImGui::SliderInt("Wall time limit", &wallTimeMinutes, 1, 720, "%d min"))
            {
                m_limits.maxWallTime = wallTimeFromMinutes(wallTimeMinutes);
                if (m_configManager != nullptr)
                {
                    m_configManager->setValue<int>("Agent", "maxWallTimeMinutes", wallTimeMinutes);
                    m_configManager->save();
                }
            }
        }

        ImGui::Separator();
        ImGui::Text("Per-request Tunables");
        int maxOutputTokens = m_maxOutputTokens;
        if (ImGui::SliderInt("Max output tokens", &maxOutputTokens, 1024, 32768))
        {
            m_maxOutputTokens = maxOutputTokens;
            if (m_configManager != nullptr)
            {
                m_configManager->setValue<int>("Agent", "maxOutputTokens", m_maxOutputTokens);
                m_configManager->save();
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Completion budget per model turn. A whole program snippet has to fit "
                              "in here; too small makes the model stop mid-tool-call.");
        }
        int thinkingTokens = m_thinkingTokens;
        if (ImGui::SliderInt("Thinking tokens", &thinkingTokens, 0, 8192))
        {
            m_thinkingTokens = thinkingTokens;
            if (m_configManager != nullptr)
            {
                m_configManager->setValue<int>("Agent", "thinkingTokens", m_thinkingTokens);
                m_configManager->save();
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Cap on the model's hidden reasoning. 0 leaves it unbounded, which on "
                              "reasoning models dominates the wall time of every turn.");
        }
        int timeoutSeconds = m_requestTimeoutSeconds;
        if (ImGui::SliderInt("Request timeout", &timeoutSeconds, 30, 1800, "%d s"))
        {
            m_requestTimeoutSeconds = timeoutSeconds;
            if (m_configManager != nullptr)
            {
                m_configManager->setValue<int>(
                  "Agent", "requestTimeoutSeconds", m_requestTimeoutSeconds);
                m_configManager->save();
            }
        }
        int cacheModeIndex = 0;
        char const * cacheModes[] = {"Auto (Anthropic only)", "None", "Tools", "All"};
        char const * cacheModeKeys[] = {"auto", "none", "tools", "all"};
        for (int i = 0; i < 4; ++i)
        {
            if (m_cacheBreakpointMode == cacheModeKeys[i])
            {
                cacheModeIndex = i;
            }
        }
        if (ImGui::Combo("Prompt cache", &cacheModeIndex, cacheModes, 4))
        {
            m_cacheBreakpointMode = cacheModeKeys[cacheModeIndex];
            if (m_configManager != nullptr)
            {
                m_configManager->setValue<std::string>(
                  "Agent", "cacheBreakpoints", m_cacheBreakpointMode);
                m_configManager->save();
            }
        }

        ImGui::Separator();
        ImGui::Text("Image Generation Provider");
        int imgApiType = static_cast<int>(m_imageProviderConfig.apiType);
        char const * imgApiTypes[] = {
            "OpenAI-compatible (/images/generations)",
            "SD WebUI / Forge (/sdapi/v1/txt2img)",
            "Google Gemini / Imagen"};
        if (ImGui::Combo("Image Backend", &imgApiType, imgApiTypes, 3))
        {
            m_imageProviderConfig.apiType = static_cast<agent::ImageApiType>(imgApiType);
        }

        char imgEpBuf[512];
        std::snprintf(imgEpBuf, sizeof(imgEpBuf), "%s", m_imageProviderConfig.endpoint.c_str());
        if (ImGui::InputText("Image Endpoint", imgEpBuf, sizeof(imgEpBuf)))
        {
            m_imageProviderConfig.endpoint = imgEpBuf;
        }

        char imgKeyBuf[512];
        std::snprintf(imgKeyBuf, sizeof(imgKeyBuf), "%s", m_imageProviderConfig.apiKey.c_str());
        if (ImGui::InputText("Image API Key", imgKeyBuf, sizeof(imgKeyBuf), ImGuiInputTextFlags_Password))
        {
            m_imageProviderConfig.apiKey = imgKeyBuf;
        }

        char imgMdlBuf[256];
        std::snprintf(imgMdlBuf, sizeof(imgMdlBuf), "%s", m_imageProviderConfig.model.c_str());
        if (ImGui::InputText("Image Model", imgMdlBuf, sizeof(imgMdlBuf)))
        {
            m_imageProviderConfig.model = imgMdlBuf;
        }

        char imgSizeBuf[64];
        std::snprintf(imgSizeBuf, sizeof(imgSizeBuf), "%s", m_imageProviderConfig.defaultSize.c_str());
        if (ImGui::InputText("Default Image Size", imgSizeBuf, sizeof(imgSizeBuf)))
        {
            m_imageProviderConfig.defaultSize = imgSizeBuf;
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
