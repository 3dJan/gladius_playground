#include "agent/AgentLoop.h"
#include "agent/trace/AgentTrace.h"
#include "agent/trace/AgentTraceProfile.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <gtest/gtest.h>
#include <queue>
#include <thread>

namespace gladius::agent::trace::tests
{
    namespace
    {
        class ScriptedProvider final : public IAiProvider
        {
          public:
            std::queue<CompletionResponse> responses;

            std::string name() const override
            {
                return "scripted";
            }

            coro::task<CompletionResponse> complete(CompletionRequest const &,
                                                    TextDeltaCallback,
                                                    io::CancellationToken const &) override
            {
                if (responses.empty())
                {
                    co_return CompletionResponse{"Done", {}, "stop", true, ""};
                }
                auto response = std::move(responses.front());
                responses.pop();
                co_return response;
            }
        };

        [[nodiscard]] nlohmann::json readJsonFile(std::filesystem::path const & path)
        {
            std::ifstream in(path);
            auto const parsed = nlohmann::json::parse(in, nullptr, false);
            return parsed.is_discarded() ? nlohmann::json{} : parsed;
        }

        [[nodiscard]] std::vector<nlohmann::json> recordsOfType(
          std::vector<nlohmann::json> const & records,
          std::string const & type)
        {
            std::vector<nlohmann::json> out;
            for (auto const & record : records)
            {
                if (record.value("type", std::string{}) == type)
                {
                    out.push_back(record);
                }
            }
            return out;
        }

        /// @brief Runs one scripted turn that calls `name`, returning the recorder afterwards
        [[nodiscard]] AgentTraceRecorder traceScriptedRun(tools::ToolRegistry & registry,
                                                          tools::ToolExecutor const & executor,
                                                          std::vector<CompletionResponse> responses,
                                                          bool designComplete)
        {
            ScriptedProvider provider;
            for (auto & response : responses)
            {
                provider.responses.push(std::move(response));
            }

            Conversation conversation;
            conversation.appendUserText("Design a sphere");
            ContextBuilder contextBuilder;
            PermissionPolicy policy;
            AgentEventQueue queue;
            AgentLimits limits;
            io::CancellationToken cancellation;
            AgentLoop loop;
            AgentTraceRecorder recorder;
            recorder.setRunId("loop-test");

            (void) coro::sync_wait(loop.run(conversation,
                                            provider,
                                            registry,
                                            executor,
                                            contextBuilder,
                                            policy,
                                            queue,
                                            limits,
                                            cancellation,
                                            [designComplete]() { return designComplete; },
                                            nullptr,
                                            nullptr,
                                            &recorder));
            return recorder;
        }
    } // namespace

    TEST(AgentTrace, BreakdownRequest_SplitsPromptByComponent)
    {
        // Arrange
        CompletionRequest request;
        request.systemPrompt = "system prompt text";
        request.tools.push_back(
          tools::ToolDefinition{"one", "first tool", {{"type", "object"}}, tools::ToolPolicy::ReadOnly});
        request.tools.push_back(tools::ToolDefinition{
          "two", "second tool", {{"type", "object"}}, tools::ToolPolicy::DesignGraphModification});

        Conversation conversation;
        conversation.appendUserText("make a sphere");
        conversation.appendAssistant("calling a tool", {ToolCall{"c1", "one", {{"radius", 5.0}}}});
        conversation.appendToolResult("c1", {{"success", true}, {"mesh", "lots of bytes"}});
        conversation.appendAssistant("done");
        conversation.appendUserImage(std::vector<std::byte>(64, std::byte{7}));
        request.messages = conversation.messages();

        // Act
        auto const breakdown = breakdownRequest(request);
        auto const callArgsBytes = static_cast<std::int64_t>(
          nlohmann::json{{"radius", 5.0}}.dump().size());

        // Assert: scaffolding, injected context and model history stay separable
        EXPECT_EQ(breakdown.systemPromptBytes,
                  static_cast<std::int64_t>(request.systemPrompt.size()));
        EXPECT_EQ(breakdown.toolDefinitions, 2u);
        EXPECT_GT(breakdown.toolSchemaBytes, 0);
        EXPECT_EQ(breakdown.userTextBytes,
                  static_cast<std::int64_t>(std::string{"make a sphere"}.size()));
        EXPECT_EQ(breakdown.assistantTextBytes,
                  static_cast<std::int64_t>(std::string{"calling a tooldone"}.size()));
        EXPECT_EQ(breakdown.toolCallBytes, callArgsBytes);
        EXPECT_EQ(breakdown.imageBytes, 64);
        EXPECT_GT(breakdown.toolResultBytes, 0);
        EXPECT_EQ(breakdown.messages, 5u);
        EXPECT_EQ(breakdown.totalBytes(),
                  breakdown.systemPromptBytes + breakdown.toolSchemaBytes + breakdown.userTextBytes
                    + breakdown.assistantTextBytes + breakdown.toolCallBytes
                    + breakdown.toolResultBytes + breakdown.imageBytes);
    }

