#include "WebGPURayMarchHarness.h"

#include "ui/render/NeutralRenderScheduler.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace gladius::tests
{
#if defined(GLADIUS_ENABLE_WEBGPU)
    namespace
    {
        /// @brief One progressive benchmark configuration: model file and resolution.
        struct ProgressiveConfig
        {
            std::string_view model;
            std::uint32_t resolution;
            std::uint32_t repetitions;
        };

        /// @brief Aggregated progressive statistics for one configuration.
        struct ProgressiveSample
        {
            std::string model;
            std::uint32_t resolution{};
            std::uint32_t repetitions{};
            std::size_t chunkPixels{};
            std::uint32_t chunks{};
            /// Simulated UI frame period the sequence was paced with; 0 means unpaced.
            double paceMs{};
            /// Wall time until the first chunk delivered pixels.
            double timeToFirstPixelMs{};
            /// Wall time until the whole frame reached full quality; the headline metric.
            double timeToHqMs{};
            double p95TimeToHqMs{};
            /// Median per-chunk numbers across the whole sequence.
            double medianChunkWallMs{};
            double medianChunkGpuMs{};
            double medianChunkSubmitToWorkDoneMs{};
            double medianChunkReadbackMs{};
            /// Median CPU time spent building and submitting a chunk.
            double medianChunkSubmitCallMs{};
            /// Median CPU time spent pumping completion callbacks.
            double medianChunkPumpMs{};
            /// Median idle time a chunk waited for the next UI frame before its successor started.
            double medianChunkPaceMs{};
            /// Median of everything that is not GPU work; the cost of submitting at all.
            double medianChunkFixedOverheadMs{};
            double gpuTimeShare{};
        };

        [[nodiscard]] double percentile(std::vector<double> sortedValues, double const fraction)
        {
            if (sortedValues.empty())
            {
                return 0.0;
            }
            auto const index = static_cast<std::size_t>(
              std::clamp(fraction * static_cast<double>(sortedValues.size() - 1u), 0.0,
                         static_cast<double>(sortedValues.size() - 1u)));
            return sortedValues[index];
        }

        /// @brief Chunk budget the progressive scheduler starts a sequence with at this resolution.
        [[nodiscard]] std::size_t currentSchedulerChunkPixels(std::uint32_t const resolution)
        {
            return ui::async_rendering::NeutralRenderScheduler::budgetForResolution(resolution, resolution);
        }

        /**
         * @brief UI frame period assumed when pacing the progressive sequence.
         *
         * The application submits at most one display task per UI frame, so the frame period
         * bounds time-to-HQ from below by chunks * framePeriod. 16.7 ms is a 60 Hz UI.
         */
        constexpr double kDefaultUiFrameMs = 16.7;

        void writeProgressiveJson(std::vector<ProgressiveSample> const & samples,
                                  std::filesystem::path const & outputPath)
        {
            std::filesystem::create_directories(outputPath.parent_path());
            std::ofstream out{outputPath};
            out << "{\n  \"schemaVersion\": 2,\n  \"samples\": [\n";
            for (std::size_t i = 0; i < samples.size(); ++i)
            {
                auto const & s = samples[i];
                out << "    {\"model\": \"" << s.model << "\", \"resolution\": " << s.resolution
                    << ", \"repetitions\": " << s.repetitions
                    << ", \"chunkPixels\": " << s.chunkPixels
                    << ", \"chunks\": " << s.chunks
                    << ", \"paceMs\": " << s.paceMs
                    << ", \"timeToFirstPixelMs\": " << s.timeToFirstPixelMs
                    << ", \"timeToHqMs\": " << s.timeToHqMs
                    << ", \"p95TimeToHqMs\": " << s.p95TimeToHqMs
                    << ", \"medianChunkWallMs\": " << s.medianChunkWallMs
                    << ", \"medianChunkGpuMs\": " << s.medianChunkGpuMs
                    << ", \"medianChunkSubmitToWorkDoneMs\": " << s.medianChunkSubmitToWorkDoneMs
                    << ", \"medianChunkReadbackMs\": " << s.medianChunkReadbackMs
                    << ", \"medianChunkSubmitCallMs\": " << s.medianChunkSubmitCallMs
                    << ", \"medianChunkPumpMs\": " << s.medianChunkPumpMs
                    << ", \"medianChunkPaceMs\": " << s.medianChunkPaceMs
                    << ", \"medianChunkFixedOverheadMs\": " << s.medianChunkFixedOverheadMs
                    << ", \"gpuTimeShare\": " << s.gpuTimeShare << "}";
                if (i + 1u < samples.size())
                {
                    out << ",";
                }
                out << "\n";
            }
            out << "  ]\n}\n";
        }
    }

    /**
     * @brief Measures how long a settled static view takes to reach full quality.
     *
     * The headline metric is timeToHqMs: wall time from the start of the progressive
     * sequence until the last chunk completed the frame. The per-chunk split shows how much
     * of that is GPU work and how much is fixed per-submission overhead.
     */
    TEST(WebGPUProgressivePerf, TimeToFullQuality)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_BENCH") == nullptr)
        {
            GTEST_SKIP() << "Benchmarks disabled; set GLADIUS_RUN_WEBGPU_BENCH=1 to enable";
        }

        std::shared_ptr<webgpu::WebGPUComputeContext> context;
        try
        {
            context = std::make_shared<webgpu::WebGPUComputeContext>();
        }
        catch (std::exception const & exception)
        {
            GTEST_SKIP() << "WebGPU device unavailable: " << exception.what();
        }
        if (!context->isValid())
        {
            GTEST_SKIP() << "WebGPU device unavailable: " << context->getErrorMessage();
        }

        constexpr std::array configurations = {
          ProgressiveConfig{.model = "SimpleGyroid.3mf", .resolution = 256u, .repetitions = 3u},
          ProgressiveConfig{.model = "SimpleGyroid.3mf", .resolution = 512u, .repetitions = 3u},
          ProgressiveConfig{.model = "ImplicitGyroid.3mf", .resolution = 256u, .repetitions = 3u},
          ProgressiveConfig{.model = "ImplicitGyroid.3mf", .resolution = 512u, .repetitions = 3u},
          ProgressiveConfig{.model = "SphereInACage.3mf", .resolution = 256u, .repetitions = 3u},
          ProgressiveConfig{.model = "SphereInACage.3mf", .resolution = 512u, .repetitions = 3u},
          ProgressiveConfig{.model = "lampbase.3mf", .resolution = 256u, .repetitions = 3u},
          ProgressiveConfig{.model = "lampbase.3mf", .resolution = 512u, .repetitions = 3u},
          ProgressiveConfig{.model = "RadialRadiator.3mf", .resolution = 256u, .repetitions = 3u},
          ProgressiveConfig{.model = "RadialRadiator.3mf", .resolution = 512u, .repetitions = 3u},
          ProgressiveConfig{.model = "webcam_003.3mf", .resolution = 256u, .repetitions = 3u},
          ProgressiveConfig{.model = "webcam_003.3mf", .resolution = 512u, .repetitions = 3u}};

        std::vector<ProgressiveSample> samples;
        samples.reserve(configurations.size());

        auto const * modelFilter = std::getenv("GLADIUS_BENCH_MODEL");
        auto const * resolutionFilter = std::getenv("GLADIUS_BENCH_RESOLUTION");
        auto const * lqBench = std::getenv("GLADIUS_BENCH_LQ");
        auto const benchSettings = lqBench != nullptr ? bench::createLqSettings()
                                                      : bench::createHqSettings();

        std::string currentModel;
        std::unique_ptr<bench::BenchScene> scene;
        for (auto const & config : configurations)
        {
            if (modelFilter != nullptr && config.model.find(modelFilter) == std::string_view::npos)
            {
                continue;
            }
            if (resolutionFilter != nullptr
                && config.resolution != static_cast<std::uint32_t>(std::atoi(resolutionFilter)))
            {
                continue;
            }

            auto const filePath = std::filesystem::path("testdata") / config.model;
            if (!std::filesystem::exists(filePath))
            {
                GTEST_SKIP() << "Benchmark model missing: " << filePath;
            }

            if (config.model != currentModel)
            {
                scene.reset();
                try
                {
                    scene = bench::createScene(filePath, context);
                }
                catch (std::exception const & exception)
                {
                    GTEST_SKIP() << "Unable to prepare " << config.model << ": " << exception.what();
                }
                currentModel = std::string{config.model};
            }

            // GLADIUS_PROGRESSIVE_BENCH_CHUNK_PIXELS overrides the scheduler budget so chunk
            // sizing experiments can be A/B'ed without rebuilding.
            auto const * chunkOverride = std::getenv("GLADIUS_PROGRESSIVE_BENCH_CHUNK_PIXELS");
            auto const chunkPixels = chunkOverride != nullptr
                                       ? static_cast<std::size_t>(std::atoll(chunkOverride))
                                       : currentSchedulerChunkPixels(config.resolution);

            // The application starts at most one display submission per UI frame, so the frame
            // period is part of the time-to-HQ budget. GLADIUS_PROGRESSIVE_BENCH_PACE_MS=0
            // measures the theoretical best case with no UI pacing.
            auto const * paceOverride = std::getenv("GLADIUS_PROGRESSIVE_BENCH_PACE_MS");
            auto const paceMs = paceOverride != nullptr ? std::atof(paceOverride) : kDefaultUiFrameMs;

            ProgressiveSample sample{.model = std::string{config.model},
                                     .resolution = config.resolution,
                                     .repetitions = config.repetitions,
                                     .chunkPixels = chunkPixels,
                                     .paceMs = paceMs};

            std::vector<double> timeToHqMs;
            std::vector<double> timeToFirstPixelMs;
            std::vector<double> chunkWallMs;
            std::vector<double> chunkGpuMs;
            std::vector<double> chunkWorkDoneMs;
            std::vector<double> chunkReadbackMs;
            std::vector<double> chunkFixedMs;
            std::vector<double> chunkSubmitCallMs;
            std::vector<double> chunkPumpMs;
            std::vector<double> chunkPaceMs;
            for (std::uint32_t repetition = 0u; repetition < config.repetitions; ++repetition)
            {
                auto const run = scene->renderProgressive(config.resolution,
                                                          config.resolution,
                                                          benchSettings,
                                                          chunkPixels,
                                                          paceMs);
                auto const expectedRowsPerChunk =
                  std::max<std::size_t>(1u, chunkPixels / static_cast<std::size_t>(config.resolution));
                EXPECT_EQ(run.chunks,
                          (static_cast<std::size_t>(config.resolution) + expectedRowsPerChunk - 1u)
                            / expectedRowsPerChunk)
                  << config.model << " did not fill the frame in whole chunks";

                timeToHqMs.push_back(run.timeToHqMs);
                timeToFirstPixelMs.push_back(run.timeToFirstPixelMs);
                for (auto const & chunk : run.chunkStats)
                {
                    chunkWallMs.push_back(chunk.wallMs);
                    chunkFixedMs.push_back(chunk.fixedOverheadMs());
                    chunkSubmitCallMs.push_back(chunk.submitCallMs);
                    chunkPumpMs.push_back(chunk.pumpMs);
                    if (chunk.paceMs > 0.0)
                    {
                        chunkPaceMs.push_back(chunk.paceMs);
                    }
                    if (chunk.submitToWorkDoneMs > 0.0)
                    {
                        chunkWorkDoneMs.push_back(chunk.submitToWorkDoneMs);
                    }
                    if (chunk.readbackMs > 0.0)
                    {
                        chunkReadbackMs.push_back(chunk.readbackMs);
                    }
                    if (chunk.gpuPassMs > 0.0)
                    {
                        chunkGpuMs.push_back(chunk.gpuPassMs);
                    }
                }
                sample.chunks = run.chunks;
            }

            std::sort(timeToHqMs.begin(), timeToHqMs.end());
            sample.timeToHqMs = percentile(timeToHqMs, 0.5);
            sample.p95TimeToHqMs = percentile(timeToHqMs, 0.95);
            std::sort(timeToFirstPixelMs.begin(), timeToFirstPixelMs.end());
            sample.timeToFirstPixelMs = percentile(timeToFirstPixelMs, 0.5);

            std::sort(chunkWallMs.begin(), chunkWallMs.end());
            sample.medianChunkWallMs = percentile(chunkWallMs, 0.5);
            std::sort(chunkFixedMs.begin(), chunkFixedMs.end());
            sample.medianChunkFixedOverheadMs = percentile(chunkFixedMs, 0.5);
            std::sort(chunkSubmitCallMs.begin(), chunkSubmitCallMs.end());
            sample.medianChunkSubmitCallMs = percentile(chunkSubmitCallMs, 0.5);
            std::sort(chunkPumpMs.begin(), chunkPumpMs.end());
            sample.medianChunkPumpMs = percentile(chunkPumpMs, 0.5);
            std::sort(chunkPaceMs.begin(), chunkPaceMs.end());
            sample.medianChunkPaceMs = percentile(chunkPaceMs, 0.5);
            std::sort(chunkWorkDoneMs.begin(), chunkWorkDoneMs.end());
            sample.medianChunkSubmitToWorkDoneMs = percentile(chunkWorkDoneMs, 0.5);
            std::sort(chunkReadbackMs.begin(), chunkReadbackMs.end());
            sample.medianChunkReadbackMs = percentile(chunkReadbackMs, 0.5);
            std::sort(chunkGpuMs.begin(), chunkGpuMs.end());
            sample.medianChunkGpuMs = percentile(chunkGpuMs, 0.5);
            if (sample.medianChunkSubmitToWorkDoneMs > 0.0)
            {
                sample.gpuTimeShare = sample.medianChunkGpuMs / sample.medianChunkSubmitToWorkDoneMs;
            }

            // Sanity: a sequence that never delivered pixels proves nothing.
            EXPECT_GT(sample.timeToHqMs, 0.0) << config.model << " produced no progressive timing";
            EXPECT_GT(sample.timeToFirstPixelMs, 0.0);
            EXPECT_GE(sample.timeToHqMs, sample.timeToFirstPixelMs);
            // One display submission per UI frame: the pacing must be visible in the result.
            EXPECT_GE(sample.timeToHqMs, static_cast<double>(sample.chunks - 1u) * paceMs * 0.9)
              << config.model << " was not paced at " << paceMs << " ms per UI frame";
            samples.push_back(sample);
        }

        auto const * outputPath = std::getenv("GLADIUS_PROGRESSIVE_BENCH_OUTPUT");
        writeProgressiveJson(samples,
                             outputPath != nullptr
                               ? std::filesystem::path{outputPath}
                               : std::filesystem::path{"webgpu_bench"} / "progressive_baseline.json");
        EXPECT_FALSE(samples.empty());
    }
#endif
}
