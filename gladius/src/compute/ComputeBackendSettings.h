#pragma once

#include "compute/ComputeBackend.h"

namespace gladius
{
    class ConfigManager;
}

namespace gladius::compute
{
    /**
     * @brief Gets a valid backend explicitly persisted in the configuration.
     *
     * Unlike getConfiguredComputeBackend(), this function does not apply the legacy default or
     * fallback policy. It is used when the application must distinguish an explicit request from
     * an unset preference before initializing a backend.
     */
    [[nodiscard]] std::optional<ComputeBackendKind> getConfiguredComputeBackendPreference(
      ConfigManager const & configManager);

    /**
     * @brief Gets the configured and available compute backend, defaulting to OpenCL for existing users.
     *
     * Falls back to another backend compiled into the current binary when a persisted preference
     * refers to a backend that is not available in this build.
     */
    [[nodiscard]] ComputeBackendKind getConfiguredComputeBackend(ConfigManager const & configManager);

    /**
     * @brief Persists the compute backend selected for future application launches.
     */
    void setConfiguredComputeBackend(ConfigManager & configManager, ComputeBackendKind backend);
}