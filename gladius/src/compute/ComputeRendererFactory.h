#pragma once

#include "compute/ComputeBackend.h"
#include "compute/IComputeRenderer.h"
#include "compute/RenderBackendSession.h"
#include "compute/RenderSceneSnapshot.h"
#include "compute/RenderSession.h"

#include <memory>
#include <string>

namespace gladius
{
    class ConfigManager;
}

namespace gladius::nodes
{
    class Assembly;
    class Model;
}

namespace gladius::compute
{
    using SharedRenderSession = std::shared_ptr<gladius::RenderSession>;

    /**
     * @brief Creates IComputeRenderer instances based on the requested backend kind.
     */
    class ComputeRendererFactory
    {
      public:
        /**
         * @brief Constructs a renderer for the given backend kind.
         * @param kind The compute backend to create.
         * @return A unique pointer to the newly created renderer.
         * @throws std::runtime_error if the backend is not built into this binary.
         */
        [[nodiscard]] static std::unique_ptr<IComputeRenderer> create(ComputeBackendKind kind);

        /**
         * @brief Constructs an OpenCL renderer backed by a retained render session.
         * @param session The OpenCL render session to use for rendering.
         * @return A unique pointer to the newly created OpenCL renderer.
         * @throws std::invalid_argument if session is null.
         */
        [[nodiscard]] static std::unique_ptr<IComputeRenderer> create(ComputeBackendKind kind,
                                                                      SharedRenderSession session);

        /**
         * @brief Creates a RenderBackendSession from configuration and model data.
         *
         * Reads compute.backend from config (defaults to OpenCL), validates the backend is built,
         * constructs the appropriate renderer, wraps it in a session, and optionally materializes
         * an initial scene snapshot if one is provided.
         *
         * @param configManager Application configuration manager.
         * @param preferredBackend Fallback backend kind when config is empty/unparseable.
         * @return A unique pointer to the initialized session, or nullptr on failure.
         */
        [[nodiscard]] static std::unique_ptr<RenderBackendSession> createRenderBackendSession(
            ConfigManager const & configManager,
            ComputeBackendKind preferredBackend);

        /**
         * @brief Creates a RenderBackendSession with an explicit OpenCL render session.
         *
         * Use this variant when you already have a SharedRenderSession from ComputeCore
         * and want to wrap it in a backend-neutral session for the scheduler.
         *
         * @param configManager Application configuration manager.
         * @param openclSession The OpenCL render session to use (required for OpenCL backend).
         * @param preferredBackend Fallback backend kind when config is empty/unparseable.
         * @return A unique pointer to the initialized session, or nullptr on failure.
         */
        [[nodiscard]] static std::unique_ptr<RenderBackendSession> createRenderBackendSession(
            ConfigManager const & configManager,
            SharedRenderSession openclSession,
            ComputeBackendKind preferredBackend);

        /**
         * @brief Materializes a scene snapshot for the given model/assembly.
         * @param assembly The assembly to materialize from (may be null).
         * @param model The model to materialize from (required if assembly is null).
         * @param generation Scene generation counter for freshness tracking.
         * @return A valid RenderSceneSnapshot, or an invalid one on failure.
         */
        [[nodiscard]] static RenderSceneSnapshot materializeScene(
            nodes::Assembly const * assembly,
            nodes::Model & model,
            std::uint64_t generation);
    };
}
