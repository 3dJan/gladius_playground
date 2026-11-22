#include "AgentPanels.h"

#include "ClipboardImageHelper.h"
#include "../ConfigManager.h"
#include "../Document.h"
#include "../ExpressionParser.h"
#include "../ExpressionToGraphConverter.h"
#include "../ProgramSnippetApplier.h"
#include "../agent/providers/AnthropicProvider.h"
#include "../agent/providers/FallbackProvider.h"
#include "../agent/providers/OpenAiProvider.h"
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
#include <cstring>
#include <filesystem>
#include <fstream>

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
          [this]() { m_documentModifiedPending.store(true); },
          &m_skills,
          &m_todos,
          &m_knowledge});
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
            state["startFresh"] = m_startFresh;
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
        m_streamingBuffer.clear();
        m_startFresh = true;
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
            m_startFresh = state.value("startFresh", m_conversation.empty());
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
              {"startFresh", m_startFresh},
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
                    m_startFresh = state.value("startFresh", false);
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
                        m_startFresh = state.value("startFresh", m_conversation.empty());
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

        std::string const account = (m_providerIndex == 1) ? "anthropic" : "openai";
        if (auto key = m_credentialStore.loadSecret(kCredentialService, account))
        {
            std::snprintf(m_apiKeyBuf, sizeof(m_apiKeyBuf), "%s", key->c_str());
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

            // Always provide an OpenRouter profile (supports free models without API key)
            bool hasOpenRouter = false;
            for (auto const & p : m_providerProfiles)
            {
                if (agent::isOpenRouterEndpoint(p.endpoint))
                {
                    hasOpenRouter = true;
                    break;
                }
            }
            if (!hasOpenRouter)
            {
                agent::ProviderProfile openRouterProf{
                  "openrouter_default",
                  "OpenRouter",
                  agent::ApiType::OpenAiCompatible,
                  "https://openrouter.ai/api/v1",
                  {},
                  ""};
                if (auto k =
                      m_credentialStore.loadSecret(kCredentialService, "profile_openrouter_default"))
                {
                    openRouterProf.apiKey = *k;
                }
                m_providerProfiles.push_back(std::move(openRouterProf));
            }
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
        if (!hasAnyAvailableModel)
        {
            for (auto const & p : m_providerProfiles)
            {
                if (agent::isOpenRouterEndpoint(p.endpoint))
                {
                    startModelFetch(p.id, true, true);
                    break;
                }
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
                saveSessionState();
            }
            else if (auto const * af = std::get_if<agent::AgentFailedEvent>(&ev))
            {
                m_activeToolName.clear();
                m_lastError = af->error;
                m_streamingBuffer.clear();
                saveSessionState();
            }
        }
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

        if (m_startFresh)
        {
            m_graph.clear();
            m_conversation.clear();
            m_checkpoints.clear();
            m_todos.clear();
            m_activityLog.clear();
            if (m_document != nullptr)
            {
                m_document->newModel();
                if (m_onDocumentModified)
                {
                    m_onDocumentModified();
                }
            }
            m_startFresh = false;
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
        agent::ProviderConfig legacyCfg;
        legacyCfg.endpoint = m_endpointBuf;
        legacyCfg.model = m_modelBuf;
        legacyCfg.apiKey = m_apiKeyBuf;
        legacyCfg.stream = true;
        int const legacyProviderIdx = m_providerIndex;
        std::string const promptForKnowledge = userPrompt;

#ifdef __EMSCRIPTEN__
        m_webRunTask = runAgentWeb(std::move(selections),
                                   std::move(profilesCopy),
                                   std::move(legacyCfg),
                                   legacyProviderIdx,
                                   promptForKnowledge);
        m_webRunTask->resume();
#else
        m_workerThread = std::thread(
          [this, selections, profilesCopy, legacyCfg, legacyProviderIdx, promptForKnowledge]() {
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
                [this, promptForKnowledge]() {
                    agent::DesignContextSnapshot snap;
                    snap.autonomy = m_policy.autonomy();
                    snap.isIncremental = (m_conversation.size() > 1)
                                         || (m_document && m_document->getAssembly()
                                             && m_document->getAssembly()->getFunctions().size() > 1);
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
                    auto const reqChanges = m_graph.consumePendingRequirementChanges();
                    for (auto const & ch : reqChanges)
                    {
                        snap.requirementChangesSummary += "- " + ch + "\n";
                    }
                    snap.designPhase = design::toString(m_graph.phase());
                    snap.designGraphSummary = m_graph.summarize();
                    snap.availableSkillsSummary = m_skills.summarizeForPrompt();
                    snap.todoSummary = m_todos.summarize();
                    snap.knowledgeSummary = m_knowledge.summarizeForPrompt(promptForKnowledge, 5);
                    return snap;
                }));
              m_running.store(false);
          });
