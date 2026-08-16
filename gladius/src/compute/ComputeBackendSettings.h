#pragma once

#include "compute/ComputeBackend.h"

namespace gladius
{
    class ConfigManager;
}

namespace gladius::compute
{
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