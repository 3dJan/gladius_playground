#include "agentBench/AgentBenchGeometry.h"
#include "agentBench/AgentBenchJudge.h"
#include "agentBench/AgentBenchMetrics.h"
#include "agentBench/AgentBenchReport.h"
#include "agentBench/AgentBenchTask.h"
#include "agentBench/HeadlessAgentHost.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cstdio>
#include <iostream>
#include <filesystem>
#include <queue>
#include <string>
#include <utility>
#include <vector>

namespace gladius::agent::bench::tests
{
    namespace
    {
        /// @brief Replays a fixed transcript so the harness can be validated without an LLM
        class ScriptedProvider final : public IAiProvider
        {
          public:
            std::queue<CompletionResponse> turns;
            TokenUsage scriptedUsage;
            std::size_t calls = 0;

            explicit ScriptedProvider(std::string label = "scripted")
                : m_label(std::move(label))
            {
            }

            [[nodiscard]] std::string name() const override
            {
                return m_label;
            }

            coro::task<CompletionResponse> complete(CompletionRequest const & request,
                                                    TextDeltaCallback /*onDelta*/,
                                                    io::CancellationToken const &) override
            {
                ++calls;
                m_lastToolCount = request.tools.size();
                if (turns.empty())
                {
                    co_return CompletionResponse{"Done", {}, "stop", true, "", {}, scriptedUsage, m_label};
                }
                auto resp = std::move(turns.front());
                turns.pop();
                resp.usage = scriptedUsage;
                resp.providerName = m_label;
                co_return resp;
            }

            [[nodiscard]] std::size_t lastToolCount() const noexcept
            {
                return m_lastToolCount;
            }

          private:
            std::string m_label;
            std::size_t m_lastToolCount = 0;
        };

        /// @brief Returns a fixed judge answer
        class ScriptedJudge final : public IAiProvider
        {
          public:
            ScriptedJudge(std::string answer, bool success = true)
                : m_answer(std::move(answer))
                , m_success(success)
            {
            }

            [[nodiscard]] std::string name() const override
            {
                return "scripted_judge";
            }

            coro::task<CompletionResponse> complete(CompletionRequest const & request,
                                                    TextDeltaCallback,
                                                    io::CancellationToken const &) override
            {
                seenTools = request.tools.size();
                for (auto const & msg : request.messages)
                {
                    for (auto const & part : msg.parts)
                    {
                        if (std::holds_alternative<ImageContent>(part))
                        {
                            ++seenImages;
                        }
                    }
                }
                co_return CompletionResponse{
                    m_answer, {}, "stop", m_success, "", {}, TokenUsage{11, 7, 0, 0}, "scripted_judge"};
            }

            std::size_t seenTools = 0;
            std::size_t seenImages = 0;

          private:
            std::string m_answer;
            bool m_success;
        };

        std::vector<std::byte> const kFakePng{std::byte{0x89}, std::byte{0x50}, std::byte{0x4e}, std::byte{0x47}};

        /// @brief Sphere of radius 5 mm as a design-agent program snippet.
        /// The template document drives the model through its root function (ID: 3), which exposes
        /// the named outputs "color" and "shape", so a snippet has to keep that signature.
        constexpr char const * const kSphereSnippet =
          "// Function: main (ID: 3) [root]\n"
          "(vec3 color, float shape) main_3(vec3 pos) {\n"
          "  color = vec3(0.1, 0.2, 1.0);\n"
          "  shape = length(pos) - 5.0;\n"
          "}\n";

        /// @brief Root program of a 1 mm sphere, used to verify the geometry gates reject wrong sizes
        constexpr char const * const kTinySphereSnippet =
          "// Function: main (ID: 3) [root]\n"
          "(vec3 color, float shape) main_3(vec3 pos) {\n"
          "  color = vec3(0.1, 0.2, 1.0);\n"
          "  shape = length(pos) - 3.0;\n"
          "}\n";

