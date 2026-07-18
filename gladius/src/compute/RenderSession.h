#pragma once

#include "RenderPayloadSnapshot.h"
#include "RenderSceneGeneration.h"
#include "RenderSessionInputs.h"

namespace gladius
{
    /**
     * @brief Retains the scene resources and inputs used by one render session.
     *
     * A session owns no duplicate scene payloads. It retains the scene owner, copies the
     * per-session camera/settings values, and records the GPU handles that were current when the
     * session was created. The scene owner remains alive until the session is released.
     *
     * Scene publication is still mutable during this migration. Callers must obtain sessions only
     * after scene materialization and must not use a session as a substitute for a published
     * immutable generation yet. The scene owner is intentionally not exposed by this type.
     */
    class RenderSession
    {
      public:
        RenderSession(SharedRenderSceneGeneration generation, RenderSessionInputs inputs);

        [[nodiscard]] SharedComputeContext getComputeContext() const;
        [[nodiscard]] RenderSceneRevision getRevision() const noexcept;
        [[nodiscard]] RenderSessionInputs const & getInputs() const;
        [[nodiscard]] RenderPayloadSnapshot const & getPayloadSnapshot() const;

        [[nodiscard]] bool isPayloadCurrent() const;

      private:
        SharedRenderSceneGeneration m_generation;
        RenderSessionInputs m_inputs;
        RenderPayloadSnapshot m_payloadSnapshot;
    };

    using SharedRenderSession = std::shared_ptr<RenderSession>;
} // namespace gladius
