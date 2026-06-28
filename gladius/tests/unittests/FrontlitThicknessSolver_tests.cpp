/**
 * @file FrontlitThicknessSolver_tests.cpp
 * @brief Unit tests for FrontlitThicknessSolver and FaceThicknessMapper
 */

#include "io/3mf/FaceThicknessMapper.h"
#include "io/3mf/FilamentOpticalProperties.h"
#include "io/3mf/FrontlitThicknessSolver.h"
#include "io/3mf/ShellMaterialOrdering.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <cmath>
#include <iostream>
#include <filesystem>
#include <fstream>
#include <limits>
#include <string>
#include <vector>

namespace gladius::io::tests
{
    namespace
    {
        struct BenchmarkMetrics
        {
            float meanLinearRgbError = 0.0F;
            float maxLinearRgbError = 0.0F;
            float meanDeltaE = 0.0F;
            float maxDeltaE = 0.0F;
            float convergenceRate = 0.0F;
            std::size_t sampleCount = 0U;
        };

        struct BenchmarkOutcome
        {
            OrderedShellMaterials orderedMaterials;
            BenchmarkMetrics metrics;
        };

        [[nodiscard]] Eigen::Vector3f hslToSrgb(float hueDegrees, float saturation, float lightness)
        {
            float const hue = std::fmod(std::fmod(hueDegrees, 360.0F) + 360.0F, 360.0F) / 60.0F;
            float const chroma = (1.0F - std::abs(2.0F * lightness - 1.0F)) * saturation;
            float const x = chroma * (1.0F - std::abs(std::fmod(hue, 2.0F) - 1.0F));

            Eigen::Vector3f rgbPrime = Eigen::Vector3f::Zero();
            if (hue < 1.0F)
            {
                rgbPrime = Eigen::Vector3f{chroma, x, 0.0F};
            }
            else if (hue < 2.0F)
            {
                rgbPrime = Eigen::Vector3f{x, chroma, 0.0F};
            }
            else if (hue < 3.0F)
            {
                rgbPrime = Eigen::Vector3f{0.0F, chroma, x};
            }
            else if (hue < 4.0F)
            {
                rgbPrime = Eigen::Vector3f{0.0F, x, chroma};
            }
            else if (hue < 5.0F)
            {
                rgbPrime = Eigen::Vector3f{x, 0.0F, chroma};
            }
            else
            {
                rgbPrime = Eigen::Vector3f{chroma, 0.0F, x};
            }

            float const match = lightness - 0.5F * chroma;
            return (rgbPrime.array() + match).matrix().cwiseMax(0.0F).cwiseMin(1.0F);
        }

        [[nodiscard]] Eigen::Vector3f linearRgbToXyz(Eigen::Vector3f const& linearRgb)
        {
            Eigen::Matrix3f const matrix = (Eigen::Matrix3f() <<
                0.4124564F, 0.3575761F, 0.1804375F,
                0.2126729F, 0.7151522F, 0.0721750F,
                0.0193339F, 0.1191920F, 0.9503041F).finished();
            return matrix * linearRgb;
        }

        [[nodiscard]] Eigen::Vector3f xyzToLab(Eigen::Vector3f const& xyz)
        {
            Eigen::Vector3f scaled = xyz;
            scaled.x() /= 0.95047F;
            scaled.y() /= 1.00000F;
            scaled.z() /= 1.08883F;

            auto const f = [](float value) {
                float const delta = 6.0F / 29.0F;
                float const threshold = delta * delta * delta;
                if (value > threshold)
                {
                    return std::cbrt(value);
                }
                return value / (3.0F * delta * delta) + 4.0F / 29.0F;
            };

            float const fx = f(std::max(scaled.x(), 0.0F));
            float const fy = f(std::max(scaled.y(), 0.0F));
            float const fz = f(std::max(scaled.z(), 0.0F));

            return Eigen::Vector3f{116.0F * fy - 16.0F, 500.0F * (fx - fy), 200.0F * (fy - fz)};
        }