        /// @brief Scripts the design-graph ceremony the finish_design gate requires
        void pushSphereCeremony(ScriptedProvider & provider)
        {
            provider.turns.push(CompletionResponse{
              "Modelling the sphere.",
              {ToolCall{"t1", "set_program_snippet", {{"snippet", kSphereSnippet}}},
               ToolCall{"t2", "add_function", {{"name", "Body"}, {"type", "component"}}},
               ToolCall{"t3", "create_variant", {{"nodeId", "node_1"}, {"name", "Sphere"}}},
               ToolCall{"t4", "list_library_items", {{"query", "sphere"}}},
               ToolCall{"t5", "import_library_item", {{"category", "primitives"}, {"name", "sphere"}}},
               ToolCall{"t6", "capture_viewport", {{"width", 128}, {"height", 128}}},
               ToolCall{"t7", "totally_missing_tool", nlohmann::json::object()},
               ToolCall{"t8",
                        "create_configuration",
                        {{"name", "Concept A"}, {"selectedVariants", {{"node_1", "var_1"}}}}},
               ToolCall{"t9", "activate_configuration", {{"configurationId", "cfg_1"}}},
               ToolCall{"t10", "finish_design", nlohmann::json::object()}},
              "tool_calls",
              true,
              ""});
        }
    } // namespace

    TEST(AgentBenchGeometry, SignedVolumeOfBoxMesh_ReportsBoxVolumeAndBBox)
    {
        // Arrange: cube from 0..10 mm written as binary STL (12 triangles, closed and consistently wound)
        std::vector<std::array<float, 3>> const tris = {
          {0, 0, 0}, {10, 0, 0}, {10, 10, 0}, {0, 0, 0}, {10, 10, 0}, {0, 10, 0},
          {0, 0, 10}, {0, 10, 10}, {10, 10, 10}, {0, 0, 10}, {10, 10, 10}, {10, 0, 10},
          {0, 0, 0}, {0, 0, 10}, {10, 0, 10}, {0, 0, 0}, {10, 0, 10}, {10, 0, 0},
          {0, 10, 0}, {10, 10, 0}, {10, 10, 10}, {0, 10, 0}, {10, 10, 10}, {0, 10, 10},
          {0, 0, 0}, {0, 10, 0}, {0, 10, 10}, {0, 0, 0}, {0, 10, 10}, {0, 0, 10},
          {10, 0, 0}, {10, 0, 10}, {10, 10, 10}, {10, 0, 0}, {10, 10, 10}, {10, 10, 0}};

        std::string path = (std::filesystem::temp_directory_path()
                            / ("agent_bench_box_" + std::to_string(::testing::UnitTest::GetInstance()
                                                                     ->random_seed())
                               + ".stl"))
                             .string();
        {
            std::FILE * out = std::fopen(path.c_str(), "wb");
            ASSERT_NE(out, nullptr);
            char header[80] = {};
            std::fwrite(header, 1, 80, out);
            std::uint32_t const count = static_cast<std::uint32_t>(tris.size() / 3u);
            std::fwrite(&count, 4, 1, out);
            for (std::size_t i = 0; i < tris.size(); i += 3u)
            {
                float normal[3] = {0.0f, 0.0f, 0.0f};
                std::fwrite(normal, 4, 3, out);
                for (std::size_t v = 0; v < 3u; ++v)
                {
                    std::fwrite(tris[i + v].data(), 4, 3, out);
                }
                std::uint16_t attr = 0;
                std::fwrite(&attr, 2, 1, out);
            }
            std::fclose(out);
        }

        // Act
        auto const facts = agent::bench::measureStl(path);
        std::remove(path.c_str());

        // Assert
        EXPECT_TRUE(facts.meshValid) << facts.error;
        EXPECT_EQ(facts.triangleCount, 12u);
        EXPECT_NEAR(facts.volumeMm3, 1000.0, 1.0e-6);
        EXPECT_NEAR(facts.axisSpans()[0], 10.0, 1.0e-6);
    }

