#pragma once

#include "compute/ComputeBackend.h"

namespace gladius
{
    class ConfigManager;
}

namespace gladius::compute
{
    /**
     * @brief Gets the configured compute backend, defaulting to OpenCL for existing users.
     */
    [[nodiscard]] ComputeBackendKind getConfiguredComputeBackend(ConfigManager const & configManager);

    /**
     * @brief Persists the compute backend selected for future application launches.
     */
    void setConfiguredComputeBackend(ConfigManager & configManager, ComputeBackendKind backend);
}