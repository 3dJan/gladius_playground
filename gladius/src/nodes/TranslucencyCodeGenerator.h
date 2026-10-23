#pragma once

#include <string>

namespace gladius::nodes
{
    class Assembly;

    /**
     * @brief Generated OpenCL translucency entry point plus whether the model is actually
     * translucent.
     */
    struct TranslucencyKernelSource
    {
        /// @brief Source defining `translucencyModel()`. Always valid so the static kernels can
        /// call it unconditionally - opaque models get a stub reporting zero translucency.
        std::string source;
        /// @brief True when the model really exposes a usable translucency channel.
        bool active{};
    };

    /**
     * @brief Stub `translucencyModel()` reporting zero translucency.
     *
     * The static render kernels always call the entry point, so every generated model source -
     * including the command-stream preview - has to define it.
     */
    [[nodiscard]] std::string createOpaqueTranslucencyKernelSource();

    /**
     * @brief Emit the OpenCL translucency entry point for an already flattened assembly.
     *
     * The render kernels call the generated `model()` entry point with a fixed float4 ABI, so the
     * scalar translucency channel cannot ride along in that signature. Instead a second codegen
     * pass over the same graph emits `float translucencyModel(float3 pos, PAYLOAD_ARGS)`.
     *
     * The result always defines the entry point: models without a translucency channel - or graphs
     * this backend cannot express - get a stub reporting zero translucency, which makes the
     * compositor degrade to the previous single-surface shading.
     *
     * @param assembly Assembly to generate from; only a flattened one (a single model) can be
     * inlined, anything else falls back to the stub.
     */
    [[nodiscard]] TranslucencyKernelSource createTranslucencyKernelSource(Assembly & assembly);
} // namespace gladius::nodes
