#include "AgentBenchReport.h"

#include "FileSystemUtils.h"
#include "agent/providers/AnthropicProvider.h"
#include "agent/providers/OpenAiProvider.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <utility>

namespace gladius::agent::bench
{
    namespace
    {
        std::string envOrReport(char const * name, std::string const & fallback)
        {
            char const * const value = std::getenv(name);
            return (value != nullptr && *value != '\0') ? std::string{value} : fallback;
        }

        /// @brief Judge runs are a single toolless turn; streaming is not needed
        [[nodiscard]] std::unique_ptr<IAiProvider>
        makeJudgeProvider(BenchRunConfig const & config)
        {
            ProviderConfig cfg;
            cfg.endpoint = config.endpoint;
            cfg.apiKey = config.apiKey;
            cfg.model = config.model;
            cfg.stream = false;
            cfg.maxTokens = 2000;
            // A reasoning model left uncapped can spend the whole completion budget on hidden
            // thinking and return no verdict at all, so the judge gets an explicit share.
            cfg.thinkingBudgetTokens = 512;
            cfg.temperature = 0.0;
            cfg.timeoutSeconds = config.providerTimeoutSeconds;
            if (config.api == "openai")
            {
                return std::make_unique<OpenAiProvider>(cfg);
            }
            return std::make_unique<AnthropicProvider>(cfg);
        }

        void writeFileBytes(std::filesystem::path const & path, std::vector<std::byte> const & bytes)
        {
            std::ofstream out(path, std::ios::binary);
            if (!out || bytes.empty())
            {
                return;
            }
            out.write(reinterpret_cast<char const *>(bytes.data()),
                      static_cast<std::streamsize>(bytes.size()));
        }

        [[nodiscard]] std::string isoNow()
        {
            auto const t = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
            std::tm tm{};
#if defined(_WIN32)
            gmtime_s(&tm, &t);
#else
            gmtime_r(&t, &tm);
#endif
            std::ostringstream out;
            out << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
            return out.str();
        }
    } // namespace

    nlohmann::json BenchRunRecord::toJson() const
    {
        return {{"taskId", task.id},
                {"title", task.title},
                {"prompt", task.prompt},
                {"metrics", metrics.toJson()},
                {"usageWithJudge",
                 [&] {
                     auto total = metrics.usage;
                     total.accumulate(judge.usage);
                     return nlohmann::json{{"inputTokens", total.inputTokens},
                                           {"outputTokens", total.outputTokens},
                                           {"cacheReadTokens", total.cacheReadTokens},
                                           {"cacheWriteTokens", total.cacheWriteTokens},
                                           {"totalPromptTokens", total.totalPromptTokens()}};
                 }()},
                {"geometry", geometry.toJson()},
                {"geometryChecks", checksToJson(geometryChecks)},
                {"judge", judge.toJson()},
                {"verdict",
                 {{"agentCompleted", metrics.agentCompleted},
                  {"geometryPassed", geometryPassed()},
                  {"visualPassed", judge.available && judge.passes},
                  {"judgeAvailable", judge.available},
                  {"taskPassed", taskPassed()}}},
                {"artifacts", artifacts},
                {"trace",
                 [&] {
                     nlohmann::json out = nlohmann::json::object();
                     if (!tracePath.empty())
                     {
                         out["path"] = tracePath;
                     }
                     if (profile)
                     {
                         out["profile"] = profile->toJson();
                     }
                     return out;
                 }()}};
    }

    AgentBenchReport::AgentBenchReport(BenchRunConfig const & config,
                                       std::filesystem::path outputDir)
        : m_config(config)
        , m_outputDir(std::move(outputDir))
    {
        std::error_code ec;
        std::filesystem::create_directories(m_outputDir, ec);
    }

