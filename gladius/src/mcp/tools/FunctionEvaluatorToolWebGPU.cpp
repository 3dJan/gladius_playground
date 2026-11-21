/**
 * @file FunctionEvaluatorToolWebGPU.cpp
 * @brief WebGPU execution path of the evaluate_function MCP tool
 */

#include "FunctionEvaluatorToolWebGPU.h"

#include "../../Document.h"
#include "../../compute/AnalyticRenderSceneSnapshotFactory.h"
#include "../../compute/ApplicationComputeRuntime.h"
#include "../../compute/FunctionEvaluatorSnapshot.h"
#include "../../compute/SdfEvaluation.h"
#include "../../nodes/Assembly.h"
#include "../../webgpu/WebGPUComputeContext.h"
#include "../../webgpu/WebGPUSdfEvaluator.h"
#include "../../webgpu/WebGPUSdfShaderComposer.h"
#include "MCPToolBase.h"

#include <fmt/format.h>

#include <algorithm>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace gladius::mcp::tools
{
    namespace
    {
        /// @brief Turn a lowered function snapshot into an executable WebGPU request.
        [[nodiscard]] compute::FunctionEvaluationRequest
        makeFunctionEvaluationRequest(compute::FunctionEvaluatorSnapshot const & snapshot)
        {
            compute::FunctionEvaluationRequest request;
            request.shaderSource = snapshot.evaluatorWgsl;
            request.argumentComponents = snapshot.descriptor.argumentComponents;
            request.outputComponents = snapshot.descriptor.outputComponents;
            request.parameterValues = snapshot.parameterValues;

            request.meshPayloadTable.reserve(snapshot.meshResources.size());
            for (auto const & payload : snapshot.meshResources)
            {
                request.meshPayloadTable.push_back(payload.data);
            }
            request.beamPayloadTable.reserve(snapshot.beamLatticeResources.size());
            for (auto const & payload : snapshot.beamLatticeResources)
            {
                request.beamPayloadTable.push_back(payload.data);
            }
            request.imagePayloadTable.reserve(snapshot.imageResources.size());
            for (auto const & payload : snapshot.imageResources)
            {
                request.imagePayloadTable.push_back(payload.data);
            }
            return request;
        }
    } // anonymous namespace

    nlohmann::json evaluateFunctionWebGPU(Document & document,
                                          nodes::Assembly & assembly,
                                          uint32_t functionId,
                                          bool isAssembly,
                                          std::size_t sampleCount,
                                          std::vector<FunctionOutputInfo> const & outputInfos,
                                          std::vector<float> const & inputBuffer,
                                          int totalInputComponents,
                                          int totalOutputComponents)
    {
        auto runtime = document.getBackendRuntime();
        auto context = runtime ? runtime->getWebGPUContext() : nullptr;
        if (!context || !context->isValid())
        {
            // Let the caller report the unsupported backend, keeping the message uniform.
            return {};
        }

        auto snapshot = compute::FunctionEvaluatorSnapshot{};
        try
        {
            auto const sceneGeneration = std::max(document.structuralEditEpoch(), 1ULL);
            compute::AnalyticRenderSceneSnapshotFactory snapshotFactory;
            snapshot = snapshotFactory.createFunctionEvaluator(
              assembly, functionId, sceneGeneration, &document.getResourceManager());
        }
        catch (std::exception const & e)
        {
            return MCPToolBase::createToolError(fmt::format("Function compilation failed: {}", e.what()));
        }

        if (isAssembly && snapshot.descriptor.arguments.size() != 1)
        {
            return MCPToolBase::createToolError(
              "Assembly evaluation requires the assembly model to expose a single 'pos' argument.");
        }

        if (static_cast<int>(snapshot.descriptor.argumentComponents) != totalInputComponents)
        {
            return MCPToolBase::createToolError(
              fmt::format("Lowered function signature mismatch: the compiled function takes {} "
                          "argument components, but the function declares {}.",
                          snapshot.descriptor.argumentComponents,
                          totalInputComponents));
        }

        if (static_cast<int>(snapshot.descriptor.outputComponents) != totalOutputComponents)
        {
            return MCPToolBase::createToolError(
              fmt::format("Lowered function signature mismatch: the compiled function returns {} "
                          "output components, but the function declares {}.",
                          snapshot.descriptor.outputComponents,
                          totalOutputComponents));
        }

        auto request = makeFunctionEvaluationRequest(snapshot);
        request.shaderSource = webgpu::WebGPUSdfShaderComposer::composeFunctionWithResourceSupport(
          snapshot.evaluatorWgsl,
          snapshot.abiDeclarations,
          !snapshot.meshResources.empty(),
          !snapshot.beamLatticeResources.empty(),
          !snapshot.imageResources.empty());
        request.arguments = inputBuffer;

        webgpu::WebGPUSdfEvaluator const evaluator(context);
        compute::FunctionEvaluationResult result;
        try
        {
            result = evaluator.evaluateFunction(std::move(request));
        }
        catch (std::exception const & e)
        {
            return MCPToolBase::createToolError(fmt::format("Evaluation failed: {}", e.what()));
        }

        return formatFunctionResults(result.values, outputInfos, totalOutputComponents,
                                     sampleCount, functionId);
    }
} // namespace gladius::mcp::tools