        [[nodiscard]] float deltaE76(Eigen::Vector3f const& lhsLinear, Eigen::Vector3f const& rhsLinear)
        {
            return (xyzToLab(linearRgbToXyz(lhsLinear)) - xyzToLab(linearRgbToXyz(rhsLinear))).norm();
        }

        [[nodiscard]] FilamentStack loadFilamentStackFromJson(std::string const& filePath,
                                                               std::size_t& backgroundIndex,
                                                               IlluminationMode& illuminationMode)
        {
            namespace fs = std::filesystem;

            std::vector<fs::path> const candidates = {
                fs::path{filePath},
                fs::path{"testdata"} / fs::path{filePath}.filename(),
                fs::path{"tests/unittests/testdata"} / fs::path{filePath}.filename(),
                fs::path{"out/build/linux-releaseWithDebug/tests/unittests/testdata"} / fs::path{filePath}.filename()};

            fs::path resolvedPath;
            for (fs::path const& candidate : candidates)
            {
                if (fs::exists(candidate))
                {
                    resolvedPath = candidate;
                    break;
                }
            }

            std::ifstream stream(resolvedPath.empty() ? filePath : resolvedPath.string());
            EXPECT_TRUE(stream.is_open()) << "Failed to open fixture: " << filePath;

            nlohmann::json json;
            stream >> json;

            backgroundIndex = json.value("backgroundIndex", std::numeric_limits<std::size_t>::max());
            std::string const mode = json.value("illuminationMode", std::string{"frontlit"});
            illuminationMode = (mode == "backlit") ? IlluminationMode::Backlit : IlluminationMode::Frontlit;

            FilamentStack stack;
            for (auto const& material : json.at("materials"))
            {
                auto const reflectance = material.at("reflectanceColor").get<std::vector<float>>();
                Eigen::Vector3f transmissionDistance = Eigen::Vector3f::Zero();
                if (material.contains("transmissionDistance"))
                {
                    if (material.at("transmissionDistance").is_array())
                    {
                        auto const td = material.at("transmissionDistance").get<std::vector<float>>();
                        if (td.size() == 3U)
                        {
                            transmissionDistance = Eigen::Vector3f{td[0], td[1], td[2]};
                        }
                    }
                    else
                    {
                        float const td = material.at("transmissionDistance").get<float>();
                        transmissionDistance = Eigen::Vector3f{td, td, td};
                    }
                }

                stack.push_back(FilamentOpticalProperties{
                    material.value("name", std::string{"Material"}),
                    Eigen::Vector3f{reflectance.at(0), reflectance.at(1), reflectance.at(2)},
                    0.6F,
                    0.4F,
                    transmissionDistance});
            }

            return stack;
        }

        [[nodiscard]] std::vector<Eigen::Vector3f> buildHslWheelSamples()
        {
            std::vector<Eigen::Vector3f> samples;
            std::vector<float> const saturations = {0.55F, 0.80F, 1.0F};
            std::vector<float> const lightnesses = {0.35F, 0.50F, 0.65F};

            for (float lightness : lightnesses)
            {
                for (float saturation : saturations)
                {
                    for (int hueIndex = 0; hueIndex < 24; ++hueIndex)
                    {
                        float const hue = 360.0F * static_cast<float>(hueIndex) / 24.0F;
                        samples.push_back(srgbToLinear(hslToSrgb(hue, saturation, lightness)));
                    }
                }
            }

            return samples;
        }