    TEST(AgentTrace, Recorder_WritesOneJsonlLinePerRecord)
    {
        // Arrange
        auto const dir = std::filesystem::temp_directory_path() / "gladius_trace_jsonl_test";
        std::filesystem::remove_all(dir);
        auto const path = dir / "trace.jsonl";

        {
            // Act
            AgentTraceRecorder recorder(path);
            recorder.setRunId("task-a");
            recorder.recordRunMeta({{"model", "iq3_s"}});
            recorder.recordTurn({{"turnIndex", 1}, {"durationMs", 10}});
            recorder.recordToolCall({{"name", "set_function_snippet"}, {"success", false}});
            recorder.recordNote("autonomous_nudge", {{"turnIndex", 2}});
            recorder.recordPhase("render", 0);
        }

        // Assert
        std::vector<nlohmann::json> parsed;
        std::ifstream in(path);
        std::string line;
        while (std::getline(in, line))
        {
            if (line.empty())
            {
                continue;
            }
            auto const value = nlohmann::json::parse(line, nullptr, false);
            ASSERT_FALSE(value.is_discarded()) << line;
            parsed.push_back(value);
        }
        ASSERT_EQ(parsed.size(), 5u);
        EXPECT_EQ(parsed[0]["type"], "run_meta");
        EXPECT_EQ(parsed[1]["type"], "turn");
        EXPECT_EQ(parsed[2]["type"], "tool_call");
        EXPECT_EQ(parsed[3]["kind"], "autonomous_nudge");
        EXPECT_EQ(parsed[4]["name"], "render");
        for (auto const & record : parsed)
        {
            EXPECT_TRUE(record.contains("tMs"));
            EXPECT_EQ(record["runId"], "task-a");
        }
        EXPECT_GE(parsed[4]["durationMs"].get<std::int64_t>(), 0);
        EXPECT_EQ(path.filename().string(), "trace.jsonl");
        std::filesystem::remove_all(dir);
    }

    TEST(AgentTrace, ScopedTracePhase_NullRecorderIsNoOp)
    {
        // Arrange / Act: optionally traced code must not need its own null check
        AgentTraceRecorder * const none = nullptr;
        {
            ScopedTracePhase span(none, "nothing");
            span.addFields({{"ignored", true}});
        }

        // Assert
        AgentTraceRecorder recorder;
        EXPECT_TRUE(recorder.records().empty());
        {
            ScopedTracePhase span(&recorder, "seed_document");
            span.addFields({{"triangles", 12}});
        }
        ASSERT_EQ(recorder.recordCount(), 1u);
        EXPECT_EQ(recorder.records()[0]["type"], "phase");
        EXPECT_EQ(recorder.records()[0]["name"], "seed_document");
        EXPECT_EQ(recorder.records()[0]["triangles"], 12);
        EXPECT_TRUE(recorder.outputFile().empty());
        EXPECT_TRUE(recorder.timelinePath().empty());
    }

