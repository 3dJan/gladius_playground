#include "HeadlessAgentHost.h"

#include "Document.h"
#include "EventLogger.h"
#include "FileSystemUtils.h"
#include "agent/providers/AnthropicProvider.h"
#include "agent/providers/OpenAiProvider.h"
#include "compute/BoundingBoxContracts.h"
#include "compute/RenderContracts.h"
#include "design/Renderability.h"
#include "io/SurfaceExtractionOptions.h"
#include "ui/OrbitalCamera.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <utility>

namespace gladius::agent::bench
{
    namespace
    {
        [[nodiscard]] std::string envOr(char const * name, std::string const & fallback)
        {
            char const * const value = std::getenv(name);
            return (value != nullptr && *value != '\0') ? std::string{value} : fallback;
        }

        [[nodiscard]] int envInt(char const * name, int fallback)
        {
            std::string const raw = envOr(name, "");
            if (raw.empty())
            {
                return fallback;
            }
            try
            {
                return std::stoi(raw);
            }
            catch (std::exception const &)
            {
                return fallback;
            }
        }

        /// @brief Orbital pitch/yaw angles of the named camera preset
        [[nodiscard]] std::pair<float, float> viewPitchYaw(std::string const & view)
        {
            constexpr float kQuarterPi = 0.7853981633974483f;
            if (view == "front")
            {
                return {0.06f, -1.5707963f};
            }
            if (view == "top")
            {
                // Slightly off the pole: the orbital camera derives its up vector from world Z.
                return {1.35f, -1.5707963f};
            }
            if (view == "bottom")
            {
                return {-1.35f, -1.5707963f};
            }
            if (view == "side")
            {
                return {0.06f, -3.1415927f};
            }
            return {kQuarterPi * 0.8f, -2.0f}; // isometric
        }

        /// @brief Fallback domain used when the WebGPU bounds probe is inconclusive
        constexpr float kFallbackHalfExtent = 40.0f;

        /// @brief Extra camera distance so judge views do not touch the image borders
        constexpr float kJudgeViewMargin = 0.6f;
    } // namespace

    UserLibrarySandbox::UserLibrarySandbox(std::filesystem::path directory)
        : m_directory(std::move(directory))
    {
        std::error_code ec;
        std::filesystem::create_directories(m_directory, ec);
        char const * const previous = std::getenv("GLADIUS_USER_LIBRARY_DIR");
        m_hadPrevious = previous != nullptr;
        if (m_hadPrevious)
        {
            m_previous = previous;
        }
        setenv("GLADIUS_USER_LIBRARY_DIR", m_directory.string().c_str(), 1);
    }

    UserLibrarySandbox::~UserLibrarySandbox()
    {
        if (m_hadPrevious)
        {
            setenv("GLADIUS_USER_LIBRARY_DIR", m_previous.c_str(), 1);
        }
        else
        {
            unsetenv("GLADIUS_USER_LIBRARY_DIR");
        }
    }

