#pragma once

#include "Primitives.h"
#include "ProgramManager.h"
#include "ResourceContext.h"

#include <compare>
#include <cstdint>

namespace gladius
{
    /**
     * @brief Identifies the document contents materialized into a render generation.
     *
     * A zero identity denotes an untagged transitional generation. Such a generation must not
     * be paired with a revision-sensitive operation such as native-save thumbnail generation.
     */
    struct RenderSceneRevision
    {
        std::uint64_t documentIdentity{0};
        std::uint64_t documentVersion{0};

        [[nodiscard]] bool isSpecified() const noexcept
        { return documentIdentity != 0; }

        auto operator<=>(RenderSceneRevision const &) const = default;
    };

    /**
     * @brief One retained renderable scene generation.
     *
     * The generation owns every context-bound input consumed by render kernels: primitive payload,
     * render resources, and the programs compiled for that payload. A render session retains this
     * object instead of observing ComputeCore's mutable working state.
     *
     * During the migration, ComputeCore still materializes the generation from its working scene.
     * Once published, this owner must not be rebound to another context or reused for a different
     * model payload.
     */
    class RenderSceneGeneration
    {
      public:
        RenderSceneGeneration(SharedComputeContext context,
                              RequiredCapabilities requiredCapabilities,
                              events::SharedLogger logger,
                              RenderSceneRevision revision = {});

        [[nodiscard]] SharedComputeContext getComputeContext() const;
        [[nodiscard]] SharedResources getResources() const;
        [[nodiscard]] SharedPrimitives getPrimitives() const;
        [[nodiscard]] RenderSceneRevision getRevision() const noexcept;
        [[nodiscard]] ProgramManager & getProgramManager();
        [[nodiscard]] ProgramManager const & getProgramManager() const;

      private:
        SharedComputeContext m_computeContext;
        SharedResources m_resources;
        SharedPrimitives m_primitives;
        RenderSceneRevision m_revision;
        ProgramManager m_programs;
    };

    using SharedRenderSceneGeneration = std::shared_ptr<RenderSceneGeneration>;
} // namespace gladius
