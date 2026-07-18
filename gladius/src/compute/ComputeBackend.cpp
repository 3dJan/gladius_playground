#include "compute/ComputeBackend.h"

namespace gladius::compute
{
    std::string_view toString(ComputeBackendKind const backend) noexcept
    {
        switch (backend)
        {
        case ComputeBackendKind::OpenCL:
            return "opencl";
        case ComputeBackendKind::WebGPU:
            return "webgpu";
        }

        return "opencl";
    }

    std::optional<ComputeBackendKind> parseComputeBackend(std::string_view const value) noexcept
    {
        if (value == "opencl")
        {
            return ComputeBackendKind::OpenCL;
        }

        if (value == "webgpu")
        {
            return ComputeBackendKind::WebGPU;
        }

        return std::nullopt;
    }

    bool isComputeBackendBuilt(ComputeBackendKind const backend) noexcept
    {
        switch (backend)
        {
        case ComputeBackendKind::OpenCL:
#if defined(GLADIUS_ENABLE_OPENCL)
            return true;
#else
            return false;
#endif
        case ComputeBackendKind::WebGPU:
#if defined(GLADIUS_ENABLE_WEBGPU)
            return true;
#else
            return false;
#endif
        }

        return false;
    }
}