    BenchRunConfig BenchRunConfig::fromEnvironment()
    {
        BenchRunConfig config;
        config.endpoint = envOr("GLADIUS_AGENT_BENCH_ENDPOINT", config.endpoint);
        config.model = envOr("GLADIUS_AGENT_BENCH_MODEL", config.model);
        config.apiKey = envOr("GLADIUS_AGENT_BENCH_API_KEY", config.apiKey);
        config.api = envOr("GLADIUS_AGENT_BENCH_API", config.api);
        config.stream = envOr("GLADIUS_AGENT_BENCH_STREAM", "1") != "0";
        config.maxModelTurns = envInt("GLADIUS_AGENT_BENCH_MAX_TURNS", config.maxModelTurns);
        config.maxToolCalls = static_cast<std::size_t>(
          std::max(0, envInt("GLADIUS_AGENT_BENCH_MAX_TOOL_CALLS",
                             static_cast<int>(config.maxToolCalls))));
        int const defaultWallMinutes = static_cast<int>(
          std::chrono::duration_cast<std::chrono::minutes>(config.maxWallTime).count());
        config.maxWallTime = std::chrono::minutes(
          std::max(1, envInt("GLADIUS_AGENT_BENCH_WALL_MINUTES", defaultWallMinutes)));
        config.renderWidth = static_cast<std::uint32_t>(
          std::max(64, envInt("GLADIUS_AGENT_BENCH_RENDER_SIZE",
                              static_cast<int>(config.renderWidth))));
        config.renderHeight = config.renderWidth;
        config.providerTimeoutSeconds =
          std::max(30, envInt("GLADIUS_AGENT_BENCH_TIMEOUT_SECONDS", config.providerTimeoutSeconds));
        config.providerMaxTokens =
          std::max(256, envInt("GLADIUS_AGENT_BENCH_MAX_TOKENS", config.providerMaxTokens));
        config.providerThinkingTokens =
          std::max(0, envInt("GLADIUS_AGENT_BENCH_THINKING_TOKENS", config.providerThinkingTokens));
        config.cacheBreakpoints = envOr("GLADIUS_AGENT_BENCH_CACHE_BREAKPOINTS", config.cacheBreakpoints);
        // The benchmark measures geometry-design capability, so by default it runs the lean tool
        // surface: fewer definitions per turn, finish_design fills in the design-graph bookkeeping
        // itself, and the viewport review budget is capped. Each switch can be turned back off with
        // its own variable to A/B a change; the product host always uses the full surface.
        config.toolSurface = envOr("GLADIUS_AGENT_BENCH_TOOL_SURFACE", std::string{"geometry"});
        config.documentSeed = envOr("GLADIUS_AGENT_BENCH_DOCUMENT_SEED", std::string{"template"});
        config.autoCompleteDesignGraph =
          envOr("GLADIUS_AGENT_BENCH_AUTO_FINISH_GRAPH", "1") != "0";
        config.maxViewportReviews = std::max(0, envInt("GLADIUS_AGENT_BENCH_MAX_VIEWPORT_REVIEWS", 2));
        return config;
    }

    HeadlessAgentHost::HeadlessAgentHost(BenchRunConfig config)
        : m_config(std::move(config))
        , m_executor(m_registry,
                      [this](tools::ToolDefinition const & def, nlohmann::json const & args) {
                          return m_policy.evaluateTool(def, args);
                      })
    {
    }

    HeadlessAgentHost::~HeadlessAgentHost() = default;

    BenchAvailability HeadlessAgentHost::initialize()
    {
        m_runtime = compute::ApplicationComputeRuntime::createWebGPU();
        if (!m_runtime || !m_runtime->isAvailable())
        {
            m_initializeError = "WebGPU runtime unavailable: "
                                + (m_runtime ? m_runtime->getErrorMessage() : std::string{"null"});
            return {false, m_initializeError};
        }

        m_logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
        m_document = std::make_unique<Document>(m_logger);
        m_document->setBackendRuntime(m_runtime.get());
        seedDocument();
        if (!m_initializeError.empty())
        {
            return {false, m_initializeError};
        }

        // Hermetic user library so export counting never touches the developer's library.
        auto const userLibrary = getUserLibraryDir();
        (void)syncShippedLibrary();

        m_knowledge.setStoragePath((userLibrary.parent_path() / "agent-knowledge.json").string());

        m_geometryOnlySurface = m_config.toolSurface == "geometry";
        m_contextBuilder.setGeometryToolSurface(m_geometryOnlySurface);

        m_toolCtx = std::make_unique<design::DesignToolContext>(design::DesignToolContext{
          m_graph,
          m_catalogue,
          m_transactions,
          m_checkpoints,
          m_document.get(),
          [this](int w, int h, std::string const & preset) { return captureViewport(w, h, preset); },
          nullptr,
          []() {},
          &m_skills,
          &m_todos,
          &m_knowledge,
          m_config.autoCompleteDesignGraph,
          static_cast<std::size_t>(std::max(0, m_config.maxViewportReviews))});
        design::registerDesignTools(m_registry,
                                    *m_toolCtx,
                                    m_geometryOnlySurface ? design::DesignToolSurface::GeometryOnly
                                                          : design::DesignToolSurface::Full);
        m_policy.setAutonomy(agent::AutonomyLevel::WorkAutonomously);
        return {true, {}};
    }

