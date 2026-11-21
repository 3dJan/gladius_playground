#if defined(GLADIUS_ENABLE_MCP)

#include "mcp/tools/FunctionEvaluatorTool.h"
#include "mcp/tools/LibraryTool.h"
#include "mcp/tools/ValidationTool.h"

#include <gtest/gtest.h>

namespace gladius::tests
{
    TEST(MCPLibraryTool, ListLibrary_WithoutOpenCL_DoesNotReturnPureWebGPUStubError)
    {
        gladius::mcp::tools::LibraryTool tool(nullptr);
        auto const result = tool.listLibrary("", "");

        // listLibrary operates purely on the filesystem/lib3mf and must not be stubbed out.
        auto const dumped = result.dump();
        EXPECT_EQ(dumped.find("unavailable in a pure WebGPU MCP build"), std::string::npos)
          << dumped;
        EXPECT_TRUE(result.value("success", false)) << dumped;
        EXPECT_TRUE(result.contains("categories"));
    }

    TEST(MCPValidationTool, ValidateModel_WithoutApplication_DoesNotReturnPureWebGPUStubError)
    {
        gladius::mcp::tools::ValidationTool tool(nullptr);
        auto const result = tool.validateModel(nlohmann::json::object());

        auto const dumped = result.dump();
        EXPECT_EQ(dumped.find("unavailable in a pure WebGPU MCP build"), std::string::npos)
          << dumped;
        EXPECT_FALSE(result.value("success", true));
    }

    TEST(MCPFunctionEvaluatorTool, EvaluateFunction_WithoutApplication_DoesNotReturnPureWebGPUStubError)
    {
        gladius::mcp::tools::FunctionEvaluatorTool tool(nullptr);
        auto const result = tool.evaluateFunction(1u, nlohmann::json::array());

        auto const dumped = result.dump();
        EXPECT_EQ(dumped.find("unavailable in a pure WebGPU MCP build"), std::string::npos)
          << dumped;
        EXPECT_FALSE(result.value("success", true));
    }
} // namespace gladius::tests

#endif