#endif
    }

#ifdef __EMSCRIPTEN__
    coro::task<void> AgentPanels::runAgentWeb(std::vector<agent::ModelSelection> selections,
                                              std::vector<agent::ProviderProfile> profilesCopy,
                                              agent::ProviderConfig legacyCfg,
                                              int legacyProviderIdx,
                                              std::string promptForKnowledge)
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
          [this, promptForKnowledge]() {
              agent::DesignContextSnapshot snap;
              snap.autonomy = m_policy.autonomy();
              snap.isIncremental = (m_conversation.size() > 1)
                                   || (m_document && m_document->getAssembly()
                                       && m_document->getAssembly()->getFunctions().size() > 1);
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
              auto const reqChanges = m_graph.consumePendingRequirementChanges();
              for (auto const & ch : reqChanges)
              {
                  snap.requirementChangesSummary += "- " + ch + "\n";
              }
              snap.designPhase = design::toString(m_graph.phase());
              snap.designGraphSummary = m_graph.summarize();
              snap.availableSkillsSummary = m_skills.summarizeForPrompt();
              snap.todoSummary = m_todos.summarize();
              snap.knowledgeSummary = m_knowledge.summarizeForPrompt(promptForKnowledge, 5);
              return snap;
          });
        m_running.store(false);
        co_return;
    }
