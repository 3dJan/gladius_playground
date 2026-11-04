#include "ui/UiFpsBenchmark.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace gladius::ui::tests
{
    TEST(UiFpsBenchmark, ReadyAndPresentedDocument_ReportsFrameIntervalStatistics)
    {
        using Clock = UiFpsBenchmark::Clock;
        auto const outputPath = std::filesystem::temp_directory_path() /
                                ("gladius-ui-fps-" +
                                 std::to_string(Clock::now().time_since_epoch().count()) + ".json");
        UiFpsBenchmark benchmark{outputPath, std::chrono::seconds{1}, std::chrono::seconds{0},
                                 std::chrono::seconds{5}};
        auto const syntheticStart = Clock::now() + std::chrono::milliseconds{10};

        UiFpsBenchmarkState state;
        auto frameStart = syntheticStart + std::chrono::milliseconds{1};
        auto frameEnd = frameStart + std::chrono::milliseconds{2};
        EXPECT_FALSE(benchmark.recordFrame(frameStart, frameEnd, state, 1280u, 720u,
                                           {1.0f, 1.0f}, false));

        state.documentLoaded = true;
        frameStart += std::chrono::milliseconds{16};
        frameEnd = frameStart + std::chrono::milliseconds{2};
        EXPECT_FALSE(benchmark.recordFrame(frameStart, frameEnd, state, 1280u, 720u,
                                           {1.0f, 1.0f}, false));

        state.previewPresented = true;
        state.stageTimings.schedulerPollMs = 0.25;
        state.stageTimings.sceneSynchronizationMs = 1.5;
        state.stageTimings.backendSceneReplacementMs = 0.75;
        state.stageTimings.bufferSwapMs = 0.125;
        frameStart += std::chrono::milliseconds{16};
        frameEnd = frameStart + std::chrono::milliseconds{2};
        EXPECT_FALSE(benchmark.recordFrame(frameStart, frameEnd, state, 1280u, 720u,
                                           {1.0f, 1.0f}, false));

        state.renderSettled = true;
        state.previewViewportPx = {640u, 360u};
        state.backend = "webgpu";
        state.uiScale = 1.25f;
        frameStart += std::chrono::milliseconds{16};
        frameEnd = frameStart + std::chrono::milliseconds{2};
        EXPECT_FALSE(benchmark.recordFrame(frameStart, frameEnd, state, 1280u, 720u,
                                           {1.0f, 1.0f}, false));

        bool completed = false;
        for (std::size_t frame = 0u; frame < 70u && !completed; ++frame)
        {
            frameStart += std::chrono::milliseconds{16};
            frameEnd = frameStart + std::chrono::milliseconds{2};
            completed = benchmark.recordFrame(frameStart, frameEnd, state, 1280u, 720u,
                                              {1.0f, 1.0f}, false);
        }
        ASSERT_TRUE(completed);

        std::ifstream input{outputPath};
        ASSERT_TRUE(input.is_open());
        std::string const contents{std::istreambuf_iterator<char>{input},
                                   std::istreambuf_iterator<char>{}};
        auto const report = nlohmann::json::parse(contents);
        EXPECT_EQ(report.at("status"), "complete");
        EXPECT_EQ(report.at("backend"), "webgpu");
        EXPECT_TRUE(report.at("milestonesMs").at("documentLoaded").is_number());
        EXPECT_TRUE(report.at("milestonesMs").at("firstPreviewPresented").is_number());
        EXPECT_TRUE(report.at("milestonesMs").at("firstSettledFrame").is_number());
        EXPECT_GT(report.at("measurement").at("intervalCount").get<std::size_t>(), 50u);
        EXPECT_GT(report.at("renderingStageDurationsMs").at("frameCount").get<std::size_t>(), 0u);
        EXPECT_NEAR(report.at("renderingStageDurationsMs").at("schedulerPoll").at("p50").get<double>(),
                0.25,
                0.001);
        EXPECT_NEAR(report.at("renderingStageDurationsMs").at("sceneSynchronization").at("p50").get<double>(),
                1.5,
                0.001);
        EXPECT_NEAR(report.at("renderingStageDurationsMs").at("backendSceneReplacement").at("p50").get<double>(),
            0.75,
            0.001);
        EXPECT_NEAR(report.at("renderingStageDurationsMs").at("bufferSwap").at("p50").get<double>(),
            0.125,
            0.001);
        EXPECT_NEAR(report.at("measurement").at("frameIntervalMs").at("mean").get<double>(),
                    16.0,
                    0.1);
        EXPECT_GT(report.at("measurement").at("effectiveFps").get<double>(), 60.0);
        EXPECT_EQ(report.at("frames").back().at("phase"), "complete");
        EXPECT_EQ(report.at("frames").back().at("displaySize"), nlohmann::json::array({1280u, 720u}));
        EXPECT_EQ(report.at("frames").back().at("previewViewportPx"),
                  nlohmann::json::array({640u, 360u}));

        std::filesystem::remove(outputPath);
    }

    TEST(UiFpsBenchmark, InterruptedMeasurement_RestartsWarmupAndContiguousWindow)
    {
        using Clock = UiFpsBenchmark::Clock;
        auto const outputPath = std::filesystem::temp_directory_path() /
                                ("gladius-ui-fps-interrupted-" +
                                 std::to_string(Clock::now().time_since_epoch().count()) + ".json");
        UiFpsBenchmark benchmark{outputPath, std::chrono::seconds{1}, std::chrono::seconds{1},
                                 std::chrono::seconds{5}};
        auto const syntheticStart = Clock::now() + std::chrono::milliseconds{10};

        UiFpsBenchmarkState state;
        state.documentLoaded = true;
        state.previewPresented = true;
        state.renderSettled = true;

        auto frameStart = syntheticStart + std::chrono::milliseconds{1};
        auto frameEnd = frameStart + std::chrono::milliseconds{2};
        EXPECT_FALSE(benchmark.recordFrame(frameStart, frameEnd, state, 1280u, 720u,
                                           {1.0f, 1.0f}, false));

        frameStart += std::chrono::milliseconds{996};
        frameEnd = frameStart + std::chrono::milliseconds{2};
        EXPECT_FALSE(benchmark.recordFrame(frameStart, frameEnd, state, 1280u, 720u,
                                           {1.0f, 1.0f}, false));

        state.renderSettled = false;
        frameStart += std::chrono::milliseconds{16};
        frameEnd = frameStart + std::chrono::milliseconds{2};
        EXPECT_FALSE(benchmark.recordFrame(frameStart, frameEnd, state, 1280u, 720u,
                                           {1.0f, 1.0f}, false));

        frameStart += std::chrono::milliseconds{16};
        frameEnd = frameStart + std::chrono::milliseconds{2};
        EXPECT_FALSE(benchmark.recordFrame(frameStart, frameEnd, state, 1280u, 720u,
                                           {1.0f, 1.0f}, false));

        state.renderSettled = true;
        frameStart += std::chrono::milliseconds{16};
        frameEnd = frameStart + std::chrono::milliseconds{2};
        EXPECT_FALSE(benchmark.recordFrame(frameStart, frameEnd, state, 1280u, 720u,
                                           {1.0f, 1.0f}, false));

        bool completed = false;
        for (std::size_t frame = 0u; frame < 150u && !completed; ++frame)
        {
            frameStart += std::chrono::milliseconds{16};
            frameEnd = frameStart + std::chrono::milliseconds{2};
            completed = benchmark.recordFrame(frameStart, frameEnd, state, 1280u, 720u,
                                              {1.0f, 1.0f}, false);
        }
        ASSERT_TRUE(completed);

        std::ifstream input{outputPath};
        ASSERT_TRUE(input.is_open());
        auto const report = nlohmann::json::parse(std::string{std::istreambuf_iterator<char>{input},
                                                              std::istreambuf_iterator<char>{}});
        EXPECT_EQ(report.at("status"), "complete");
        EXPECT_GT(report.at("measurement").at("intervalCount").get<std::size_t>(), 50u);
        auto const frames = report.at("frames");
        auto const interrupted = std::find_if(frames.begin(), frames.end(), [](auto const & frame)
        {
            return frame.at("phase") == "interrupted";
        });
        ASSERT_NE(interrupted, frames.end());
        EXPECT_FALSE(interrupted->at("measured").get<bool>());
        EXPECT_FALSE(interrupted->at("measuredInterval").get<bool>());

        std::filesystem::remove(outputPath);
    }
}
