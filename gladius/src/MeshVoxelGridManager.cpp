/// @file MeshVoxelGridManager.cpp
/// @brief Implementation of MeshVoxelGridManager
/// @see MeshVoxelGridManager.h

#include "MeshVoxelGridManager.h"

#include "CLProgram.h"

namespace gladius
{
    MeshVoxelGridManager::MeshVoxelGridManager(SharedComputeContext context)
        : m_context(std::move(context))
    {
    }

    bool MeshVoxelGridManager::buildGrid(CLProgram& program,
                                          cl::Buffer& primitiveDataBuffer,
                                          MeshVoxelGridBuildParams const& params)
    {
        if (params.voxelCount <= 0)
        {
            return false;  // No voxels to build
        }
        
        // Run the build kernel
        // Global work size = total voxels
        cl::NDRange const globalRange(static_cast<size_t>(params.voxelCount));
        
        cl::Event event = program.runNonBlocking(m_context->GetQueue(),
                                                  "buildMeshVoxelGrid",
                                                  cl::NullRange,
                                                  globalRange,
                                                  primitiveDataBuffer,
                                                  params.headerStart,
                                                  params.voxelDataOffset,
                                                  params.nodesOffset,
                                                  params.trianglesOffset,
                                                  params.normalsOffset,
                                                  params.indicesOffset,
                                                  params.nodeCount,
                                                  params.triCount,
                                                  params.vertexNormalCount);
        
        // Check if the event is valid (non-null cl_event indicates success)
        return event() != nullptr;
    }

    void MeshVoxelGridManager::queueBuild(MeshVoxelGridBuildParams params)
    {
        m_buildQueue.push_back(params);
    }

    void MeshVoxelGridManager::executeQueuedBuilds(CLProgram& program, cl::Buffer& primitiveDataBuffer)
    {
        for (auto const& params : m_buildQueue)
        {
            buildGrid(program, primitiveDataBuffer, params);
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