    TEST(AgentBenchGeometry, WrongSizeModel_FailsSpanGate)
    {
        agent::bench::GeometryFacts facts;
        facts.meshValid = true;
        facts.triangleCount = 5000u;
        facts.volumeMm3 = 523.6;
        facts.meshMin = {-5.0, -5.0, -5.0};
        facts.meshMax = {5.0, 5.0, 9.0};

        auto const task = [&] {
            for (auto const & t : agent::bench::defaultBenchTasks())
            {
                if (t.id == "sphere_r5")
                {
                    return t;
                }
            }
            return agent::bench::AgentBenchTask{};
        }();

        auto const checks = agent::bench::evaluateGeometry(task, facts);
        EXPECT_FALSE(agent::bench::geometryPassed(checks));
        auto const failed = std::any_of(checks.begin(), checks.end(), [](auto const & c) {
            return !c.passed && c.name == "max_span_mm";
        });
        EXPECT_TRUE(failed);
    }

    TEST(AgentBenchGeometry, ReorientedBrick_SpanGatesMatchRegardlessOfAxis)
    {
        // Arrange: a correct 3x4 brick (24 x 32 x 11.4 mm) that the agent stood up on its side
        agent::bench::GeometryFacts facts;
        facts.meshValid = true;
        facts.triangleCount = 50000u;
        facts.volumeMm3 = 4000.0;
        facts.meshMin = {0.0, 0.0, 0.0};
        facts.meshMax = {11.4, 24.0, 32.0};
        facts.updateDerivedFacts();

        auto const task = [&] {
            for (auto const & t : agent::bench::defaultBenchTasks())
            {
                if (t.id == "lego_brick_3x4")
                {
                    return t;
                }
            }
            return agent::bench::AgentBenchTask{};
        }();

        // Act
        auto const checks = agent::bench::evaluateGeometry(task, facts);

        // Assert
        EXPECT_TRUE(agent::bench::geometryPassed(checks))
          << agent::bench::checksToJson(checks).dump();
    }

    TEST(AgentBenchGeometry, SolidBrickWithoutCavity_FailsFillGate)
    {
        // Arrange: correct brick outer size, but built as one solid block
        agent::bench::GeometryFacts facts;
        facts.meshValid = true;
        facts.triangleCount = 50000u;
        facts.volumeMm3 = 11.4 * 24.0 * 32.0;
        facts.meshMin = {0.0, 0.0, 0.0};
        facts.meshMax = {11.4, 24.0, 32.0};
        facts.updateDerivedFacts();

        auto const task = [&] {
            for (auto const & t : agent::bench::defaultBenchTasks())
            {
                if (t.id == "lego_brick_3x4")
                {
                    return t;
                }
            }
            return agent::bench::AgentBenchTask{};
        }();

        // Act
        auto const checks = agent::bench::evaluateGeometry(task, facts);

        // Assert
        EXPECT_FALSE(agent::bench::geometryPassed(checks));
        auto const failed = std::any_of(checks.begin(), checks.end(), [](auto const & c) {
            return !c.passed && c.name == "fill_fraction";
        });
        EXPECT_TRUE(failed) << agent::bench::checksToJson(checks).dump();
    }

    TEST(AgentBenchJudge, ValidVerdictJson_ParsesPassesAndIssues)
    {
        auto const parsed = agent::bench::AgentBenchJudge::extractJson(
          R"(Here you go: {"passes": false, "confidence": 0.7, "observed": "a cube", "issues": ["not round"]} thanks)");
        ASSERT_TRUE(parsed.is_object());
        EXPECT_EQ(parsed["passes"], false);
        EXPECT_NEAR(parsed["confidence"].get<double>(), 0.7, 1.0e-9);
        EXPECT_EQ(parsed["issues"].size(), 1u);
    }

    TEST(AgentBenchJudge, GarbageAnswer_LeavesVerdictUnavailable)
    {
        AgentBenchJudge judge{std::make_unique<ScriptedJudge>("I think it looks fine."), 64u};
        auto const verdict = judge.judge(agent::bench::defaultBenchTasks()[0], {kFakePng});
        EXPECT_FALSE(verdict.available);
        EXPECT_FALSE(verdict.passes);
        EXPECT_FALSE(verdict.error.empty());
    }

