#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace gladius
{
    class ResourceManager;
}

namespace gladius::nodes
{
    class Model;
}

namespace gladius::webgpu
{
    class WebGPUComputeContext;

    /**
     * @brief Outcome of a WebGPU program (WGSL) compilation.
     *
     * @param ok true when Dawn accepted the shader module without validation errors.
     * @param message Dawn's validation message; empty on success.
     * @param shaderSource the composed WGSL that was submitted, for diagnostics.
     */
    struct ProgramValidationResult
    {
        bool ok{};
        std::string message;
        std::string shaderSource;
    };

    /**
     * @brief Compiles a Gladius model evaluator for the WebGPU backend.
     *
     * The OpenCL backend exposes compilation failures through SlicerProgram; the WebGPU
     * backend compiles WGSL only as a side effect of building a render pipeline. This class
     * lowers a model with ToWgslVisitor and compiles the result inside a Dawn validation
     * error scope, which surfaces shader errors without touching the render pipeline cache.
     */
    class WebGPUProgramValidator final
    {
      public:
        explicit WebGPUProgramValidator(std::shared_ptr<WebGPUComputeContext> context);

        [[nodiscard]] bool isAvailable() const noexcept;
        [[nodiscard]] std::string getErrorMessage() const;

        /// @brief Lower the model to WGSL and compile it.
        /// @param model the flattened model to lower.
        /// @param resourceManager resources referenced by mesh/beam/image nodes.
        /// @param sceneGeneration generation stamp used for the scene snapshot.
        ProgramValidationResult validateModel(nodes::Model & model,
                                              ResourceManager const & resourceManager,
                                              std::uint64_t sceneGeneration);

        /// @brief Compile an already composed WGSL program.
        ProgramValidationResult validateShaderSource(std::string const & shaderSource);

      private:
        std::shared_ptr<WebGPUComputeContext> m_context;
    };
}
