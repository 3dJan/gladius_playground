#include "compute/ComputeBackendSettings.h"

#include "ConfigManager.h"

namespace gladius::compute
{
    std::optional<ComputeBackendKind> getConfiguredComputeBackendPreference(
      ConfigManager const & configManager)
    {
        auto const configuredBackend =
          configManager.getValue<std::string>("compute", "backend", std::string{});
        return parseComputeBackend(configuredBackend);
    }

    ComputeBackendKind getConfiguredComputeBackend(ConfigManager const & configManager)
    {
        auto const configuredBackend =
          configManager.getValue<std::string>("compute", "backend", "opencl");
        auto const backend = parseComputeBackend(configuredBackend).value_or(ComputeBackendKind::OpenCL);

        if (isComputeBackendBuilt(backend))
        {
            return backend;
        }

        // A persisted preference can outlive the build configuration that created it.
        // Select the other compiled backend instead of reporting an unavailable one to
        // the viewport and backend factory.
        if (isComputeBackendBuilt(ComputeBackendKind::OpenCL))
        {
            return ComputeBackendKind::OpenCL;
        }

        if (isComputeBackendBuilt(ComputeBackendKind::WebGPU))
        {
            return ComputeBackendKind::WebGPU;
        }

        return backend;
    }

    void setConfiguredComputeBackend(ConfigManager & configManager, ComputeBackendKind const backend)
    {
        configManager.setValue("compute", "backend", std::string{toString(backend)});
    }
}