    std::vector<std::byte> HeadlessAgentHost::captureViewport(int width,
                                                              int height,
                                                              std::string const & preset) const
    {
        auto const size = static_cast<std::uint32_t>(
          std::max(std::min(std::max(width, height), 1024), 64));
        return renderView(preset.empty() ? "isometric" : preset, size);
    }

    std::filesystem::path HeadlessAgentHost::templatePath() const
    {
        return getAppDir() / "examples" / "template.3mf";
    }

    void HeadlessAgentHost::seedDocument()
    {
        // The template is what a user gets from File > New: root function (ID 3), primitive helpers,
        // domain object, level set, volumetric data and build item. Seeding it keeps the benchmark
        // measuring design capability instead of scaffold construction.
        if (m_config.documentSeed == "blank")
        {
            m_document->newModel();
            return;
        }

        auto const path = templatePath();
        if (!std::filesystem::exists(path))
        {
            // Silently falling back to a blank document made every downstream metric meaningless.
            m_initializeError = "Benchmark template document not found at " + path.string();
            return;
        }
        m_document->load(path);
    }

    namespace
    {
        CacheBreakpointPlacement parseCacheBreakpoints(std::string const & raw)
        {
            if (raw == "none")
            {
                return CacheBreakpointPlacement::None;
            }
            if (raw == "all")
            {
                return CacheBreakpointPlacement::CacheAll;
            }
            return CacheBreakpointPlacement::Tools;
        }
    }

    std::unique_ptr<IAiProvider> HeadlessAgentHost::makeDefaultProvider() const
    {
        ProviderConfig providerCfg;
        providerCfg.endpoint = m_config.endpoint;
        providerCfg.apiKey = m_config.apiKey;
        providerCfg.model = m_config.model;
        providerCfg.stream = m_config.stream;
        providerCfg.timeoutSeconds = m_config.providerTimeoutSeconds;
        providerCfg.maxTokens = m_config.providerMaxTokens;
        providerCfg.thinkingBudgetTokens = m_config.providerThinkingTokens;
        providerCfg.cacheBreakpoints = parseCacheBreakpoints(m_config.cacheBreakpoints);

        if (m_config.api == "openai")
        {
            return std::make_unique<OpenAiProvider>(providerCfg);
        }
        return std::make_unique<AnthropicProvider>(providerCfg);
    }

    void HeadlessAgentHost::setTraceDirectory(std::filesystem::path directory)
    {
        m_traceDir = std::move(directory);
    }

    void HeadlessAgentHost::beginTaskTrace(AgentBenchTask const & task)
    {
        m_trace.reset();
        if (m_traceDir.empty())
        {
            return;
        }
        std::error_code ec;
        std::filesystem::create_directories(m_traceDir, ec);
        m_trace = std::make_unique<agent::trace::AgentTraceRecorder>(m_traceDir / "trace.jsonl");
        m_trace->setRunId(task.id);
        m_trace->recordRunMeta({{"taskId", task.id},
                                {"title", task.title},
                                {"endpoint", m_config.endpoint},
                                {"model", m_config.model},
                                {"api", m_config.api},
                                {"streaming", m_config.stream},
                                {"maxModelTurns", m_config.maxModelTurns},
                                {"maxToolCalls", m_config.maxToolCalls},
                                {"renderSize", m_config.renderWidth},
                                {"toolSurface", m_config.toolSurface},
                                {"autoCompleteDesignGraph", m_config.autoCompleteDesignGraph},
                                {"maxViewportReviews", m_config.maxViewportReviews}});
    }