        [[nodiscard]] BenchmarkMetrics evaluateBenchmarkForOrderedStack(FilamentStack const& stack,
                                                                         ThicknessConstraints const& constraints,
                                                                         IlluminationMode mode,
                                                                         std::size_t backgroundIndex,
                                                                         std::vector<Eigen::Vector3f> const& samples)
        {
            FrontlitThicknessSolver solver(stack, constraints, mode, backgroundIndex);

            BenchmarkMetrics metrics;
            metrics.sampleCount = samples.size();

            std::size_t convergedCount = 0U;
            for (Eigen::Vector3f const& target : samples)
            {
                ThicknessSolution const solution = solver.solve(target);
                float const linearRgbError = (solution.achievedColor - target).norm();
                float const deltaE = deltaE76(solution.achievedColor, target);

                metrics.meanLinearRgbError += linearRgbError;
                metrics.maxLinearRgbError = std::max(metrics.maxLinearRgbError, linearRgbError);
                metrics.meanDeltaE += deltaE;
                metrics.maxDeltaE = std::max(metrics.maxDeltaE, deltaE);
                if (solution.converged)
                {
                    ++convergedCount;
                }
            }

            if (!samples.empty())
            {
                float const invCount = 1.0F / static_cast<float>(samples.size());
                metrics.meanLinearRgbError *= invCount;
                metrics.meanDeltaE *= invCount;
                metrics.convergenceRate = static_cast<float>(convergedCount) * invCount;
            }

            return metrics;
        }

        [[nodiscard]] BenchmarkOutcome benchmarkHeuristicOrder(FilamentStack const& stack,
                                                                ThicknessConstraints const& constraints,
                                                                IlluminationMode mode,
                                                                std::size_t backgroundIndex,
                                                                std::vector<Eigen::Vector3f> const& samples)
        {
            BenchmarkOutcome outcome;
            outcome.orderedMaterials = ShellMaterialOrdering::reorderForShells(stack, backgroundIndex, mode);
            outcome.metrics = evaluateBenchmarkForOrderedStack(
                outcome.orderedMaterials.stack,
                constraints,
                mode,
                outcome.orderedMaterials.backgroundIndex,
                samples);
            return outcome;
        }

        [[nodiscard]] BenchmarkOutcome benchmarkOptimizedOrder(FilamentStack const& stack,
                                                                ThicknessConstraints const& constraints,
                                                                IlluminationMode mode,
                                                                std::size_t backgroundIndex,
                                                                std::vector<Eigen::Vector3f> const& samples)
        {
            BenchmarkOutcome outcome;
            outcome.orderedMaterials = ShellMaterialOrdering::optimizeGlobalOrderForShells(
                stack,
                backgroundIndex,
                mode,
                [&](FilamentStack const& candidate, std::size_t candidateBackgroundIndex) {
                    return evaluateBenchmarkForOrderedStack(
                        candidate,
                        constraints,
                        mode,
                        candidateBackgroundIndex,
                        samples).meanDeltaE;
                });

            outcome.metrics = evaluateBenchmarkForOrderedStack(
                outcome.orderedMaterials.stack,
                constraints,
                mode,
                outcome.orderedMaterials.backgroundIndex,
                samples);
            return outcome;
        }

        void printBenchmarkOutcome(char const* label, BenchmarkOutcome const& outcome)
        {
            std::cout << label << ": order=";
            for (std::size_t index = 0; index < outcome.orderedMaterials.stack.size(); ++index)
            {
                if (index > 0U)
                {
                    std::cout << " -> ";
                }
                std::cout << outcome.orderedMaterials.stack[index].name;
            }
            std::cout << ", meanRGB=" << outcome.metrics.meanLinearRgbError
                      << ", maxRGB=" << outcome.metrics.maxLinearRgbError
                      << ", meanDeltaE=" << outcome.metrics.meanDeltaE
                      << ", maxDeltaE=" << outcome.metrics.maxDeltaE
                      << ", convergence=" << outcome.metrics.convergenceRate << std::endl;
        }
    } // namespace

    class FrontlitThicknessSolverTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            // Create a simple 3-filament stack: black (bottom), red, white (top)
            m_blackFilament = FilamentOpticalProperties{
                "Black",
                Eigen::Vector3f(0.05f, 0.05f, 0.05f),
                0.8f,
                0.4f};

            m_redFilament = FilamentOpticalProperties{
                "Red",
                Eigen::Vector3f(0.9f, 0.1f, 0.1f),
                0.6f,
                0.4f};

            m_whiteFilament = FilamentOpticalProperties{
                "White",
                Eigen::Vector3f(0.95f, 0.95f, 0.95f),
                0.7f,
                0.4f};

