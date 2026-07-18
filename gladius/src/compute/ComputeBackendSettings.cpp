#include "compute/ComputeBackendSettings.h"

#include "ConfigManager.h"

namespace gladius::compute
{
    ComputeBackendKind getConfiguredComputeBackend(ConfigManager const & configManager)
    {
        auto const configuredBackend =
          configManager.getValue<std::string>("compute", "backend", "opencl");
        return parseComputeBackend(configuredBackend).value_or(ComputeBackendKind::OpenCL);
    }

    void setConfiguredComputeBackend(ConfigManager & configManager, ComputeBackendKind const backend)
    {
        configManager.setValue("compute", "backend", std::string{toString(backend)});
    }
}