    std::int64_t HeadlessAgentHost::traceMark() const
    {
        return m_trace ? m_trace->mark() : 0;
    }

    void HeadlessAgentHost::tracePhase(std::string const & name,
                                       std::int64_t startedMark,
                                       nlohmann::json fields)
    {
        if (m_trace)
        {
            m_trace->recordPhase(name, startedMark, std::move(fields));
        }
    }

    std::filesystem::path HeadlessAgentHost::writeTaskTimeline() const
    {
        if (!m_trace)
        {
            return {};
        }
        std::string error;
        auto const path = m_traceDir / "timeline.json";
        if (!m_trace->writeTimeline(path, error))
        {
            return {};
        }
        return path;
    }

    AgentRunResult HeadlessAgentHost::runTask(AgentBenchTask const & task,
                                              std::unique_ptr<IAiProvider> provider)
    {
        if (!provider)
        {
            provider = makeDefaultProvider();
        }

        // Every task starts from an untouched document and design state so nothing measured for
        // one task leaks into the next one.
        beginTaskTrace(task);
        {
            agent::trace::ScopedTracePhase seeding(m_trace.get(), "seed_document");
            seedDocument();
            m_graph.clear();
            m_todos.clear();
            m_checkpoints.clear();
            m_knowledge.clear();
            m_graph.setPhase(design::DesignPhase::ClarifyRequirements);
            m_conversation.clear();
            m_conversation.appendUserText(task.prompt);
            if (m_toolCtx)
            {
                m_toolCtx->viewportReviewsUsed = 0;
            }
        }

        AgentLimits limits;
        limits.maxModelTurns = m_config.maxModelTurns;
        limits.maxToolCalls = m_config.maxToolCalls;
        limits.maxWallTime = m_config.maxWallTime;

        auto const prompt = task.prompt;
        agent::AgentLoop loop;
        return coro::sync_wait(loop.run(
          m_conversation,
          *provider,
          m_registry,
          m_executor,
          m_contextBuilder,
          m_policy,
          m_events,
          limits,
          m_cancellation,
          [this]() { return m_graph.phase() == design::DesignPhase::Complete; },
          nullptr,
          [this, prompt]() {
              agent::DesignContextSnapshot snap;
              snap.autonomy = m_policy.autonomy();
              snap.designPhase = design::toString(m_graph.phase());
              if (m_document != nullptr)
              {
                  auto const facts = design::collectRenderabilityFacts(m_document.get());
                  if (!facts.isRenderable())
                  {
                      snap.renderabilitySummary = facts.blockersAsText();
                  }
              }
              snap.designGraphSummary = m_graph.summarize();
              snap.availableSkillsSummary = m_skills.summarizeForPrompt();
              snap.todoSummary = m_todos.summarize();
              snap.knowledgeSummary = m_knowledge.summarizeForPrompt(prompt, 5);
              for (auto const & req : m_graph.requirements())
              {
                  snap.requirementsSummary += "- " + req.id + " [" + req.category
                                              + (req.mandatory ? ", mandatory" : ", optional") + "] \""
                                              + req.description + "\"\n";
              }
              return snap;
          },
          m_trace.get()));
    }

    std::vector<AgentEvent> HeadlessAgentHost::drainEvents()
    {
        return m_events.drain();
    }

