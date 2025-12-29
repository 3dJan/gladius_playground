#pragma once

#include <ComputeContext.h>
#include <gtest/gtest.h>

/// @brief Macro to skip tests that require OpenCL if it's not available
/// This should be used at the beginning of any test that creates a ComputeContext or ComputeCore
#define SKIP_IF_OPENCL_UNAVAILABLE()                                                               \
    do                                                                                             \
    {                                                                                              \
        if (!gladius::ComputeContext::isOpenCLAvailable())                                         \
        {                                                                                          \
            GTEST_SKIP() << "OpenCL is not available on this system";                              \
        }                                                                                          \
    } while (0)

/// @brief Macro to skip tests that use NanoVDB when Rusticl is the OpenCL runtime
/// NOTE: This macro is now deprecated as rusticl is supported. It remains for compatibility
/// but no longer skips tests.
/// @param context A valid ComputeContext reference or pointer
#define SKIP_IF_NANOVDB_UNAVAILABLE_ON_RUSTICL(context)                                            \
    do                                                                                             \
    {                                                                                              \
        /* NanoVDB is now enabled for rusticl - no longer skipping */                              \
        (void)(context); /* suppress unused warning */                                             \
    } while (0)

/// @brief Check if NanoVDB is available on the given compute context
/// NanoVDB is now enabled for all OpenCL runtimes including rusticl
/// @param context A valid ComputeContext reference
inline bool isNanoVdbAvailable(gladius::ComputeContext const & context)
{
    // NanoVDB is now universally available
    (void)context; // suppress unused parameter warning
    return true;
}
