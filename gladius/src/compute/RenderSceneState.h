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
     * The state is mutable while a document refresh materializes a new scene. Future render-session
     * APIs must expose it only after publication and must not mutate it during rendering.
     */
    class RenderSceneState
    {
      public:
        RenderSceneState(SharedComputeContext context,
                         RequiredCapabilities requiredCapabilities,
                         events::SharedLogger logger);

        [[nodiscard]] SharedComputeContext getComputeContext() const;

        /// @brief Rebuild all context-local scene resources for a replacement context.
        void setComputeContext(SharedComputeContext context);

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