    TEST(AgentTrace, Run_EmitsTurnAndToolCallRecords)
    {
        // Arrange
        tools::ToolRegistry registry;
        registry.registerSyncTool(
          {"finish_design",
           "Finish",
           {{"type", "object"}},
           tools::ToolPolicy::DesignGraphModification},
          [](nlohmann::json const &)
          { return nlohmann::json{{"success", true}, {"detail", std::string(400, 'x')}}; });
        tools::ToolExecutor executor(registry);

        // Act
        auto const recorder = traceScriptedRun(
          registry,
          executor,
          {CompletionResponse{"Finishing now",
                              {ToolCall{"c1", "finish_design", nlohmann::json::object()}},
                              "tool_calls",
                              true,
                              ""}},
          true);

        // Assert
        auto const turns = recordsOfType(recorder.records(), "turn");
        auto const toolCalls = recordsOfType(recorder.records(), "tool_call");
        // The scripted provider answers the follow-up turn without tool calls, so the run ends there
        ASSERT_EQ(turns.size(), 2u);
        ASSERT_EQ(toolCalls.size(), 1u);

        auto const & turn = turns[0];
        EXPECT_EQ(turn["turnIndex"], 1);
        EXPECT_GE(turn["durationMs"].get<std::int64_t>(), turn["buildMs"].get<std::int64_t>());
        EXPECT_TRUE(turn.contains("promptBytes"));
        EXPECT_GT(turn["promptBytes"]["systemPromptBytes"].get<std::int64_t>(), 0);
        EXPECT_GT(turn["promptBytes"]["toolSchemaBytes"].get<std::int64_t>(), 0);
        EXPECT_FALSE(turn.contains("request"));

        auto const & tool = toolCalls[0];
        EXPECT_EQ(tool["name"], "finish_design");
        EXPECT_EQ(tool["policy"], "design_graph_modification");
        EXPECT_TRUE(tool["success"].get<bool>());
        EXPECT_FALSE(tool["denied"].get<bool>());
        EXPECT_EQ(tool["turnIndex"], 1);
        EXPECT_EQ(tool["callId"], "c1");
        EXPECT_GE(tool["durationMs"].get<std::int64_t>(), 0);
        EXPECT_GT(tool["resultBytes"].get<std::int64_t>(), 400);
    }

    TEST(AgentTrace, Run_PromptDumpAddsRequestOnlyWhenEnabled)
    {
        // Arrange
        tools::ToolRegistry registry;
        registry.registerSyncTool(
          {"finish_design", "Finish", {{"type", "object"}}, tools::ToolPolicy::DesignGraphModification},
          [](nlohmann::json const &) { return nlohmann::json{{"success", true}}; });
        tools::ToolExecutor executor(registry);
        ScriptedProvider provider;
        provider.responses.push(CompletionResponse{
          "Finishing now",
          {ToolCall{"c1", "finish_design", nlohmann::json::object()}},
          "tool_calls",
          true,
          ""});

        Conversation conversation;
        conversation.appendUserText("Design a sphere");
        ContextBuilder contextBuilder;
        PermissionPolicy policy;
        AgentEventQueue queue;
        AgentLimits limits;
        io::CancellationToken cancellation;
        AgentLoop loop;
        AgentTraceRecorder recorder;
        recorder.setPromptDumpEnabled(true);

        // Act
        (void) coro::sync_wait(loop.run(conversation,
                                        provider,
                                        registry,
                                        executor,
                                        contextBuilder,
                                        policy,
                                        queue,
                                        limits,
                                        cancellation,
                                        []() { return true; },
                                        nullptr,
                                        nullptr,
                                        &recorder));

        // Assert
        auto const turns = recordsOfType(recorder.records(), "turn");
        ASSERT_FALSE(turns.empty());
        ASSERT_TRUE(turns[0].contains("request"));
        EXPECT_TRUE(turns[0]["request"]["systemPrompt"].is_string());
        EXPECT_FALSE(turns[0]["request"]["systemPrompt"].get<std::string>().empty());
        EXPECT_TRUE(turns[0]["request"]["messages"].is_array());
    }

