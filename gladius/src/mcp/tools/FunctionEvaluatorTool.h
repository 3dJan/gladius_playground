/**
 * @file FunctionEvaluatorTool.h
 * @brief Tool for evaluating functions at sample points on the active compute backend
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
         * Compiles the function graph on the active compute backend (OpenCL or WebGPU)
         * and dispatches a 1D evaluation over the sample points.
         * Supports scalar, vec3 and matrix-valued outputs.
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
