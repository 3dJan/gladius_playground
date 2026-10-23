/// @file TranslucencyKernelSource_tests.cpp
/// @brief Source-level regressions for the OpenCL translucency compositor.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace gladius::tests
{
    namespace
    {
        /**
         * @brief Read a kernel source file from the checked-in kernel directory.
         * @param fileName file name below src/kernel
         * @return file contents, empty when the file is missing
         */
        std::string loadKernelSource(char const * fileName)
        {
            std::filesystem::path const path =
              std::filesystem::path{GLADIUS_KERNEL_SOURCE_DIR} / fileName;
            std::ifstream file(path);
            if (!file)
            {
                return {};
            }

            std::stringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }

        /// @brief Read the WebGPU frame shader used by the analytic renderer.
        std::string loadWebGpuFrameShader()
        {
            std::filesystem::path const path =
              std::filesystem::path{GLADIUS_KERNEL_SOURCE_DIR}.parent_path() / "webgpu" /
              "shaders" / "frame_compute.wgsl";
            std::ifstream file(path);
            if (!file)
            {
                return {};
            }

            std::stringstream buffer;
            buffer << file.rdbuf();
            return buffer.str();
        }

        /**
         * @brief Extract the body of a kernel from a kernel source file.
         * @param source complete kernel source
         * @param kernelName name of the kernel whose body is wanted
         * @return text between the kernel declaration and the following one, empty when missing
         */
        std::string extractKernelBody(std::string const & source, std::string const & kernelName)
        {
            auto const declPos = source.find("void kernel " + kernelName + "(");
            if (declPos == std::string::npos)
            {
                return {};
            }

            auto const nextKernelPos = source.find("void kernel ", declPos + 1);
            auto const endPos =
              nextKernelPos == std::string::npos ? source.size() : nextKernelPos;
            return source.substr(declPos, endPos - declPos);
        }
    }

    TEST(TranslucencyKernelSource, Compositor_GatedByFlagAndPreviewApproximation)
    {
        auto const source = loadKernelSource("translucency.cl");
        ASSERT_FALSE(source.empty());

        EXPECT_NE(source.find("RF_ENABLE_TRANSLUCENCY"), std::string::npos);
        EXPECT_NE(source.find("AM_ONLY_PRECOMPSDF"), std::string::npos);
    }

    TEST(TranslucencyKernelSource, AttenuationCoefficient_ConvertsAlphaToBeerLambert)
    {
        auto const source = loadKernelSource("translucency.cl");
        ASSERT_FALSE(source.empty());

        EXPECT_NE(source.find("translucencyModel(pos, PASS_PAYLOAD_ARGS)"), std::string::npos);
        EXPECT_NE(source.find("-log(1.f - alpha) / referenceThickness"), std::string::npos);
    }

    TEST(TranslucencyKernelSource, InteriorTransmittance_IntegratesSigmaTrapezoidally)
    {
        auto const source = loadKernelSource("translucency.cl");
        ASSERT_FALSE(source.empty());

        EXPECT_NE(source.find("(sigma + nextSigma) * 0.5f * (nextDistance - distance)"),
                  std::string::npos);
        // The far crossing has to be refined, otherwise the last partial segment is over-counted.
        EXPECT_NE(source.find("modelInternal(eyePosition + middle * rayDirection"),
                  std::string::npos);
    }

    TEST(TranslucencyKernelSource, ShadeRayResult_KeepsNonModelHitsOpaque)
    {
        auto const source = loadKernelSource("translucency.cl");
        ASSERT_FALSE(source.empty());

        auto const guardPos = source.find("raycastingResult.type == 1.f");
        ASSERT_NE(guardPos, std::string::npos);
        EXPECT_NE(source.find("renderTranslucentRay", guardPos), std::string::npos);
    }

    TEST(TranslucencyKernelSource, Compositor_TreatsBuildPlateAsOpaqueTerminalSurface)
    {
        auto const source = loadKernelSource("translucency.cl");
        ASSERT_FALSE(source.empty());

        auto const helperSurfaceGuard = source.find("if (current.type != 1.f)");
        ASSERT_NE(helperSurfaceGuard, std::string::npos);
        auto const interiorIntegration = source.find("interiorTransmittance(", helperSurfaceGuard);
        ASSERT_NE(interiorIntegration, std::string::npos);
        EXPECT_LT(helperSurfaceGuard, interiorIntegration);
    }

    TEST(TranslucencyKernelSource, WebGpuCompositor_TreatsBuildPlateAsOpaqueTerminalSurface)
    {
        auto const source = loadWebGpuFrameShader();
        ASSERT_FALSE(source.empty());

        auto const helperSurfaceGuard = source.find("if (hit_sample.object_type != 1.0f)");
        ASSERT_NE(helperSurfaceGuard, std::string::npos);
        auto const interiorIntegration = source.find("interior_transmittance(", helperSurfaceGuard);
        ASSERT_NE(interiorIntegration, std::string::npos);
        EXPECT_LT(helperSurfaceGuard, interiorIntegration);
    }

    TEST(TranslucencyKernelSource, RenderingKernel_DoesNotReferenceTranslucencyEntryPoint)
    {
        auto const source = loadKernelSource("rendering.cl");
        ASSERT_FALSE(source.empty());

        // rendering.cl is linked into the slicer, contour and dual-contouring programs, whose
        // generated model kernel has no translucency entry point. A reference here breaks them.
        EXPECT_EQ(source.find("translucencyModel"), std::string::npos);
        EXPECT_EQ(source.find("shadeRayResult"), std::string::npos);
    }

    TEST(TranslucencyKernelSource, RendererKernels_UseShadeRayResultForHighQualityPaths)
    {
        auto const source = loadKernelSource("renderer.cl");
        ASSERT_FALSE(source.empty());

        // The high-quality kernels shade through the compositor, which is what makes
        // RF_ENABLE_TRANSLUCENCY reachable from a dispatch.
        for (auto const * kernelName :
             {"renderScene", "renderSceneWithDistanceInit", "renderSceneWithMetrics"})
        {
            auto const body = extractKernelBody(source, kernelName);
            ASSERT_FALSE(body.empty()) << kernelName;
            EXPECT_NE(body.find("shadeRayResult"), std::string::npos) << kernelName;
        }

        // The distance-output kernel feeds the precomputed-SDF preview and stays opaque.
        auto const previewBody = extractKernelBody(source, "renderSceneWithDistanceOutput");
        ASSERT_FALSE(previewBody.empty());
        EXPECT_NE(previewBody.find("determineColor"), std::string::npos);
        EXPECT_EQ(previewBody.find("shadeRayResult"), std::string::npos);
    }
} // namespace gladius::tests