    TEST(AgentBenchJudge, JudgeCallIsToolless_AndReceivesEveryView)
    {
        auto scripted = std::make_unique<ScriptedJudge>(
          R"({"passes": true, "confidence": 0.9, "observed": "sphere", "issues": []})");
        auto * raw = scripted.get();
        AgentBenchJudge judge{std::move(scripted), 64u};
        auto const verdict = judge.judge(agent::bench::defaultBenchTasks()[0], {kFakePng, kFakePng});

        EXPECT_TRUE(verdict.available);
        EXPECT_TRUE(verdict.passes);
        EXPECT_EQ(raw->seenTools, 0u);
        EXPECT_EQ(raw->seenImages, 2u);
        EXPECT_EQ(judge.totalUsage().outputTokens, 7);
    }

    TEST(AgentBenchMetrics, CollectFrom_FoldsToolFailuresAndLibraryUsage)
    {
        // Arrange: a host-less event stream is enough for the collector itself
        agent::bench::AgentBenchMetricsCollector collector{"unit"};
        collector.record(ToolCallStartedEvent{"c1", "import_library_item", {{"category", "primitives"}, {"name", "sphere"}}});
        collector.record(ToolCallCompletedEvent{"c1", "import_library_item", nlohmann::json{{"success", true}}, true});
        collector.record(ToolCallStartedEvent{"c2", "import_library_item", {{"category", "primitives"}, {"name", "sphere"}}});
        collector.record(ToolCallCompletedEvent{"c2", "import_library_item", nlohmann::json{{"success", true}}, true});
        collector.record(ToolCallStartedEvent{"c3", "set_program_snippet", nlohmann::json::object()});
        collector.record(ToolCallCompletedEvent{"c3",
                                               "set_program_snippet",
                                               {{"success", false}, {"error", "bad snippet"}},
                                               false});
        collector.record(ToolCallStartedEvent{"c4", "no_such_tool", nlohmann::json::object()});
        collector.record(ToolCallCompletedEvent{"c4",
                                               "no_such_tool",
                                               {{"success", false}, {"error", "Unknown tool: no_such_tool"}},
                                               false});

        auto const & metrics = collector.metrics();
        EXPECT_EQ(metrics.toolCalls, 4u);
        EXPECT_EQ(metrics.toolFailures, 2u);
        EXPECT_EQ(metrics.unknownToolCalls, 1u);
        EXPECT_EQ(metrics.libraryImports, 1u) << "Repeated imports of one entry count once";
        EXPECT_EQ(metrics.perTool.at("set_program_snippet").failures, 1u);
        EXPECT_EQ(metrics.toJson()["taskId"], "unit");
    }

