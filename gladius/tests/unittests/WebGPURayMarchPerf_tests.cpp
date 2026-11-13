#include "WebGPURayMarchHarness.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
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
        /// @brief One benchmark configuration: model file, resolution, iteration count.
        struct BenchConfig
        {
            std::string_view model;
            std::uint32_t resolution;
            std::uint32_t iterations;
        };

        /// @brief Aggregated timing and counter statistics for one configuration.
        struct BenchSample
        {
            std::string model;
            std::uint32_t resolution{};
            std::uint32_t iterations{};
            /// End-to-end latency from request to delivered frame; what the user experiences.
            double medianWallMs{};
            double p5WallMs{};
            double p95WallMs{};
            double minWallMs{};
            double maxWallMs{};
            /// GPU queue time from submission until the queue signalled work completion.
            double medianWorkDoneMs{};
            double medianGpuMs{};
            std::uint64_t totalRays{};
            std::uint64_t totalSteps{};
            std::uint64_t totalHits{};
            std::uint64_t shadowTaps{};
            std::uint64_t aoTaps{};
            std::uint64_t refinementTaps{};
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

        void writeBenchmarkJson(std::vector<BenchSample> const & samples,
                                std::filesystem::path const & outputPath)
        {
            std::filesystem::create_directories(outputPath.parent_path());
            std::ofstream out{outputPath};
            out << "{\n  \"schemaVersion\": 1,\n  \"samples\": [\n";
            for (std::size_t i = 0; i < samples.size(); ++i)
            {
                auto const & s = samples[i];
                out << "    {\"model\": \"" << s.model << "\", \"resolution\": " << s.resolution
                    << ", \"iterations\": " << s.iterations
                    << ", \"medianWallMs\": " << s.medianWallMs
                    << ", \"p5WallMs\": " << s.p5WallMs << ", \"p95WallMs\": " << s.p95WallMs
                    << ", \"minWallMs\": " << s.minWallMs << ", \"maxWallMs\": " << s.maxWallMs
                    << ", \"medianWorkDoneMs\": " << s.medianWorkDoneMs
                    << ", \"medianGpuMs\": " << s.medianGpuMs
                    << ", \"totalRays\": " << s.totalRays << ", \"totalSteps\": " << s.totalSteps
                    << ", \"totalHits\": " << s.totalHits << ", \"shadowTaps\": " << s.shadowTaps
                    << ", \"aoTaps\": " << s.aoTaps << ", \"refinementTaps\": " << s.refinementTaps
                    << ", \"stepsPerRay\": "
                    << (s.totalRays != 0u ? static_cast<double>(s.totalSteps) / static_cast<double>(s.totalRays) : 0.0)
                    << ", \"hitsPerRay\": "
                    << (s.totalRays != 0u ? static_cast<double>(s.totalHits) / static_cast<double>(s.totalRays) : 0.0)
                    << "}";
                if (i + 1u < samples.size())
                {
                    out << ",";
                }
                out << "\n";
            }
            out << "  ]\n}\n";
        }
    }

    TEST(WebGPURayMarchPerf, RunBenchmarkCorpus)
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
          BenchConfig{.model = "SimpleGyroid.3mf", .resolution = 256u, .iterations = 9u},
          BenchConfig{.model = "SimpleGyroid.3mf", .resolution = 512u, .iterations = 7u},
          BenchConfig{.model = "SimpleGyroid.3mf", .resolution = 1024u, .iterations = 5u},
          BenchConfig{.model = "ImplicitGyroid.3mf", .resolution = 256u, .iterations = 9u},
          BenchConfig{.model = "ImplicitGyroid.3mf", .resolution = 512u, .iterations = 7u},
          BenchConfig{.model = "ImplicitGyroid.3mf", .resolution = 1024u, .iterations = 5u},
          BenchConfig{.model = "SphereInACage.3mf", .resolution = 256u, .iterations = 9u},
          BenchConfig{.model = "SphereInACage.3mf", .resolution = 512u, .iterations = 7u},
          BenchConfig{.model = "SphereInACage.3mf", .resolution = 1024u, .iterations = 5u},
          BenchConfig{.model = "lampbase.3mf", .resolution = 256u, .iterations = 9u},
          BenchConfig{.model = "lampbase.3mf", .resolution = 512u, .iterations = 7u},
          BenchConfig{.model = "lampbase.3mf", .resolution = 1024u, .iterations = 5u},
          BenchConfig{.model = "RadialRadiator.3mf", .resolution = 256u, .iterations = 9u},
          BenchConfig{.model = "RadialRadiator.3mf", .resolution = 512u, .iterations = 7u},
          BenchConfig{.model = "RadialRadiator.3mf", .resolution = 1024u, .iterations = 5u},
          BenchConfig{.model = "webcam_003.3mf", .resolution = 256u, .iterations = 9u},
          BenchConfig{.model = "webcam_003.3mf", .resolution = 512u, .iterations = 7u},
          BenchConfig{.model = "webcam_003.3mf", .resolution = 1024u, .iterations = 5u}};

        std::vector<BenchSample> samples;
        samples.reserve(configurations.size());

        // Optional filters to run a subset (e.g. for smoke tests).
        auto const * modelFilter = std::getenv("GLADIUS_BENCH_MODEL");
        auto const * resolutionFilter = std::getenv("GLADIUS_BENCH_RESOLUTION");

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

            BenchSample sample{.model = std::string{config.model},
                               .resolution = config.resolution,
                               .iterations = config.iterations};

            // Timing pass: counters stay off so they cannot perturb what they measure.
            std::vector<double> wallDurationsMs;
            std::vector<double> workDoneMs;
            std::vector<double> gpuDurationsMs;
            wallDurationsMs.reserve(config.iterations);
            workDoneMs.reserve(config.iterations);
            gpuDurationsMs.reserve(config.iterations);
            for (std::uint32_t iteration = 0u; iteration < config.iterations; ++iteration)
            {
                auto const start = std::chrono::steady_clock::now();
                auto frame = scene->renderFrame(config.resolution, config.resolution,
                                                bench::createHqSettings(), true, false);
                auto const elapsedMs = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - start)
                                         .count();
                ASSERT_EQ(frame.pixels.size(),
                          static_cast<std::size_t>(config.resolution) * config.resolution);

                auto const timing = scene->getLastTiming();
                wallDurationsMs.push_back(elapsedMs);
                if (timing.has_value() && timing->submitToWorkDoneMs > 0.0)
                {
                    workDoneMs.push_back(timing->submitToWorkDoneMs);
                }
                if (timing.has_value() && timing->gpuTimestampsAvailable && timing->gpuPassMs > 0.0)
                {
                    gpuDurationsMs.push_back(timing->gpuPassMs);
                }
            }

            std::sort(wallDurationsMs.begin(), wallDurationsMs.end());
            sample.minWallMs = wallDurationsMs.front();
            sample.maxWallMs = wallDurationsMs.back();
            sample.medianWallMs = percentile(wallDurationsMs, 0.5);
            sample.p5WallMs = percentile(wallDurationsMs, 0.05);
            sample.p95WallMs = percentile(wallDurationsMs, 0.95);
            if (!workDoneMs.empty())
            {
                std::sort(workDoneMs.begin(), workDoneMs.end());
                sample.medianWorkDoneMs = percentile(workDoneMs, 0.5);
            }
            if (!gpuDurationsMs.empty())
            {
                std::sort(gpuDurationsMs.begin(), gpuDurationsMs.end());
                sample.medianGpuMs = percentile(gpuDurationsMs, 0.5);
            }

            // Counter pass: one instrumented frame reports the ray-march work for this resolution.
            auto frame = scene->renderFrame(config.resolution, config.resolution,
                                            bench::createHqSettings(), false, true);
            ASSERT_EQ(frame.pixels.size(),
                      static_cast<std::size_t>(config.resolution) * config.resolution);
            auto const metrics = scene->getLastMetrics();
            ASSERT_TRUE(metrics.has_value()) << "metrics missing for " << config.model;
            EXPECT_EQ(metrics->totalRays,
                      static_cast<std::uint64_t>(config.resolution) * config.resolution);
            sample.totalRays = metrics->totalRays;
            sample.totalSteps = metrics->totalSteps;
            sample.totalHits = metrics->totalHits;
            sample.shadowTaps = metrics->shadowTaps;
            sample.aoTaps = metrics->aoTaps;
            sample.refinementTaps = metrics->refinementTaps;

            // Sanity: the frame must actually show the model, otherwise the timing is meaningless.
            EXPECT_GT(sample.totalHits, 0u) << config.model << " produced no hits";
            EXPECT_GT(sample.medianWallMs, 0.0);
            samples.push_back(sample);
        }

        auto const * outputPath = std::getenv("GLADIUS_BENCH_OUTPUT");
        writeBenchmarkJson(samples,
                           outputPath != nullptr ? std::filesystem::path{outputPath}
                                                 : std::filesystem::path{"webgpu_bench"} / "baseline.json");
        EXPECT_FALSE(samples.empty());
    }
#endif
}