#endif

    void AgentPanels::stopRun()
    {
        m_cancellation.requestCancellation();
        {
            std::lock_guard<std::mutex> lock(m_confirmMutex);
            m_confirmationResolved = true;
            m_confirmationApproved = false;
        }
        m_confirmCv.notify_all();
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

        float const footerHeight = m_pendingImages.empty() ? 152.f : 206.f;
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

        // Image attachment, clipboard paste, and document insertion controls
        if (ImGui::Button(reinterpret_cast<char const *>(ICON_FA_IMAGE " Attach Image")))
        {
            char const * filters[] = {"*.png", "*.jpg", "*.jpeg"};
            char const * selected =
              tinyfd_openFileDialog("Attach Reference Image", "", 3, filters, "Image files", 0);
            if (selected != nullptr)
            {
                (void) attachImageFile(selected);
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Paste Image"))
        {
            auto clip = ClipboardImageHelper::readClipboardImage();
            if (clip.success && !clip.pngBytes.empty())
            {
                std::string const key = "att_" + std::to_string(m_nextAttachmentId++);
                m_pendingImages.push_back(PendingImageAttachment{
                  std::move(clip.filename),
                  std::move(clip.pngBytes),
                  std::move(clip.mimeType),
                  "",
                  key});
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
        ImGui::SameLine();
        ImGui::Checkbox("Start fresh", &m_startFresh);
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Clear current model & history on next prompt (auto-unchecks after first prompt)");
        }

        if (!m_pendingImages.empty())
        {
            for (std::size_t i = 0; i < m_pendingImages.size(); ++i)
            {
                if (i > 0)
                {
                    ImGui::SameLine();
                }
                ImGui::PushID(static_cast<int>(i));
                auto & att = m_pendingImages[i];
                if (att.previewKey.empty())
                {
                    att.previewKey = "att_" + std::to_string(m_nextAttachmentId++);
                }
                int w = 0;
                int h = 0;
                unsigned int const texId =
                  getOrCreatePreviewTexture(att.previewKey, att.bytes, w, h);
                if (texId != 0)
                {
                    if (ImGui::ImageButton("##thumb",
                                           static_cast<ImTextureID>(static_cast<uintptr_t>(texId)),
                                           ImVec2(40.f, 40.f)))
                    {
                        m_inspectedImageIndex = static_cast<int>(i);
                        ImGui::OpenPopup("AttachedImageModal");
                    }
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::SetTooltip("%s\nClick to enlarge & add comment%s",
                                          att.filename.c_str(),
                                          att.comment.empty() ? "" : ("\nComment: " + att.comment).c_str());
                    }
                }
                else
                {
                    if (ImGui::SmallButton(att.filename.c_str()))
                    {
                        m_inspectedImageIndex = static_cast<int>(i);
                        ImGui::OpenPopup("AttachedImageModal");
                    }
                }
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                {
                    if (m_inspectedImageIndex == static_cast<int>(i))
                    {
                        m_inspectedImageIndex = -1;
                    }
                    m_pendingImages.erase(m_pendingImages.begin() + static_cast<long>(i));
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
        }

        // Modal popup for enlarged image preview & comment editing
        if (ImGui::BeginPopup("AttachedImageModal"))
        {
            if (m_inspectedImageIndex >= 0
                && static_cast<std::size_t>(m_inspectedImageIndex) < m_pendingImages.size())
            {
                auto & att = m_pendingImages[static_cast<std::size_t>(m_inspectedImageIndex)];
                ImGui::Text("File: %s (%zu bytes, %s)",
                            att.filename.c_str(),
                            att.bytes.size(),
                            att.mimeType.c_str());
                int w = 0;
                int h = 0;
                unsigned int const texId =
                  getOrCreatePreviewTexture(att.previewKey, att.bytes, w, h);
                if (texId != 0 && w > 0 && h > 0)
                {
                    float const maxDim = 280.f;
                    float const scale = std::min(maxDim / static_cast<float>(w),
                                                 maxDim / static_cast<float>(h));
                    ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(texId)),
                                 ImVec2(static_cast<float>(w) * scale,
                                        static_cast<float>(h) * scale));
                }
                char commentBuf[512];
                std::snprintf(commentBuf, sizeof(commentBuf), "%s", att.comment.c_str());
                if (ImGui::InputText("Comment / Feedback", commentBuf, sizeof(commentBuf)))
                {
                    att.comment = commentBuf;
                }
                if (ImGui::Button("Close"))
                {
                    ImGui::CloseCurrentPopup();
                }
            }
            else
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        ImGui::InputTextMultiline("##AgentPrompt", m_promptBuf, sizeof(m_promptBuf), ImVec2(-75.f, 52.f));
        m_promptFocused = ImGui::IsItemFocused();
        if (m_promptFocused)
        {
            ImGuiIO const & io = ImGui::GetIO();
            bool const pasteChord =
              (io.KeyCtrl || io.KeySuper) && ImGui::IsKeyPressed(ImGuiKey_V, false);
            if (pasteChord)
            {
                auto clip = ClipboardImageHelper::readClipboardImage();
                if (clip.success && !clip.pngBytes.empty())
                {
                    std::string const key = "att_" + std::to_string(m_nextAttachmentId++);
                    m_pendingImages.push_back(PendingImageAttachment{
                      std::move(clip.filename),
                      std::move(clip.pngBytes),
                      std::move(clip.mimeType),
                      "",
                      key});
                }
            }
        }

        ImGui::SameLine();
        ImGui::BeginDisabled(m_running.load());
        if (ImGui::Button("Send", ImVec2(65.f, 52.f)))
        {
            std::string prompt(m_promptBuf);
            m_promptBuf[0] = '\0';
            startRun(prompt);
        }
        ImGui::EndDisabled();

        // Primary & Fallback provider/model selection dropdowns below prompt
        std::vector<agent::ModelSelection> allChoices;
        std::vector<std::string> allLabels;
        for (auto const & prof : m_providerProfiles)
        {
            for (auto const & mdl : prof.models)
            {
                if (!prof.isModelAvailable(mdl))
                {
                    continue;
                }
                allChoices.push_back(agent::ModelSelection{prof.id, mdl});
                allLabels.push_back(prof.name + " / " + mdl);
            }
        }
        if (allChoices.empty())
        {
            if (ImGui::Button("No available models - Configure Providers"))
            {
                m_showSettings = true;
            }
        }
        else
        {
            std::string primaryLabel = "Select model...";
            for (std::size_t i = 0; i < allChoices.size(); ++i)
            {
                if (allChoices[i] == m_primarySelection)
                {
                    primaryLabel = allLabels[i];
                    break;
                }
            }
            auto showModelTooltip = [&](agent::ModelSelection const & sel) {
                if (!ImGui::IsItemHovered())
                {
                    return;
                }
                for (auto const & prof : m_providerProfiles)
                {
                    if (prof.id != sel.profileId)
                    {
                        continue;
                    }
                    auto mit = prof.modelMetadata.find(sel.model);
                    if (mit != prof.modelMetadata.end())
                    {
                        auto const & meta = mit->second;
                        std::string ctxStr =
                          meta.contextLength > 0
                            ? (std::to_string(meta.contextLength / 1000) + "k ctx")
                            : "Context unknown";
                        ImGui::SetTooltip("Price: %s\nContext: %s\nVision: %s",
                                          meta.formatPriceSummary().c_str(),
                                          ctxStr.c_str(),
                                          meta.supportsVision ? "Yes" : "No");
                    }
                    else if (agent::isLocalEndpoint(prof.endpoint))
                    {
                        ImGui::SetTooltip("Local model (free / no API cost)");
                    }
                    else
                    {
                        ImGui::SetTooltip("Provider: %s\nPrice info not queried",
                                          prof.name.c_str());
                    }
                    break;
                }
            };

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.48f);
            if (ImGui::BeginCombo("##PrimaryModel", primaryLabel.c_str()))
            {
                for (std::size_t i = 0; i < allChoices.size(); ++i)
                {
                    bool const selected = (allChoices[i] == m_primarySelection);
                    if (ImGui::Selectable(allLabels[i].c_str(), selected))
                    {
                        m_primarySelection = allChoices[i];
                        saveSettings();
                    }
                    showModelTooltip(allChoices[i]);
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            showModelTooltip(m_primarySelection);
            ImGui::SameLine();
            std::string const fbPreview =
              m_fallbackSelections.empty()
                ? "Fallbacks: None"
                : ("Fallbacks: " + std::to_string(m_fallbackSelections.size()));
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::BeginCombo("##FallbackModels", fbPreview.c_str()))
            {
                for (std::size_t i = 0; i < allChoices.size(); ++i)
                {
                    auto it = std::find(
                      m_fallbackSelections.begin(), m_fallbackSelections.end(), allChoices[i]);
                    bool checked = (it != m_fallbackSelections.end());
                    if (ImGui::Checkbox(allLabels[i].c_str(), &checked))
                    {
                        if (checked && it == m_fallbackSelections.end())
                        {
                            m_fallbackSelections.push_back(allChoices[i]);
                        }
                        else if (!checked && it != m_fallbackSelections.end())
                        {
                            m_fallbackSelections.erase(it);
                        }
                        saveSettings();
                    }
                    showModelTooltip(allChoices[i]);
                }
                ImGui::EndCombo();
            }
        }

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
            auto const reqsCopy = m_graph.requirements();
            for (auto const & req : reqsCopy)
            {
                ImGui::PushID(req.id.c_str());
                char descBuf[512];
                std::snprintf(descBuf, sizeof(descBuf), "%s", req.description.c_str());
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
                bool edited = ImGui::InputText("##desc", descBuf, sizeof(descBuf));
                ImGui::SameLine();
                char catBuf[64];
                std::snprintf(catBuf, sizeof(catBuf), "%s", req.category.c_str());
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.45f);
                edited |= ImGui::InputText("##cat", catBuf, sizeof(catBuf));
                ImGui::SameLine();
                bool mandatory = req.mandatory;
                edited |= ImGui::Checkbox("Req##mand", &mandatory);
                ImGui::SameLine();
                if (ImGui::SmallButton("x"))
                {
                    m_graph.removeRequirement(req.id, true);
                    ImGui::PopID();
                    break;
                }
                if (edited)
                {
                    m_graph.updateRequirement(req.id, descBuf, catBuf, mandatory, true);
                }
                ImGui::PopID();
            }

            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.55f);
            ImGui::InputTextWithHint("##newReqDesc", "New requirement...", m_newReqBuf, sizeof(m_newReqBuf));
            ImGui::SameLine();
            ImGui::SetNextItemWidth(70.f);
            ImGui::InputText("##newReqCat", m_newReqCatBuf, sizeof(m_newReqCatBuf));
            ImGui::SameLine();
            ImGui::Checkbox("Req##newMand", &m_newReqMandatory);
            ImGui::SameLine();
            if (ImGui::SmallButton("+") && m_newReqBuf[0] != '\0')
            {
                std::string const newId =
                  m_graph.addRequirement(m_newReqBuf, m_newReqCatBuf, m_newReqMandatory);
                m_graph.recordUserAddedRequirement(newId);
                m_newReqBuf[0] = '\0';
            }
            ImGui::TreePop();
        }

        if (!m_todos.items().empty() && ImGui::TreeNode("Planning Todos"))
        {
            for (auto const & todo : m_todos.items())
            {
                ImGui::PushID(todo.id.c_str());
                ImGui::Text("[%s] %s", design::toString(todo.status), todo.title.c_str());
                for (std::size_t ci = 0; ci < todo.checklist.size(); ++ci)
                {
                    bool chk = todo.checklist[ci].checked;
                    if (ImGui::Checkbox(
                          (todo.checklist[ci].label + "##chk" + std::to_string(ci)).c_str(), &chk))
                    {
                        m_todos.toggleChecklistEntry(todo.id, ci, chk);
                    }
                }
                ImGui::PopID();
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

        if (m_selectedRecombineConfigs.size() >= 2)
        {
            if (ImGui::Button("Recombine Selected"))
            {
                std::string const & baseId = m_selectedRecombineConfigs[0];
                std::string const & overId = m_selectedRecombineConfigs[1];
                auto const overIt = m_graph.configurations().find(overId);
                std::vector<std::string> overrideNodes;
                if (overIt != m_graph.configurations().end())
                {
                    for (auto const & [nid, _] : overIt->second.selectedVariants)
                    {
                        overrideNodes.push_back(nid);
                    }
                }
                std::string const hybridName = "Hybrid (" + baseId + " + " + overId + ")";
                if (auto const hid =
                      m_graph.hybridizeConfigurations(hybridName, baseId, overId, overrideNodes))
                {
                    m_graph.activateConfiguration(*hid);
                }
                m_selectedRecombineConfigs.clear();
            }
            ImGui::Separator();
        }

        auto const configsCopy = m_graph.configurations();
        for (auto const & [cid, cfg] : configsCopy)
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
                if (ImGui::IsItemHovered())
                {
                    ImGui::BeginTooltip();
                    ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(texId)),
                                 ImVec2(224.f, 224.f));
                    ImGui::Text("%s (%s) %s", cfg.name.c_str(), cid.c_str(), cfg.active ? "[Active]" : "");
                    ImGui::Text("Pruning score: %.1f | Variants: %zu",
                                cfg.evaluation.pruningScore,
                                cfg.selectedVariants.size());
                    if (!cfg.userFeedback.empty())
                    {
                        ImGui::TextWrapped("Feedback: %s", cfg.userFeedback.c_str());
                    }
                    for (auto const & p : cfg.evaluation.deterministicPassed)
                    {
                        ImGui::TextColored(ImVec4(0.4f, 0.9f, 0.4f, 1.f), "OK: %s", p.c_str());
                    }
                    for (auto const & wMsg : cfg.evaluation.deterministicWarnings)
                    {
                        ImGui::TextColored(ImVec4(1.f, 0.7f, 0.3f, 1.f), "WARN: %s", wMsg.c_str());
                    }
                    ImGui::EndTooltip();
                }
                ImGui::SameLine();
            }
            ImGui::BeginGroup();
            auto selIt = std::find(
              m_selectedRecombineConfigs.begin(), m_selectedRecombineConfigs.end(), cid);
            bool selectedForRecombine = (selIt != m_selectedRecombineConfigs.end());
            if (ImGui::Checkbox("##recomb", &selectedForRecombine))
            {
                if (selectedForRecombine && selIt == m_selectedRecombineConfigs.end())
                {
                    m_selectedRecombineConfigs.push_back(cid);
                }
                else if (!selectedForRecombine && selIt != m_selectedRecombineConfigs.end())
                {
                    m_selectedRecombineConfigs.erase(selIt);
                }
            }
            ImGui::SameLine();
            ImGui::Text("%s %s", cfg.name.c_str(), cfg.active ? "[Active]" : "");
            if (!cfg.active && ImGui::Button("Activate"))
            {
                m_graph.activateConfiguration(cid);
            }
            char fbBuf[256];
            std::snprintf(fbBuf, sizeof(fbBuf), "%s", cfg.userFeedback.c_str());
            ImGui::SetNextItemWidth(-1.f);
            if (ImGui::InputTextWithHint("##fb", "Add comment/feedback...", fbBuf, sizeof(fbBuf)))
            {
                m_graph.setConfigurationFeedback(cid, fbBuf);
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
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::Image(static_cast<ImTextureID>(static_cast<uintptr_t>(texId)),
                                     ImVec2(224.f, 224.f));
                        ImGui::Text("%s (%s)", cp.label.c_str(), cp.id.c_str());
                        ImGui::Text("Functions: %zu | Level: %d", cp.functionCount, cp.level);
                        if (!cp.userFeedback.empty())
                        {
                            ImGui::TextWrapped("Feedback: %s", cp.userFeedback.c_str());
                        }
                        ImGui::EndTooltip();
                    }
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
                char cpFb[256];
                std::snprintf(cpFb, sizeof(cpFb), "%s", cp.userFeedback.c_str());
                ImGui::SetNextItemWidth(-1.f);
                if (ImGui::InputTextWithHint("##cpfb", "Snapshot comment...", cpFb, sizeof(cpFb)))
                {
                    m_checkpoints.setFeedback(cp.id, cpFb);
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