    TEST(AgentTrace, Run_FailedToolMarksNextTurnAsRetry)
    {
        // Arrange
        tools::ToolRegistry registry;
        registry.registerSyncTool(
          {"broken", "Fails", {{"type", "object"}}, tools::ToolPolicy::ReadOnly},
          [](nlohmann::json const &)
          { return nlohmann::json{{"success", false}, {"error", "boom"}}; });
        tools::ToolExecutor executor(registry);

        // Act: the tool fails in turn 1, the model speaks again in turn 2
        auto const recorder = traceScriptedRun(
          registry,
          executor,
          {CompletionResponse{"Trying",
                              {ToolCall{"c1", "broken", nlohmann::json::object()}},
                              "tool_calls",
                              true,
                              ""},
           CompletionResponse{"Retrying", {}, "stop", true, ""}},
          false);

        // Assert
        auto const notes = recordsOfType(recorder.records(), "note");
        ASSERT_EQ(notes.size(), 1u);
        EXPECT_EQ(notes[0]["kind"], "retry_after_failure");
        EXPECT_EQ(notes[0]["turnIndex"], 2);
        auto const toolCalls = recordsOfType(recorder.records(), "tool_call");
        ASSERT_EQ(toolCalls.size(), 1u);
        EXPECT_FALSE(toolCalls[0]["success"].get<bool>());
        EXPECT_EQ(toolCalls[0]["error"], "boom");

        auto const profile = analyzeTrace(recorder.records());
        EXPECT_EQ(profile.failedToolCalls, 1u);
        EXPECT_EQ(profile.turnsAfterFailure, 1u);
        EXPECT_GE(profile.turnsAfterFailureMs, 0);
        EXPECT_EQ(profile.turns, 2u);
    }

    TEST(AgentTraceProfile, AnalyzeTrace_SplitsTimeAndFlagsWaste)
    {
        // Arrange: one truncated turn, one failing tool, one harness phase
        std::vector<nlohmann::json> records;
        records.push_back(
          {{"type", "turn"},
           {"turnIndex", 1},
           {"tMs", 100},
           {"startedMarkMs", 0},
           {"durationMs", 100},
           {"buildMs", 10},
           {"completionMs", 90},
           {"ttftMs", 30},
           {"toolCallsIssued", 2},
           {"thinkingChars", 500},
           {"textChars", 20},
           {"toolJsonChars", 80},
           {"stopReason", "tool_use"},
           {"usage", {{"totalPromptTokens", 1000}, {"outputTokens", 90}}},
           {"promptBytes",
            {{"systemPromptBytes", 4000}, {"toolSchemaBytes", 1000}, {"totalBytes", 5000}}}});
        records.push_back({{"type", "tool_call"},
                           {"turnIndex", 1},
                           {"tMs", 140},
                           {"durationMs", 30},
                           {"name", "set_function_snippet"},
                           {"policy", "design_graph_modification"},
                           {"success", false},
                           {"resultBytes", 240}});
        records.push_back({{"type", "tool_call"},
                           {"turnIndex", 1},
                           {"tMs", 170},
                           {"durationMs", 10},
                           {"name", "get_model_summary"},
                           {"success", true},
                           {"resultBytes", 900}});
        records.push_back({{"type", "note"}, {"turnIndex", 2}, {"kind", "retry_after_failure"}});
        records.push_back(
          {{"type", "turn"},
           {"turnIndex", 2},
           {"tMs", 300},
           {"startedMarkMs", 180},
           {"durationMs", 120},
           {"buildMs", 8},
           {"completionMs", 112},
           {"ttftMs", 12},
           {"stopReason", "max_tokens"},
           {"usage", {{"totalPromptTokens", 1400}, {"outputTokens", 500}}}});
        records.push_back({{"type", "phase"}, {"name", "render"}, {"tMs", 420}, {"durationMs", 40}});
        records.push_back({{"type", "phase"}, {"name", "judge"}, {"tMs", 460}, {"durationMs", 20}});

        // Act
        auto const profile = analyzeTrace(records, 500);

        // Assert: every millisecond is attributed, nothing is silently folded into model time
        EXPECT_EQ(profile.turns, 2u);
        EXPECT_EQ(profile.toolCalls, 2u);
        EXPECT_EQ(profile.contextBuildMs, 18);
        EXPECT_EQ(profile.modelMs, 202);
        // Model time splits into prompt prefill and output generation: only the first is reduced
        // by trimming context, only the second by shorter outputs.
        EXPECT_EQ(profile.modelPrefillMs, 42);
        EXPECT_EQ(profile.modelGenerateMs, 160);
        EXPECT_EQ(profile.toolMs, 40);
        EXPECT_EQ(profile.harnessMs, 60);
        EXPECT_EQ(profile.judgeMs, 20);
        EXPECT_EQ(profile.unaccountedMs, 500 - (18 + 202 + 40 + 60));

        // Context growth: turn 2 pays again for everything turn 1 produced
        EXPECT_EQ(profile.firstTurnPromptTokens, 1000);
        EXPECT_EQ(profile.lastTurnPromptTokens, 1400);
        EXPECT_EQ(profile.totalPromptTokens, 2400);
        EXPECT_DOUBLE_EQ(profile.contextTax, 2400.0 / 1400.0);
        EXPECT_EQ(profile.promptBytesByComponent["systemPromptBytes"], 4000);
        EXPECT_EQ(profile.promptBytesByComponent["toolSchemaBytes"], 1000);
        EXPECT_EQ(profile.largestToolResultBytes, 900);
        EXPECT_EQ(profile.largestToolResultTurn, 1u);

        // Waste attribution
        EXPECT_EQ(profile.failedToolCalls, 1u);
        EXPECT_EQ(profile.truncatedTurns, 1u);
        EXPECT_EQ(profile.turnsAfterFailure, 1u);
        EXPECT_EQ(profile.turnsAfterFailureMs, 120);
        EXPECT_EQ(profile.turnsAfterFailurePromptTokens, 1400);
        // The second turn issued no tool call, so it bought no progress
        EXPECT_EQ(profile.turnsWithoutToolCalls, 1u);
        EXPECT_EQ(profile.readOnlyTurnsMs, 120);
        EXPECT_EQ(profile.mutatingToolCalls, 1u);

        // Tools rank by total wall time
        ASSERT_EQ(profile.tools.size(), 2u);
        EXPECT_EQ(profile.tools[0].name, "set_function_snippet");
        EXPECT_EQ(profile.tools[0].totalMs, 30);
        EXPECT_EQ(profile.tools[0].failures, 1u);
        EXPECT_EQ(profile.tools[1].name, "get_model_summary");

        EXPECT_EQ(profile.turnDurationMaxMs, 120);
        EXPECT_EQ(profile.thinkingChars, 500u);
        auto const json = profile.toJson();
        EXPECT_EQ(json["timeSplit"]["unaccountedMs"], profile.unaccountedMs);
        EXPECT_EQ(json["context"]["totalPromptTokens"], 2400);
        EXPECT_FALSE(formatProfileSummary(profile).empty());
    }

