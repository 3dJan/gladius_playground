#pragma once

#include "compute/IBoundsService.h"
#include "compute/IComputeRenderer.h"

#include <memory>
#include <string>

namespace gladius::compute
{
    /**
     * @brief Owns one selected renderer and its currently usable materialized scene.
     *
     * A replacement scene is materialized before it replaces the current scene. Failed
     * materialization therefore leaves the last usable scene available for presentation while the
     * caller reports the failure. Submitted work must retain any backend resources it needs until
     * terminal completion; this owner only controls submission of future frames.
     */
    class RenderBackendSession
    {
      public:
        explicit RenderBackendSession(std::unique_ptr<IComputeRenderer> renderer,
                                      std::shared_ptr<IBoundsService> boundsService = {});

        [[nodiscard]] ComputeBackendKind getBackendKind() const noexcept;
        [[nodiscard]] RendererCapability getCapabilities() const noexcept;
        [[nodiscard]] bool isAvailable() const noexcept;
        [[nodiscard]] bool hasMaterializedScene() const noexcept;
        [[nodiscard]] std::uint64_t getSceneGeneration() const noexcept;
        [[nodiscard]] std::shared_ptr<const RenderSceneSnapshot>
        getSceneSnapshot() const noexcept;
        [[nodiscard]] IBoundsService * getBoundsService() noexcept;
        [[nodiscard]] std::string const & getErrorMessage() const noexcept;

        /// @brief Materialize and atomically replace the active scene on success.
        [[nodiscard]] bool replaceScene(RenderSceneSnapshot snapshot) noexcept;

        /// @brief Submit a frame against the active materialized scene.
        [[nodiscard]] std::unique_ptr<IRenderSubmission> submitFrame(RenderRequest request);

      private:
        std::unique_ptr<IComputeRenderer> m_renderer;
        std::unique_ptr<IRenderScene> m_scene;
        std::shared_ptr<const RenderSceneSnapshot> m_snapshot;
        std::shared_ptr<IBoundsService> m_boundsService;
        std::string m_errorMessage;
    };
}