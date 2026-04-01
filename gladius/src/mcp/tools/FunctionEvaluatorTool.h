/**
 * @file FunctionEvaluatorTool.h
 * @brief Tool for evaluating functions at sample points via OpenCL
 */

#pragma once

#include "MCPToolBase.h"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace gladius
{
    class Application;

    namespace mcp::tools
    {
        /**
         * @brief Evaluates volumetric functions at arbitrary 3D sample points.
         *
         * Uses the existing OpenCL compute infrastructure (ToOCLVisitor / ComputeContext)
         * to compile and dispatch a 1D evaluation kernel over sample points.
         * Supports both float and vec3 output types.
         */
        class FunctionEvaluatorTool : public MCPToolBase
        {
          public:
            explicit FunctionEvaluatorTool(Application * app);
            ~FunctionEvaluatorTool() override = default;

            /// @brief Evaluate a function at the given sample points.
            /// @param functionId Model resource ID of the function to evaluate.
            /// @param samples JSON array of sample point objects.
            /// @return JSON with "success", "function_id", "results" (or "error").
            nlohmann::json evaluateFunction(uint32_t functionId,
                                            nlohmann::json const & samples);
        };
    } // namespace mcp::tools
} // namespace gladius