    TEST(AgentTraceProfile, AnalyzeTrace_FallsBackToTraceExtentForWallTime)
    {
        // Arrange
        std::vector<nlohmann::json> records;
        records.push_back({{"type", "turn"}, {"turnIndex", 1}, {"tMs", 250}, {"durationMs", 250}});

        // Act
        auto const profile = analyzeTrace(records);

        // Assert
        EXPECT_EQ(profile.wallMs, 250);
        EXPECT_EQ(profile.modelMs, 250);
        EXPECT_EQ(profile.unaccountedMs, 0);
    }

    TEST(AgentTraceProfile, AnalyzeTrace_MeasuresPrefillUntilFirstContent)
    {
        // Arrange: the stream opened early but the server produced nothing for 800 ms
        std::vector<nlohmann::json> records;
        records.push_back(
          {{"type", "turn"},
           {"turnIndex", 1},
           {"tMs", 900},
           {"durationMs", 900},
           {"buildMs", 0},
           {"completionMs", 900},
           {"ttftMs", 5},
           {"firstContentMs", 800},
           {"usage", {{"totalPromptTokens", 2000}, {"outputTokens", 20}}}});

        // Act
        auto const profile = analyzeTrace(records, 900);

        // Assert: otherwise the endpoint would look like it prefilled 2000 tokens in 5 ms
        EXPECT_EQ(profile.ttftP50Ms, 800);
        EXPECT_DOUBLE_EQ(profile.meanPrefillTokensPerSec, 2000.0 / 0.8);
    }