            m_stack.push_back(m_blackFilament);
            m_stack.push_back(m_redFilament);
            m_stack.push_back(m_whiteFilament);
        }

        FilamentOpticalProperties m_blackFilament;
        FilamentOpticalProperties m_redFilament;
        FilamentOpticalProperties m_whiteFilament;
        FilamentStack m_stack;
    };

    TEST_F(FrontlitThicknessSolverTest, EffectiveOpacity_ZeroThickness_ReturnsZero)
    {
        float const opacity = m_blackFilament.computeEffectiveOpacity(0.0f);
        EXPECT_FLOAT_EQ(opacity, 0.0f);
    }

    TEST_F(FrontlitThicknessSolverTest, EffectiveOpacity_ReferenceThickness_ReturnsNominalOpacity)
    {
        float const opacity = m_blackFilament.computeEffectiveOpacity(m_blackFilament.referenceThickness);
        EXPECT_GT(opacity, 0.5f);
        EXPECT_LT(opacity, 1.0f);
    }

    TEST_F(FrontlitThicknessSolverTest, EffectiveOpacity_LargeThickness_ApproachesOne)
    {
        float const opacity = m_blackFilament.computeEffectiveOpacity(100.0f);
        EXPECT_GT(opacity, 0.99f);
    }

    TEST_F(FrontlitThicknessSolverTest, EffectiveOpacity_NegativeThickness_ReturnsZero)
    {
        float const opacity = m_blackFilament.computeEffectiveOpacity(-1.0f);
        EXPECT_FLOAT_EQ(opacity, 0.0f);
    }

    TEST(FrontlitKubelkaMunkTest, ComputeKubelkaMunkRT_UsesTransmissionDistance)
    {
        FilamentOpticalProperties filament{
            "GrayKM",
            Eigen::Vector3f(0.5f, 0.5f, 0.5f),
            0.6f,
            0.4f,
            Eigen::Vector3f(1.0f, 1.0f, 1.0f)};

        auto const rt = filament.computeKubelkaMunkRT(1.0f);

        EXPECT_NEAR(rt.reflectance.x(), 0.5f, 0.02f);
        EXPECT_NEAR(rt.reflectance.y(), 0.5f, 0.02f);
        EXPECT_NEAR(rt.reflectance.z(), 0.5f, 0.02f);
        EXPECT_NEAR(rt.transmittance.x(), 0.04f, 0.02f);
        EXPECT_NEAR(rt.transmittance.y(), 0.04f, 0.02f);
        EXPECT_NEAR(rt.transmittance.z(), 0.04f, 0.02f);
    }

    TEST(FrontlitKubelkaMunkTest, PredictColor_SingleLayerMatchesKMReflectance)
    {
        FilamentOpticalProperties filament{
            "CyanKM",
            Eigen::Vector3f(0.1f, 0.7f, 0.8f),
            0.5f,
            0.4f,
            Eigen::Vector3f(1.2f, 1.2f, 1.2f)};

        FilamentStack stack;
        stack.push_back(filament);

        FrontlitThicknessSolver solver{stack};

        std::vector<float> thicknesses = {1.0f};
        Eigen::Vector3f const predicted = solver.predictColor(thicknesses);

        auto const rt = filament.computeKubelkaMunkRT(1.0f);

        EXPECT_NEAR(predicted.x(), rt.reflectance.x(), 1e-3f);
        EXPECT_NEAR(predicted.y(), rt.reflectance.y(), 1e-3f);
        EXPECT_NEAR(predicted.z(), rt.reflectance.z(), 1e-3f);
    }

    TEST_F(FrontlitThicknessSolverTest, PredictColor_AllZeroThickness_ReturnsBlackOrDefault)
    {
        FrontlitThicknessSolver solver(m_stack);
        std::vector<float> thicknesses = {0.0f, 0.0f, 0.0f};
        Eigen::Vector3f const predicted = solver.predictColor(thicknesses);
        EXPECT_LT(predicted.norm(), 0.1f);
    }

    TEST_F(FrontlitThicknessSolverTest, PredictColor_OnlyTopLayerThick_ReturnsTopColor)
    {
        FrontlitThicknessSolver solver(m_stack);
        std::vector<float> thicknesses = {0.0f, 0.0f, 5.0f};
        Eigen::Vector3f const predicted = solver.predictColor(thicknesses);
        EXPECT_GT(predicted.x(), 0.8f);
        EXPECT_GT(predicted.y(), 0.8f);
        EXPECT_GT(predicted.z(), 0.8f);
    }

    TEST_F(FrontlitThicknessSolverTest, PredictColor_OnlyBottomLayerThick_ReturnsBottomColor)
    {
        FrontlitThicknessSolver solver(m_stack);
        std::vector<float> thicknesses = {5.0f, 0.0f, 0.0f};
        Eigen::Vector3f const predicted = solver.predictColor(thicknesses);
        EXPECT_LT(predicted.x(), 0.2f);
        EXPECT_LT(predicted.y(), 0.2f);
        EXPECT_LT(predicted.z(), 0.2f);
    }

    TEST_F(FrontlitThicknessSolverTest, PredictColor_OnlyMiddleLayerThick_ReturnsMiddleColor)
    {
        FrontlitThicknessSolver solver(m_stack);
        std::vector<float> thicknesses = {0.0f, 5.0f, 0.0f};
        Eigen::Vector3f const predicted = solver.predictColor(thicknesses);
        EXPECT_GT(predicted.x(), 0.7f);
        EXPECT_LT(predicted.y(), 0.3f);
        EXPECT_LT(predicted.z(), 0.3f);
    }

    TEST_F(FrontlitThicknessSolverTest, PredictColor_TopOccludesLower_ColorDominatedByTop)
    {
        FrontlitThicknessSolver solver(m_stack);
        std::vector<float> thicknesses = {5.0f, 5.0f, 5.0f};
        Eigen::Vector3f const predicted = solver.predictColor(thicknesses);
        EXPECT_GT(predicted.x(), 0.7f);
        EXPECT_GT(predicted.y(), 0.7f);
        EXPECT_GT(predicted.z(), 0.7f);
    }

    TEST_F(FrontlitThicknessSolverTest, Visibilities_SumToApproximatelyOne)
    {
        FrontlitThicknessSolver solver(m_stack);
        std::vector<float> thicknesses = {1.0f, 1.0f, 1.0f};
        std::vector<float> const visibilities = solver.computeVisibilities(thicknesses);

        ASSERT_EQ(visibilities.size(), 3u);
        float const sum = visibilities[0] + visibilities[1] + visibilities[2];
        EXPECT_LE(sum, 1.0f);
        EXPECT_GT(sum, 0.5f);
    }

    TEST_F(FrontlitThicknessSolverTest, Visibilities_TopLayerMostVisible)
    {
        FrontlitThicknessSolver solver(m_stack);
        std::vector<float> thicknesses = {1.0f, 1.0f, 1.0f};
        std::vector<float> const visibilities = solver.computeVisibilities(thicknesses);
        EXPECT_GT(visibilities[2], visibilities[1]);
        EXPECT_GT(visibilities[1], visibilities[0]);
    }

    TEST_F(FrontlitThicknessSolverTest, Solve_TargetWhite_IncreaseTopLayerThickness)
    {
        ThicknessConstraints constraints;
        constraints.minThickness = 0.0f;
        constraints.maxThickness = 5.0f;

        FrontlitThicknessSolver solver(m_stack, constraints);
        Eigen::Vector3f const targetWhite(0.9f, 0.9f, 0.9f);
        ThicknessSolution const solution = solver.solve(targetWhite);

        ASSERT_EQ(solution.thicknesses.size(), 3u);
        EXPECT_GT(solution.thicknesses[2], solution.thicknesses[0]);
        EXPECT_GT(solution.thicknesses[2], solution.thicknesses[1]);
        EXPECT_LT(solution.colorError, 0.2f);
    }

    TEST_F(FrontlitThicknessSolverTest, Solve_TargetRed_IncreaseRedLayerThickness)
    {
        ThicknessConstraints constraints;
        constraints.minThickness = 0.0f;
        constraints.maxThickness = 5.0f;

        FrontlitThicknessSolver solver(m_stack, constraints);
        Eigen::Vector3f const targetRed(0.8f, 0.1f, 0.1f);
        ThicknessSolution const solution = solver.solve(targetRed);

        ASSERT_EQ(solution.thicknesses.size(), 3u);
        EXPECT_LT(solution.thicknesses[2], 1.0f);
        EXPECT_LT(solution.colorError, 0.3f);
    }

    TEST_F(FrontlitThicknessSolverTest, Solve_TargetBlack_MinimalTopLayers)
    {
        ThicknessConstraints constraints;
        constraints.minThickness = 0.0f;
        constraints.maxThickness = 5.0f;

        FrontlitThicknessSolver solver(m_stack, constraints);
        Eigen::Vector3f const targetBlack(0.1f, 0.1f, 0.1f);
        ThicknessSolution const solution = solver.solve(targetBlack);

        ASSERT_EQ(solution.thicknesses.size(), 3u);
        EXPECT_LT(solution.achievedColor.norm(), 0.5f);
    }

    TEST_F(FrontlitThicknessSolverTest, Solve_RespectsConstraints)
    {
        ThicknessConstraints constraints;
        constraints.minThickness = 0.1f;
        constraints.maxThickness = 2.0f;
        constraints.layerHeight = 0.04f;

        FrontlitThicknessSolver solver(m_stack, constraints);
        Eigen::Vector3f const targetGray(0.5f, 0.5f, 0.5f);
        ThicknessSolution const solution = solver.solve(targetGray);

        for (float t : solution.thicknesses)
        {
            EXPECT_GE(t, constraints.minThickness - 1e-6f);
            EXPECT_LE(t, constraints.maxThickness + 1e-6f);

            float const remainder = std::fmod(t, constraints.layerHeight);
            bool const isQuantized = remainder < 1e-5f || (constraints.layerHeight - remainder) < 1e-5f;
            EXPECT_TRUE(isQuantized) << "Thickness " << t << " not quantized to layer height "
                                     << constraints.layerHeight;
        }
    }

    class FaceThicknessMapperTest : public FrontlitThicknessSolverTest
    {
    };

    TEST_F(FaceThicknessMapperTest, MapColors_EmptyInput_ReturnsEmptyResult)
    {
        FaceThicknessMapper mapper(m_stack);
        std::vector<Eigen::Vector3f> const emptyColors;
        FaceThicknessResult const result = mapper.mapColors(emptyColors);
        EXPECT_EQ(result.numFaces(), 0u);
        EXPECT_EQ(result.numLayers(), 3u);
        EXPECT_FLOAT_EQ(result.convergenceRate, 1.0f);
    }

    TEST_F(FaceThicknessMapperTest, MapColors_SingleFace_ReturnsValidResult)
    {
        FaceThicknessMapper mapper(m_stack);
        std::vector<Eigen::Vector3f> colors = {Eigen::Vector3f(0.5f, 0.5f, 0.5f)};
        FaceThicknessResult const result = mapper.mapColors(colors);
        EXPECT_EQ(result.numFaces(), 1u);
        EXPECT_EQ(result.numLayers(), 3u);
        for (auto const& layer : result.layerThicknesses)
        {
            EXPECT_EQ(layer.size(), 1u);
        }
    }

    TEST_F(FaceThicknessMapperTest, MapColors_MultipleFaces_ReturnsCorrectDimensions)
    {
        FaceThicknessMapper mapper(m_stack);

        std::vector<Eigen::Vector3f> colors = {
            Eigen::Vector3f(0.9f, 0.9f, 0.9f),
            Eigen::Vector3f(0.8f, 0.1f, 0.1f),
            Eigen::Vector3f(0.1f, 0.1f, 0.1f),
            Eigen::Vector3f(0.5f, 0.5f, 0.5f)};

        FaceThicknessResult const result = mapper.mapColors(colors);

        EXPECT_EQ(result.numFaces(), 4u);
        EXPECT_EQ(result.numLayers(), 3u);
        EXPECT_EQ(result.achievedColors.size(), 4u);
        EXPECT_EQ(result.colorErrors.size(), 4u);
        for (auto const& layer : result.layerThicknesses)
        {
            EXPECT_EQ(layer.size(), 4u);
        }
    }

    TEST_F(FaceThicknessMapperTest, MapColorsWithSmoothing_NoAdjacency_SameAsNoSmoothing)
    {
        FaceThicknessMapper mapper(m_stack);

        std::vector<Eigen::Vector3f> colors = {
            Eigen::Vector3f(0.9f, 0.9f, 0.9f),
            Eigen::Vector3f(0.1f, 0.1f, 0.1f)};

        std::vector<std::vector<std::size_t>> const emptyAdjacency;
        FaceThicknessResult const resultNoSmooth = mapper.mapColors(colors);
        FaceThicknessResult const resultSmooth = mapper.mapColorsWithSmoothing(colors, emptyAdjacency, 3, 0.3f);

        EXPECT_EQ(resultNoSmooth.numFaces(), resultSmooth.numFaces());
        EXPECT_EQ(resultNoSmooth.numLayers(), resultSmooth.numLayers());
    }

    TEST_F(FaceThicknessMapperTest, MapColorsWithSmoothing_WithAdjacency_SmoothsThicknesses)
    {
        FaceThicknessMapper mapper(m_stack);

        std::vector<Eigen::Vector3f> colors = {
            Eigen::Vector3f(0.9f, 0.9f, 0.9f),
            Eigen::Vector3f(0.1f, 0.1f, 0.1f),
            Eigen::Vector3f(0.9f, 0.9f, 0.9f)};

        std::vector<std::vector<std::size_t>> adjacency = {
            {1},
            {0, 2},
            {1}};

        FaceThicknessResult const resultNoSmooth = mapper.mapColors(colors);
        FaceThicknessResult const resultSmooth = mapper.mapColorsWithSmoothing(colors, adjacency, 5, 0.5f);

        for (std::size_t layer = 0; layer < resultSmooth.numLayers(); ++layer)
        {
            float const diffNoSmooth =
                std::abs(resultNoSmooth.layerThicknesses[layer][1] -
                         (resultNoSmooth.layerThicknesses[layer][0] + resultNoSmooth.layerThicknesses[layer][2]) / 2.0f);

            float const diffSmooth =
                std::abs(resultSmooth.layerThicknesses[layer][1] -
                         (resultSmooth.layerThicknesses[layer][0] + resultSmooth.layerThicknesses[layer][2]) / 2.0f);

            EXPECT_LE(diffSmooth, diffNoSmooth + 0.5f);
        }
    }

    TEST(ColorConversionTest, SrgbToLinear_Zero_ReturnsZero)
    {
        EXPECT_FLOAT_EQ(srgbToLinear(0.0f), 0.0f);
    }

    TEST(ColorConversionTest, SrgbToLinear_One_ReturnsOne)
    {
        EXPECT_FLOAT_EQ(srgbToLinear(1.0f), 1.0f);
    }

    TEST(ColorConversionTest, LinearToSrgb_Zero_ReturnsZero)
    {
        EXPECT_FLOAT_EQ(linearToSrgb(0.0f), 0.0f);
    }

    TEST(ColorConversionTest, LinearToSrgb_One_ReturnsOne)
    {
        EXPECT_FLOAT_EQ(linearToSrgb(1.0f), 1.0f);
    }

    TEST(ColorConversionTest, RoundTrip_PreservesValue)
    {
        for (float v : {0.0f, 0.1f, 0.5f, 0.8f, 1.0f})
        {
            float const roundTrip = srgbToLinear(linearToSrgb(v));
            EXPECT_NEAR(roundTrip, v, 1e-5f) << "Round trip failed for value " << v;
        }
    }

    TEST(ColorConversionTest, LinearToSrgb_MidGray_IsLighter)
    {
        float const srgb = linearToSrgb(0.5f);
        EXPECT_GT(srgb, 0.5f);
        EXPECT_LT(srgb, 1.0f);
    }

    TEST(ColorConversionTest, SrgbToLinear_MidGray_IsDarker)
    {
        float const linear = srgbToLinear(0.5f);
        EXPECT_LT(linear, 0.5f);
        EXPECT_GT(linear, 0.0f);
    }

    TEST(FrontlitThicknessSolverBenchmark, DISABLED_HslWheelBenchmark_PetgFixtureProducesFiniteMetrics)
    {
        std::size_t backgroundIndex = std::numeric_limits<std::size_t>::max();
        IlluminationMode mode = IlluminationMode::Frontlit;
        FilamentStack const stack = loadFilamentStackFromJson("testdata/petg_cymk.json", backgroundIndex, mode);

        ASSERT_EQ(stack.size(), 4U);
        EXPECT_EQ(mode, IlluminationMode::Frontlit);
        EXPECT_EQ(backgroundIndex, 3U) << "Fixture is expected to be the current CMYW-style white-backed stack.";

        ThicknessConstraints constraints;
        constraints.minThickness = 0.0F;
        constraints.maxThickness = 4.0F;
        constraints.layerHeight = 0.04F;
        constraints.totalMaxThickness = 6.0F;

        std::vector<Eigen::Vector3f> const samples = buildHslWheelSamples();
        ASSERT_FALSE(samples.empty());

        BenchmarkOutcome const heuristic = benchmarkHeuristicOrder(stack, constraints, mode, backgroundIndex, samples);
        printBenchmarkOutcome("heuristic", heuristic);

        EXPECT_EQ(heuristic.metrics.sampleCount, samples.size());
        EXPECT_TRUE(std::isfinite(heuristic.metrics.meanLinearRgbError));
        EXPECT_TRUE(std::isfinite(heuristic.metrics.maxLinearRgbError));
        EXPECT_TRUE(std::isfinite(heuristic.metrics.meanDeltaE));
        EXPECT_TRUE(std::isfinite(heuristic.metrics.maxDeltaE));
        EXPECT_GE(heuristic.metrics.meanLinearRgbError, 0.0F);
        EXPECT_GE(heuristic.metrics.meanDeltaE, 0.0F);
        EXPECT_GE(heuristic.metrics.convergenceRate, 0.0F);
        EXPECT_LE(heuristic.metrics.convergenceRate, 1.0F);
    }

    TEST(FrontlitThicknessSolverBenchmark, DISABLED_HslWheelBenchmark_OptimizedGlobalOrderDoesNotUnderperformHeuristic)
    {
        std::size_t backgroundIndex = std::numeric_limits<std::size_t>::max();
        IlluminationMode mode = IlluminationMode::Frontlit;
        FilamentStack const stack = loadFilamentStackFromJson("testdata/petg_cymk.json", backgroundIndex, mode);

        ThicknessConstraints constraints;
        constraints.minThickness = 0.0F;
        constraints.maxThickness = 4.0F;
        constraints.layerHeight = 0.04F;
        constraints.totalMaxThickness = 6.0F;

        std::vector<Eigen::Vector3f> const samples = buildHslWheelSamples();

        BenchmarkOutcome const heuristic = benchmarkHeuristicOrder(stack, constraints, mode, backgroundIndex, samples);
        BenchmarkOutcome const optimized = benchmarkOptimizedOrder(stack, constraints, mode, backgroundIndex, samples);
        printBenchmarkOutcome("heuristic", heuristic);
        printBenchmarkOutcome("optimized", optimized);

        EXPECT_LE(optimized.metrics.meanDeltaE, heuristic.metrics.meanDeltaE + 1e-4F);
        EXPECT_LE(optimized.metrics.meanLinearRgbError, heuristic.metrics.meanLinearRgbError + 1e-4F);
    }

} // namespace gladius::io::tests

