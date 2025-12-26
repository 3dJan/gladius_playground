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
/// NanoVDB causes driver freezes on Rusticl, so these tests must be skipped.
/// This should be used in tests that use MeshResource, BeamLatticeResource, or VDB grids.
/// @param context A valid ComputeContext reference or pointer
#define SKIP_IF_NANOVDB_UNAVAILABLE_ON_RUSTICL(context)                                            \
    do                                                                                             \
    {                                                                                              \
        if ((context).getCapabilities().rusticl)                                                   \
        {                                                                                          \
            GTEST_SKIP() << "NanoVDB tests are disabled on Rusticl (causes driver freezes)";       \
        }                                                                                          \
    } while (0)

/// @brief Check if NanoVDB is available on the given compute context
/// Returns false if the runtime is Rusticl or lacks fp64 support
/// @param context A valid ComputeContext reference
inline bool isNanoVdbAvailable(gladius::ComputeContext const & context)
{
    auto const & caps = context.getCapabilities();
    return caps.fp64 && !caps.rusticl;
}
