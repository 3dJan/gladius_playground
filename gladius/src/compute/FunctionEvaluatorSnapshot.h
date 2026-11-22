#pragma once

#include "compute/RenderContracts.h"
#include "compute/RenderSceneSnapshot.h"

#include <cstddef>
#include <string>
#include <vector>

namespace gladius::compute
{
    /// @brief One scalar, vector or matrix value in the flat function evaluation record.
    struct FunctionValueLayout
    {
        std::string name;
        std::size_t components{};
    };

    /// @brief Layout of the flat argument and result records of an evaluated graph function.
    /// The order matches the order of the emitted WGSL record adapter.
    struct FunctionEvaluatorDescriptor
    {
        std::string entryPointName;
        std::vector<FunctionValueLayout> arguments;
        std::vector<FunctionValueLayout> outputs;
        std::size_t argumentComponents{};
        std::size_t outputComponents{};
    };

    /**
     * @brief Backend-neutral description of a compiled graph function evaluation.
     *
     * Unlike a render scene snapshot this is not bound to the `evaluateModel(position)` contract:
     * the WGSL exposes the function's own signature plus a `gladiusCallRecord` adapter that reads
     * one flat argument record and writes one flat result record per sample.
     */
    struct FunctionEvaluatorSnapshot
    {
        /// WGSL for the graph function and its flat record adapter.
        std::string evaluatorWgsl;
        /// WGSL declarations the evaluation driver shader needs (record widths).
        std::string abiDeclarations;
        FunctionEvaluatorDescriptor descriptor;
        std::vector<float> parameterValues;
        std::vector<MeshResourcePayload> meshResources;
        std::vector<MeshResourcePayload> beamLatticeResources;
        std::vector<ImageResourcePayload> imageResources;
        RendererCapability requiredCapabilities{RendererCapability::None};

        [[nodiscard]] bool isValid() const noexcept
        {
            return !evaluatorWgsl.empty() && descriptor.outputComponents > 0u;
        }
    };
}
