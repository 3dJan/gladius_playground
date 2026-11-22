/**
 * @file FunctionEvaluatorToolWebGPU.h
 * @brief WebGPU execution path of the evaluate_function MCP tool
 */

#pragma once

#include "FunctionEvaluationSupport.h"

#include <cstdint>
#include <nlohmann/json.hpp>
#include <vector>

namespace gladius
{
    class Document;

    namespace nodes
    {
        class Assembly;
    }

    namespace mcp::tools
    {
        /**
         * @brief Lowers and evaluates a function on the WebGPU compute backend.
         *
         * Mirrors the OpenCL evaluation path so that both backends produce identical
         * tool output: the graph is lowered and flattened to a standalone function
         * signature, compiled to WGSL, and dispatched over the sample records.
         * @param document Active document providing the compute context and resources.
         * @param assembly Assembly containing the function to evaluate.
         * @param functionId Resource ID of the function to evaluate.
         * @param isAssembly True when the assembly model itself is evaluated.
         * @param sampleCount Number of sample points in the flattened argument buffer.
         * @param outputInfos Output metadata used for serialization.
         * @param inputBuffer Flattened per-sample argument components.
         * @param totalInputComponents Argument components per sample.
         * @param totalOutputComponents Output components per sample.
         * @return Tool result JSON, or null when WebGPU cannot serve the request.
         */
        nlohmann::json evaluateFunctionWebGPU(Document & document,
                                              nodes::Assembly & assembly,
                                              uint32_t functionId,
                                              bool isAssembly,
                                              std::size_t sampleCount,
                                              std::vector<FunctionOutputInfo> const & outputInfos,
                                              std::vector<float> const & inputBuffer,
                                              int totalInputComponents,
                                              int totalOutputComponents);
    } // namespace mcp::tools
} // namespace gladius
