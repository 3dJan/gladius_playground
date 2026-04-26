/// @file RenderingKernelSource_tests.cpp
/// @brief Source-level regressions for the OpenCL rendering kernel.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace gladius::tests
{
    namespace
    {
        std::string loadRenderingKernelSource()
        {
            std::filesystem::path path = std::filesystem::current_path();
            for (int depth = 0; depth < 8; ++depth)
            {
                auto const candidate = path / "src" / "kernel" / "rendering.cl";
                if (std::filesystem::exists(candidate))
                {
                    std::ifstream file(candidate);
                    std::stringstream buffer;
                    buffer << file.rdbuf();
                    return buffer.str();
                }
                auto const nestedCandidate = path / "gladius" / "src" / "kernel" / "rendering.cl";
                if (std::filesystem::exists(nestedCandidate))
                {
                    std::ifstream file(nestedCandidate);
                    std::stringstream buffer;
                    buffer << file.rdbuf();
                    return buffer.str();
                }
                if (!path.has_parent_path())
                {
                    break;
                }
                path = path.parent_path();
            }
            return {};
        }
    }

    TEST(RenderingKernelSource, DetermineColor_InPreviewOnly_DoesNotFetchModelColor)
    {
        auto const source = loadRenderingKernelSource();
        ASSERT_FALSE(source.empty());

        auto const previewGuardPos = source.find("bool const previewOnly");
        ASSERT_NE(previewGuardPos, std::string::npos);

        auto const modelColorPos = source.find("model(pos, PASS_PAYLOAD_ARGS).xyz", previewGuardPos);
        ASSERT_NE(modelColorPos, std::string::npos);

        auto const guardUsePos = source.rfind("!previewOnly", modelColorPos);
        ASSERT_NE(guardUsePos, std::string::npos);

        auto const determineColorPos = source.rfind("determineColor", guardUsePos);
        ASSERT_NE(determineColorPos, std::string::npos);
    }
} // namespace gladius::tests