    BenchRunRecord AgentBenchReport::runTask(HeadlessAgentHost & host,
                                             AgentBenchTask const & task,
                                             std::unique_ptr<IAiProvider> agentProvider,
                                             std::unique_ptr<IAiProvider> judgeProvider)
    {
        auto const userLibrary = getUserLibraryDir();
        auto const libraryBefore = listLibraryFiles(userLibrary);

        auto const counter = std::make_shared<ProviderFailureCounter>();
        auto observing = std::make_unique<ObservingProvider>(std::move(agentProvider),
                                                            m_config.model,
                                                            counter);

        auto const taskDir = m_outputDir / task.id;
        std::error_code ec;
        std::filesystem::create_directories(taskDir, ec);
        host.setTraceDirectory(taskDir);

        AgentBenchMetricsCollector collector{task.id};
        AgentRunResult const result = host.runTask(task, std::move(observing));
        collector.collectFrom(host, result, counter->failures);
        collector.diffUserLibrary(userLibrary, libraryBefore);

        BenchRunRecord record;
        record.task = task;
        record.metrics = collector.metrics();
        record.metrics.providerName = result.providerName;

        auto * trace = host.trace();
        std::string stlError;
        auto const stlPath = taskDir / "model.stl";
        auto stlMark = host.traceMark();
        bool const stlExported = host.exportStl(stlPath, stlError);
        host.tracePhase("export_stl", stlMark, {{"success", stlExported}});
        if (stlExported)
        {
            auto const measureMark = host.traceMark();
            record.geometry = measureStl(stlPath.string());
            host.tracePhase("measure_mesh",
                            measureMark,
                            {{"triangles", record.geometry.triangleCount}});
            record.artifacts.push_back(stlPath.string());
        }
        else if (record.geometry.error.empty())
        {
            record.geometry.error = stlError.empty() ? "STL export failed" : stlError;
        }
        auto const doc3mf = taskDir / "model.3mf";
        std::string saveError;
        auto const saveMark = host.traceMark();
        bool const saved = host.saveDocument(doc3mf, saveError);
        host.tracePhase("save_document", saveMark, {{"success", saved}});
        if (saved)
        {
            record.artifacts.push_back(doc3mf.string());
        }

        std::vector<std::vector<std::byte>> views;
        for (auto const & view : task.judgeViews)
        {
            auto const renderMark = host.traceMark();
            auto bytes = host.renderView(view, m_config.renderWidth);
            host.tracePhase("render:" + view,
                            renderMark,
                            {{"view", view},
                             {"bytes", bytes.size()},
                             {"width", m_config.renderWidth}});
            if (bytes.empty())
            {
                continue;
            }
            auto const viewPath = taskDir / ("view-" + view + ".png");
            writeFileBytes(viewPath, bytes);
            record.artifacts.push_back(viewPath.string());
            views.push_back(std::move(bytes));
        }

        record.geometryChecks = evaluateGeometry(task, record.geometry);

        if (!judgeProvider)
        {
            judgeProvider = makeJudgeProvider(m_config);
        }
        AgentBenchJudge judge(std::move(judgeProvider), m_config.renderWidth);
        judge.setTraceRecorder(trace);
        record.judge = judge.judge(task, views);

        if (trace)
        {
            record.tracePath = trace->outputFile().string();
            auto const timeline = host.writeTaskTimeline();
            if (!timeline.empty())
            {
                record.artifacts.push_back(timeline.string());
            }
            // Wall time comes from the trace extent, not from `metrics.wallMs`: the trace starts at
            // document seeding and ends after the judge, so the time split can add up to the whole task.
            record.profile = trace::analyzeTrace(trace->records());
            record.artifacts.push_back(record.tracePath);
        }

        m_records.push_back(record);
        return record;
    }

    std::filesystem::path AgentBenchReport::taskTracePath(std::string const & taskId) const
    {
        return m_outputDir / taskId / "trace.jsonl";
    }

    void AgentBenchReport::add(BenchRunRecord record)
    {
        m_records.push_back(std::move(record));
    }

    std::size_t AgentBenchReport::passedCount() const
    {
        return countIf([](BenchRunRecord const & r) { return r.taskPassed(); });
    }

    std::size_t AgentBenchReport::countIf(
      std::function<bool(BenchRunRecord const &)> const & predicate) const
    {
        return static_cast<std::size_t>(
          std::count_if(m_records.begin(), m_records.end(), predicate));
    }

    nlohmann::json AgentBenchReport::runMetadata() const
    {
        return {{"schemaVersion", 1},
                {"generatedAt", isoNow()},
                {"endpoint", m_config.endpoint},
                {"model", m_config.model},
                {"api", m_config.api},
                {"streaming", m_config.stream},
                {"limits",
                 {{"maxModelTurns", m_config.maxModelTurns},
                  {"maxToolCalls", m_config.maxToolCalls},
                  {"wallTimeMs", m_config.maxWallTime.count()}}},
                {"renderSize", m_config.renderWidth},
                {"userLibraryDir", getUserLibraryDir().string()},
                {"elapsedMs",
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - m_startTime)
                   .count()}};
    }

    nlohmann::json AgentBenchReport::toJson() const
    {
        nlohmann::json runs = nlohmann::json::array();
        for (auto const & record : m_records)
        {
            runs.push_back(record.toJson());
        }
        return {{"schemaVersion", 1},
                {"metadata", runMetadata()},
                {"summary",
                 {{"runs", m_records.size()},
                  {"passed", passedCount()},
                  {"agentCompleted",
                   countIf([](BenchRunRecord const & r) { return r.metrics.agentCompleted; })},
                  {"geometryPassed",
                   countIf([](BenchRunRecord const & r) { return r.geometryPassed(); })},
                  {"visualPassed",
                   countIf([](BenchRunRecord const & r) {
                       return r.judge.available && r.judge.passes;
                   })}}},
                {"runs", std::move(runs)}};
    }

    std::filesystem::path AgentBenchReport::write(std::string const & fileName) const
    {
        std::error_code ec;
        std::filesystem::create_directories(m_outputDir, ec);
        auto const path = m_outputDir / fileName;
        std::ofstream out(path);
        if (out)
        {
            out << toJson().dump(2);
        }
        return path;
    }

    std::filesystem::path benchOutputDir()
    {
        return envOrReport("GLADIUS_AGENT_BENCH_OUTPUT", "agent_bench");
    }

    BenchSession & BenchSession::instance()
    {
        static BenchSession session;
        return session;
    }

    BenchSession::BenchSession()
        : m_config(BenchRunConfig::fromEnvironment())
        , m_outputDir(benchOutputDir())
        , m_report(std::make_unique<AgentBenchReport>(m_config, m_outputDir))
    {
    }

    BenchAvailability BenchSession::ensureHost()
    {
        if (m_host)
        {
            return m_availability;
        }
        m_librarySandbox.emplace(m_outputDir / "user-library");
        m_host = std::make_unique<HeadlessAgentHost>(m_config);
        m_availability = m_host->initialize();
        if (!m_availability.available)
        {
            m_host.reset();
            m_librarySandbox.reset();
        }
        return m_availability;
    }

    BenchRunRecord BenchSession::runTask(AgentBenchTask const & task)
    {
        return m_report->runTask(*m_host, task, m_host->makeDefaultProvider(), nullptr);
    }
} // namespace gladius::agent::bench