    compute::RenderCamera HeadlessAgentHost::frameCamera(std::string const & view,
                                                         std::uint32_t size,
                                                         BoundingBox const & bounds)
    {
        ui::OrbitalCamera camera;
        auto const [pitch, yaw] = viewPitchYaw(view);
        camera.setLookAt({(bounds.min.x + bounds.max.x) * 0.5f,
                          (bounds.min.y + bounds.max.y) * 0.5f,
                          (bounds.min.z + bounds.max.z) * 0.5f});
        camera.setAngle(pitch, yaw);
        camera.snapToTarget();
        // Fit the whole model into the frame; the shipped thumbnail renderer frames the same way.
        camera.adjustDistanceToTarget(bounds, static_cast<float>(size), static_cast<float>(size));
        // The fit is exact, so the object touches all four edges. Judge views read better (and the
        // silhouette stays recognisable) with some empty space around it.
        camera.zoom(kJudgeViewMargin);
        camera.snapToTarget();

        auto const eye = camera.getEyePosition();
        auto const matrix = camera.computeModelViewPerspectiveMatrix();
        return compute::RenderCamera{
          .eyePosition = {eye.x, eye.y, eye.z},
          .forwardDirection = {matrix.s8, matrix.s9, matrix.sa},
          .rightDirection = {matrix.s0, matrix.s1, matrix.s2},
          .upDirection = {matrix.s4, matrix.s5, matrix.s6}};
    }

    std::vector<std::byte> HeadlessAgentHost::renderView(std::string const & view,
                                                         std::uint32_t size) const
    {
        if (!m_document)
        {
            return {};
        }
        BoundingBox bounds{float4{-kFallbackHalfExtent, -kFallbackHalfExtent, -kFallbackHalfExtent, 0.0f},
                           float4{kFallbackHalfExtent, kFallbackHalfExtent, kFallbackHalfExtent, 0.0f}};
        auto const probed = m_document->computeModelBoundsBlocking();
        if (probed && probed->isUsable() && probed->modelBounds.has_value())
        {
            auto const & b = *probed->modelBounds;
            bounds = BoundingBox{float4{b.min[0], b.min[1], b.min[2], 0.0f},
                                 float4{b.max[0], b.max[1], b.max[2], 0.0f}};
        }

        try
        {
            auto const image = m_document->createThumbnailPng(
              size, size, frameCamera(view, size, bounds), std::nullopt);
            std::vector<std::byte> out(image.data.size());
            std::transform(image.data.begin(), image.data.end(), out.begin(), [](unsigned char b) {
                return static_cast<std::byte>(b);
            });
            return out;
        }
        catch (std::exception const &)
        {
            return {};
        }
    }

    std::vector<std::string> HeadlessAgentHost::modelErrors() const
    {
        std::vector<std::string> errors;
        if (!m_document)
        {
            return errors;
        }
        for (auto const & issue : m_document->getIssueList().getAll())
        {
            errors.push_back(issue.model + ": " + issue.message);
        }
        auto const logger = m_document->getSharedLogger();
        if (!logger)
        {
            return errors;
        }
        for (auto it = logger->cbegin(); it != logger->cend(); ++it)
        {
            if (it->getSeverity() == events::Severity::Error)
            {
                errors.push_back(it->getMessage());
            }
        }
        return errors;
    }

    bool HeadlessAgentHost::exportStl(std::filesystem::path const & path, std::string & error) const
    {
        if (!m_document)
        {
            error = "No document";
            return false;
        }
        try
        {
            m_document->exportAsStl(path, io::StlExportOptions{});
            return std::filesystem::exists(path);
        }
        catch (std::exception const & e)
        {
            error = e.what();
            // A failed export is almost always a symptom (dangling function call, unflattenable
            // graph), so report what the document itself complained about.
            auto const issues = modelErrors();
            for (std::size_t i = issues.size() > 3u ? issues.size() - 3u : 0u; i < issues.size(); ++i)
            {
                error += " | " + issues[i];
            }
            return false;
        }
    }

    bool HeadlessAgentHost::saveDocument(std::filesystem::path const & path, std::string & error) const
    {
        if (!m_document)
        {
            error = "No document";
            return false;
        }
        try
        {
            m_document->saveAs(path, false);
            return std::filesystem::exists(path);
        }
        catch (std::exception const & e)
        {
            error = e.what();
            return false;
        }
    }

    std::size_t HeadlessAgentHost::toolCount() const
    {
        return m_registry.size();
    }
} // namespace gladius::agent::bench
