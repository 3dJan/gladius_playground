#pragma once

#include <stdexcept>
#include <string>
#include <string_view>

namespace gladius::webgpu
{
    /// @brief Error for WebGPU entry points that must pump the host event loop to finish.
    ///
    /// emdawnwebgpu delivers WebGPU callbacks only from the JS event loop, so a synchronous
    /// caller in the browser build can never receive them and would spin at 100% CPU.
    /// Such entry points must use the async variant in the browser and guard here instead.
    inline std::runtime_error makeBrowserBlockingUnavailableError(std::string_view const feature)
    {
        return std::runtime_error("Blocking " + std::string{feature} +
                                  " is not available in the browser build");
    }
}