    TEST(AgentTraceProfile, AnalyzeTrace_FlagsPromptCacheLossAndContextShaping)
    {
        // Arrange: turn 2 sends a different system prompt and gets nothing back from the cache
        std::vector<nlohmann::json> records;
        records.push_back({{"type", "turn"},
                           {"turnIndex", 1},
                           {"tMs", 1000},
                           {"durationMs", 1000},
                           {"completionMs", 1000},
                           {"ttftMs", 900},
                           {"firstContentMs", 900},
                           {"systemPromptHash", 1},
                           {"context",
                            {{"imagesKept", 1},
                             {"imagesDropped", 2},
                             {"toolResultsKept", 6},
                             {"toolResultsCompacted", 3}}},
                           {"promptBytes", {{"imageBytes", 120}, {"images", 1}}},
                           {"usage", {{"totalPromptTokens", 5000}, {"cacheReadTokens", 4000}}}});
        records.push_back({{"type", "turn"},
                           {"turnIndex", 2},
                           {"tMs", 2000},
                           {"durationMs", 1000},
                           {"completionMs", 1000},
                           {"ttftMs", 990},
                           {"firstContentMs", 990},
                           {"systemPromptHash", 2},
                           {"context", {{"imagesKept", 1}, {"imagesDropped", 0}}},
                           {"promptBytes", {{"imageBytes", 0}, {"images", 0}}},
                           {"usage", {{"totalPromptTokens", 6000}, {"cacheReadTokens", 0}}}});

        // Act
        auto const profile = analyzeTrace(records, 2000);

        // Assert
        EXPECT_EQ(profile.systemPromptMutations, 1u);
        EXPECT_EQ(profile.cacheMissTurns, 1u);
        EXPECT_EQ(profile.imagesKept, 2u);
        EXPECT_EQ(profile.imagesDropped, 2u);
        EXPECT_EQ(profile.compactedToolResults, 3u);
        EXPECT_EQ(profile.imagesInHistoryLastTurn, 0u);
    }

    TEST(AgentTrace, WriteTimeline_EmitsValidChromeEvents)
    {
        // Arrange
        auto const dir = std::filesystem::temp_directory_path() / "gladius_trace_timeline_test";
        std::filesystem::remove_all(dir);
        AgentTraceRecorder recorder(dir / "trace.jsonl");
        recorder.setRunId("task-b");
        recorder.recordRunMeta({{"model", "iq3_s"}});
        // A turn span is positioned from its own duration, so the durations have to match elapsed
        // time for the timeline offsets to stay non-negative.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        recorder.recordTurn({{"turnIndex", 1},
                             {"startedMarkMs", 0},
                             {"durationMs", recorder.mark()},
                             {"buildMs", 8},
                             {"completionMs", recorder.mark() - 8},
                             {"ttftMs", 5}});
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        recorder.recordToolCall({{"name", "export_mesh"},
                                 {"startedMarkMs", 25},
                                 {"durationMs", 5},
                                 {"success", true}});
        recorder.recordNote("truncation_nudge", {{"turnIndex", 2}});
        {
            ScopedTracePhase phase(&recorder, "judge");
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        // Act
        std::string error;
        auto const timelinePath = recorder.timelinePath();
        ASSERT_TRUE(recorder.writeTimeline(timelinePath, error)) << error;
        auto const timeline = readJsonFile(timelinePath);

        // Assert: run_meta is not a span, a turn is split into nested slices
        ASSERT_TRUE(timeline.is_object());
        ASSERT_TRUE(timeline.contains("traceEvents"));
        std::size_t spans = 0;
        std::size_t instants = 0;
        std::int64_t maxEndMicros = 0;
        for (auto const & event : timeline["traceEvents"])
        {
            EXPECT_EQ(event["pid"], 1);
            EXPECT_TRUE(event.contains("tid"));
            EXPECT_GE(event["ts"].get<std::int64_t>(), 0);
            if (event["ph"] == "X")
            {
                ++spans;
                EXPECT_GT(event["dur"].get<std::int64_t>(), 0);
                maxEndMicros = std::max(maxEndMicros,
                                        event["ts"].get<std::int64_t>()
                                          + event["dur"].get<std::int64_t>());
            }
            else
            {
                ++instants;
                EXPECT_EQ(event["ph"], "i");
            }
        }
        // turn base span plus its three nested slices, then tool, phase; note is an instant
        EXPECT_EQ(spans, 6u);
        EXPECT_EQ(instants, 1u);
        EXPECT_GE(maxEndMicros, 20 * 1000);
        std::filesystem::remove_all(dir);
    }

    TEST(AgentTrace, WriteTimeline_UnwritablePathReportsError)
    {
        // Arrange
        AgentTraceRecorder recorder;
        std::string error;

        // Act
        bool const ok = recorder.writeTimeline("/proc/no-such-dir/timeline.json", error);

        // Assert
        EXPECT_FALSE(ok);
        EXPECT_FALSE(error.empty());
    }
} // namespace gladius::agent::trace::tests
