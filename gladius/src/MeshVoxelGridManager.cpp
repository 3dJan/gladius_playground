/// @file MeshVoxelGridManager.cpp
/// @brief Implementation of MeshVoxelGridManager
/// @see MeshVoxelGridManager.h

#include "MeshVoxelGridManager.h"

#include "CLProgram.h"
#include "compute/GpuKernelAccessGuard.h"

namespace gladius
{
    MeshVoxelGridManager::MeshVoxelGridManager(SharedComputeContext context)
        : m_context(std::move(context))
    {
    }

    bool MeshVoxelGridManager::buildGrid(CLProgram & program,
                                         PrimitiveDataBuffer & primitiveData,
                                         MeshVoxelGridBuildParams const & params)
    {
        if (params.voxelCount <= 0)
        {
            return false; // No voxels to build
        }

        // Run the build kernel
        // Global work size = total voxels
        cl::NDRange const globalRange(static_cast<size_t>(params.voxelCount));

        auto const & queue = m_context->GetQueue();
        GpuKernelAccessGuard gpuAccess(
          *m_context,
          queue,
          "buildMeshVoxelGrid",
          {{primitiveData.gpuResourceHandle(), GpuAccessMode::ReadWrite}});
        if (!gpuAccess.granted())
        {
            return false;
        }

        cl::Event event = program.runNonBlockingWithWaitList(queue,
                                                             "buildMeshVoxelGrid",
                                                             cl::NullRange,
                                                             globalRange,
                                                             gpuAccess.waitEvents(),
                                                             primitiveData.getBuffer(),
                                                             params.headerStart,
                                                             params.voxelDataOffset,
                                                             params.nodesOffset,
                                                             params.trianglesOffset,
                                                             params.normalsOffset,
                                                             params.indicesOffset,
                                                             params.edgeNeighborsOffset,
                                                             params.nodeCount,
                                                             params.triCount,
                                                             params.vertexNormalCount);
        gpuAccess.complete(event);

        // Check if the event is valid (non-null cl_event indicates success)
        return event() != nullptr;
    }

    void MeshVoxelGridManager::queueBuild(MeshVoxelGridBuildParams params)
    {
        m_buildQueue.push_back(params);
    }

    void MeshVoxelGridManager::executeQueuedBuilds(CLProgram & program,
                                                   PrimitiveDataBuffer & primitiveData)
    {
        for (auto const & params : m_buildQueue)
        {
            buildGrid(program, primitiveData, params);
        }

        // Wait for all builds to complete
        if (!m_buildQueue.empty())
        {
            CL_ERROR(m_context->GetQueue().finish());
        }

        m_buildQueue.clear();
    }

    void MeshVoxelGridManager::clearQueue()
    {
        m_buildQueue.clear();
    }

}  // namespace gladius
