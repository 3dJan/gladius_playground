#include "compute/ComputeBackendSettings.h"

#include "ConfigManager.h"

namespace gladius::compute
{
    ComputeBackendKind getConfiguredComputeBackend(ConfigManager const & configManager)
    {
        auto const configuredBackend =
          configManager.getValue<std::string>("compute", "backend", "opencl");
        auto const backend = parseComputeBackend(configuredBackend).value_or(ComputeBackendKind::WebGPU);
        // LOG: Backend selection - using <backend> (config=<value>, available=<bool>)
        return backend;
    }

    void setConfiguredComputeBackend(ConfigManager & configManager, ComputeBackendKind const backend)
    {
        configManager.setValue("compute", "backend", std::string{toString(backend)});
    }
}