    /**
     * @brief Drives the whole harness with a scripted provider: real design tools, real geometry
     * measurement, real report writing - but no LLM.
     */
    TEST(AgentBenchHarness, ScriptedSphereCeremony_CompletesAndIsMeasured)
    {
        // Arrange
        BenchRunConfig config;
        config.renderWidth = 96;
        UserLibrarySandbox sandbox{std::filesystem::temp_directory_path()
                                   / "gladius_agent_bench_selftest_library"};
        HeadlessAgentHost host{config};
        auto const availability = host.initialize();
        if (!availability.available)
        {
            GTEST_SKIP() << availability.reason;
        }
        ASSERT_GT(host.toolCount(), 20u);

        ScriptedProvider provider;
        provider.scriptedUsage = TokenUsage{500, 40, 0, 0};
        pushSphereCeremony(provider);

        auto const outputDir = std::filesystem::temp_directory_path() / "gladius_agent_bench_selftest";
        AgentBenchReport report{config, outputDir};
        auto const task = [&] {
            for (auto const & t : defaultBenchTasks())
            {
                if (t.id == "sphere_r5")
                {
                    return t;
                }
            }
            return AgentBenchTask{};
        }();

        auto judge = std::make_unique<ScriptedJudge>(
          R"({"passes": true, "confidence": 0.8, "observed": "a round solid sphere", "issues": []})");

        // Act
        auto const record = report.runTask(host, task, std::make_unique<ScriptedProvider>(std::move(provider)),
                                           std::move(judge));

        // Assert: agent reached finish_design through the design-graph ceremony
        EXPECT_TRUE(record.metrics.agentCompleted) << record.metrics.agentError;
        EXPECT_EQ(record.metrics.modelTurns, 1u);
        EXPECT_EQ(record.metrics.toolCalls, 10u);
        EXPECT_EQ(record.metrics.toolFailures, 1u) << "Only the intentionally missing tool fails: "
                                       << record.metrics.toJson().dump();
        EXPECT_EQ(record.metrics.unknownToolCalls, 1u);
        EXPECT_EQ(record.metrics.viewportCaptures, 1u);
        EXPECT_EQ(record.metrics.librarySearches, 1u);
        EXPECT_EQ(record.metrics.libraryImports, 1u);
        EXPECT_EQ(record.metrics.designGraphMutations, 5u);
        EXPECT_EQ(record.metrics.usage.inputTokens, 500);
        EXPECT_EQ(record.metrics.usage.outputTokens, 40);
        EXPECT_EQ(record.metrics.providerName, "scripted");

        // Assert: geometry gates measured the scripted 5 mm sphere
        EXPECT_TRUE(record.geometry.meshValid) << record.geometry.error;
        EXPECT_NEAR(record.geometry.volumeMm3, 523.6, 60.0);
        EXPECT_TRUE(record.geometryPassed()) << record.geometry.toJson().dump();

        // Assert: verdict requires both judge and geometry
        EXPECT_TRUE(record.judge.available);
        EXPECT_TRUE(record.judge.passes);
        EXPECT_TRUE(record.taskPassed());
        EXPECT_EQ(record.judge.usage.outputTokens, 7);

        auto const resultsPath = report.write();
        std::ifstream in{resultsPath};
        ASSERT_TRUE(in.is_open());
        auto const doc = nlohmann::json::parse(in);
        EXPECT_EQ(doc["summary"]["passed"], 1);
        EXPECT_EQ(doc["runs"][0]["taskId"], "sphere_r5");

        // Assert: the same run is traceable, and the aggregate agrees with the counters
        ASSERT_TRUE(record.profile.has_value());
        EXPECT_EQ(record.profile->turns, record.metrics.modelTurns);
        EXPECT_EQ(record.profile->toolCalls, record.metrics.toolCalls);
        EXPECT_EQ(record.profile->failedToolCalls, record.metrics.toolFailures);
        EXPECT_GT(record.profile->harnessMs, 0) << "export, render and judge must be spans";
        EXPECT_GT(record.profile->totalPromptTokens, 0);
        auto const & traceJson = doc["runs"][0]["trace"];
        EXPECT_EQ(traceJson["path"], record.tracePath);
        EXPECT_GT(traceJson["profile"]["timeSplit"]["harnessMs"].get<std::int64_t>(), 0);
        EXPECT_TRUE(traceJson["profile"].contains("context"));

        std::error_code ec;
        EXPECT_TRUE(std::filesystem::exists(record.tracePath, ec)) << record.tracePath;
        auto const timeline = outputDir / "sphere_r5" / "timeline.json";
        EXPECT_TRUE(std::filesystem::exists(timeline, ec)) << timeline;

        std::ifstream traceIn{record.tracePath};
        std::vector<std::string> types;
        std::vector<std::string> phaseNames;
        std::string line;
        while (std::getline(traceIn, line))
        {
            if (line.empty())
            {
                continue;
            }
            auto const parsed = nlohmann::json::parse(line);
            types.push_back(parsed["type"].get<std::string>());
            if (parsed["type"] == "phase")
            {
                phaseNames.push_back(parsed["name"].get<std::string>());
            }
        }
        auto const hasType = [&](std::string const & type)
        { return std::find(types.begin(), types.end(), type) != types.end(); };
        auto const hasPhase = [&](std::string const & name)
        { return std::find(phaseNames.begin(), phaseNames.end(), name) != phaseNames.end(); };
        EXPECT_TRUE(hasType("run_meta"));
        EXPECT_TRUE(hasType("turn"));
        EXPECT_TRUE(hasType("tool_call"));
        for (auto const & phase : {"seed_document", "export_stl", "measure_mesh", "save_document",
                                   "render:isometric", "judge"})
        {
            EXPECT_TRUE(hasPhase(phase)) << phase << " missing from "
                                         << nlohmann::json(phaseNames).dump();
        }
        std::filesystem::remove_all(outputDir, ec);
    }

