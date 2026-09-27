#pragma once

// Include complete C++ bindings for Lib3MF (types + wrapper + inline API)
#include <lib3mf_types.hpp>
#include <lib3mf_abi.hpp>
#include <lib3mf_implicit.hpp>

namespace gladius::io
{
    // Create the wrapper without mutating the process-wide current working directory.
    inline Lib3MF::PWrapper loadLib3mfScoped()
    {
        return Lib3MF::CWrapper::loadLibrary();
    }
} // namespace gladius::io
