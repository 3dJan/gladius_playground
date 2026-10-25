#pragma once

#include "ProgramManager.h"

namespace gladius
{
    /**
     * @brief Owns the context-bound resources used to evaluate one renderable scene.
     *
     * This object is deliberately separate from ComputeCore's camera, presentation images, and
     * interactive state. It is the first ownership boundary for retained scene generations:
     * resource payload buffers, primitive buffers, and compiled OpenCL programs stay alive as one
     * unit while render sessions use them.
     *
     * The state is materialized once and is never rebound to another compute context. During this
     * migration, the payload buffers and program manager remain mutable for normal model updates;
     * retained render sessions must therefore treat this owner as a lifetime boundary, not yet as
     * a fully immutable generation.
     */
    class RenderSceneState
    {
      public:
        RenderSceneState(SharedComputeContext context,
                         RequiredCapabilities requiredCapabilities,
                         events::SharedLogger logger);

        [[nodiscard]] SharedComputeContext getComputeContext() const;

        [[nodiscard]] SharedResources & getResourceContext();
        [[nodiscard]] SharedResources getResourceContext() const;

        [[nodiscard]] SharedPrimitives & getPrimitives();
        [[nodiscard]] SharedPrimitives getPrimitives() const;

        [[nodiscard]] ProgramManager & getProgramManager();
        [[nodiscard]] ProgramManager const & getProgramManager() const;

      private:
        SharedComputeContext m_computeContext;
        SharedResources m_resources;
        SharedPrimitives m_primitives;
        ProgramManager m_programs;
    };

    using SharedRenderSceneState = std::shared_ptr<RenderSceneState>;
} // namespace gladius