    TEST(AgentBenchHarness, ScriptedWrongGeometry_FailsGeometryGates)
    {
        // Arrange: same ceremony, but a 1 mm sphere instead of the requested 5 mm
        BenchRunConfig config;
        config.renderWidth = 96;
        UserLibrarySandbox sandbox{std::filesystem::temp_directory_path()
                                   / "gladius_agent_bench_selftest_library2"};
        HeadlessAgentHost host{config};
        auto const availability = host.initialize();
        if (!availability.available)
        {
            GTEST_SKIP() << availability.reason;
        }

        ScriptedProvider provider;
        provider.turns.push(CompletionResponse{
          "Modelling a too-small sphere.",
          {ToolCall{"t1", "set_program_snippet", {{"snippet", kTinySphereSnippet}}},
           ToolCall{"t2", "add_function", {{"name", "Body"}, {"type", "component"}}},
           ToolCall{"t3", "create_variant", {{"nodeId", "node_1"}, {"name", "Sphere"}}},
           ToolCall{"t4", "create_configuration",
                    {{"name", "A"}, {"selectedVariants", {{"node_1", "var_1"}}}}},
           ToolCall{"t5", "activate_configuration", {{"configurationId", "cfg_1"}}},
           ToolCall{"t6", "finish_design", nlohmann::json::object()}},
          "tool_calls",
          true,
          ""});

        AgentBenchReport report{config, std::filesystem::temp_directory_path()
                                          / "gladius_agent_bench_selftest2"};
        auto const task = [&] {
            for (auto const & t : defaultBenchTasks())
            {
                if (t.id == "sphere_r5")
                {
                    return t;
                }
            }
            return AgentBenchTask{};
        }();

        // Act
        auto const record = report.runTask(host,
                                           task,
                                           std::make_unique<ScriptedProvider>(std::move(provider)),
                                           std::make_unique<ScriptedJudge>(
                                             R"({"passes": true, "confidence": 0.9, "observed": "sphere", "issues": []})"));

        // Assert: the visual judge is happy, the deterministic gates are not
        EXPECT_TRUE(record.metrics.agentCompleted) << record.metrics.agentError;
        EXPECT_TRUE(record.judge.passes);
        EXPECT_FALSE(record.geometryPassed());
        EXPECT_FALSE(record.taskPassed());
        auto const failed = std::count_if(record.geometryChecks.begin(),
                                          record.geometryChecks.end(),
                                          [](GeometryCheck const & check) { return !check.passed; });
        EXPECT_EQ(failed, 4);
        auto const spans = record.geometry.axisSpans();
        EXPECT_NEAR(spans[0], 6.0, 0.05);
        EXPECT_NEAR(spans[1], 6.0, 0.05);
        EXPECT_NEAR(spans[2], 6.0, 0.05);

        std::error_code ec;
        std::filesystem::remove_all(std::filesystem::path{"gladius_agent_bench_selftest2"}, ec);
        std::filesystem::remove_all(std::filesystem::temp_directory_path()
                                      / "gladius_agent_bench_selftest2",
                                    ec);
    }

    TEST(AgentBenchTaskTable, DefaultTasks_AreCompleteAndDistinct)
    {
        auto const tasks = agent::bench::defaultBenchTasks();
        ASSERT_EQ(tasks.size(), 3u);
        for (auto const & task : tasks)
        {
            EXPECT_FALSE(task.id.empty());
            EXPECT_FALSE(task.prompt.empty());
            EXPECT_FALSE(task.judgeRubric.empty());
            EXPECT_FALSE(task.judgeViews.empty());
        }
        EXPECT_EQ(tasks[0].id, "sphere_r5");
        EXPECT_EQ(tasks[1].id, "sphere_gyroid");
        EXPECT_EQ(tasks[2].id, "lego_brick_3x4");
    }
} // namespace gladius::agent::bench::tests
