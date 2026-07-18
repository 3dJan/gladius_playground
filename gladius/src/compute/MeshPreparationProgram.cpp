#include "compute/MeshPreparationProgram.h"

#include "Profiling.h"
#include "compute/GpuKernelAccessGuard.h"

#include <algorithm>
#include <utility>

namespace gladius
{
    namespace
    {
        FileNames meshPreparationSourceFiles()
        {
            return {"mesh_sdf.cl"};
        }
    }

    MeshPreparationProgram::MeshPreparationProgram(SharedComputeContext context,
                                                   SharedResources resources)
        : m_computeContext(std::move(context))
        , m_resources(std::move(resources))
        , m_program(m_computeContext)
    {
        m_program.setDebugLabel("MeshPreparationProgram");
    }

    void MeshPreparationProgram::setLogger(events::SharedLogger logger)
    {
        m_program.setLogger(std::move(logger));
    }

    void MeshPreparationProgram::setDebugLabel(std::string label)
    {
        m_program.setDebugLabel(std::move(label));
    }

    void MeshPreparationProgram::setCacheDirectory(std::filesystem::path const & path)
    {
        m_program.setCacheDirectory(path);
    }

    void MeshPreparationProgram::ensureCompiled()
    {
        ProfileFunction;
        std::lock_guard<std::mutex> lock(m_compileMutex);

        if (m_program.isCompilationInProgress())
        {
            m_program.finishCompilation();
        }

        if (m_program.isValid())
        {
            return;
        }

        BuildCallBack noCallback;
        m_program.buildCompleteProgram(meshPreparationSourceFiles(), noCallback);
    }

    bool MeshPreparationProgram::isValid() const
    {
        return m_program.isValid();
    }

    bool MeshPreparationProgram::isCompilationInProgress() const
    {
        return m_program.isCompilationInProgress();
    }

    void MeshPreparationProgram::waitForCompilation()
    {
        std::lock_guard<std::mutex> lock(m_compileMutex);
        m_program.finishCompilation();
    }

    void MeshPreparationProgram::requestShutdown()
    {
        m_program.requestShutdown();
    }

    bool MeshPreparationProgram::buildMeshVoxelGrid(Primitives & primitives,
                                                    MeshVoxelGridBuildParams const & params)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        ProfileFunction;

        if (params.voxelCount <= 0)
        {
            return false;
        }

        ensureCompiled();
        if (!m_program.isValid())
        {
            return false;
        }

        cl::NDRange const origin = {0, 0, 0};
        cl::NDRange const globalRange = {static_cast<size_t>(params.voxelCount), 1, 1};

                cl::CommandQueue const & queue = m_computeContext->GetQueue();
                GpuKernelAccessGuard gpuAccess(
                    *m_computeContext,
                    queue,
                    "buildMeshVoxelGrid",
                    {{primitives.data.gpuResourceHandle(), GpuAccessMode::ReadWrite}});
                if (!gpuAccess.granted())
                {
                        return false;
                }

                cl::Event const event = m_program.runNonBlockingWithWaitList(
                    queue,
                    "buildMeshVoxelGrid",
                    origin,
                    globalRange,
                    gpuAccess.waitEvents(),
                    primitives.data.getBuffer(),
                    static_cast<cl_int>(params.headerStart),
                    static_cast<cl_int>(params.voxelDataOffset),
                    static_cast<cl_int>(params.nodesOffset),
                    static_cast<cl_int>(params.trianglesOffset),
                    static_cast<cl_int>(params.normalsOffset),
                    static_cast<cl_int>(params.indicesOffset),
                    static_cast<cl_int>(params.edgeNeighborsOffset),
                    static_cast<cl_int>(params.nodeCount),
                    static_cast<cl_int>(params.triCount),
                    static_cast<cl_int>(params.vertexNormalCount));
                gpuAccess.complete(event);
                if (event())
                {
                        event.wait();
                        queue.finish();
                }

        return true;
    }

    bool MeshPreparationProgram::buildMeshFwnAggregates(
      Primitives & primitives, MeshFwnAggregateBuildParams const & params)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        ProfileFunction;
        GLADIUS_FWN_PREP_SCOPE("MeshPreparationProgram::buildMeshFwnAggregates queue kernel");

        if (params.nodeCount <= 0 || params.triCount <= 0 || params.fwnAggregatesOffset <= 0)
        {
            return false;
        }

        ensureCompiled();
        if (!m_program.isValid())
        {
            GLADIUS_FWN_PREP_LOG(
              "MeshPreparationProgram::buildMeshFwnAggregates skipped: program invalid");
            return false;
        }

        cl::CommandQueue const & queue = m_computeContext->GetQueue();
        cl::NDRange const origin = {0, 0, 0};
        cl::NDRange const globalRange = {static_cast<size_t>(params.nodeCount), 1, 1};

            GpuKernelAccessGuard gpuAccess(
              *m_computeContext,
              queue,
              "buildMeshFwnAggregates",
              {{primitives.data.gpuResourceHandle(), GpuAccessMode::ReadWrite}});
            if (!gpuAccess.granted())
            {
                return false;
            }

        cl::Event const event =
              m_program.runNonBlockingWithWaitList(queue,
                                   "buildMeshFwnAggregates",
                                   origin,
                                   globalRange,
                                   gpuAccess.waitEvents(),
                                   primitives.data.getBuffer(),
                                   static_cast<cl_int>(params.nodesOffset),
                                   static_cast<cl_int>(params.trianglesOffset),
                                   static_cast<cl_int>(params.fwnAggregatesOffset),
                                   static_cast<cl_int>(params.nodeCount),
                                   static_cast<cl_int>(params.triCount));
            gpuAccess.complete(event);
        return event() != nullptr;
    }

    bool MeshPreparationProgram::buildMeshSignCache(Primitives & primitives,
                                                    MeshSignCacheBuildParams const & params)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        ProfileFunction;
        GLADIUS_FWN_PREP_SCOPE("MeshPreparationProgram::buildMeshSignCache queue kernels");

        if (params.wordCount <= 0 || params.wordsToBuild <= 0 || params.resolution <= 0)
        {
            return false;
        }

        ensureCompiled();
        if (!m_program.isValid())
        {
            return false;
        }

        int constexpr wordsPerBatch = 64;
        cl::CommandQueue const & queue = m_computeContext->GetQueue();
        cl::NDRange const origin = {0, 0, 0};

        GpuKernelAccessGuard gpuAccess(*m_computeContext,
                                       queue,
                                       "buildMeshSignCache",
                                       {{primitives.data.gpuResourceHandle(), GpuAccessMode::ReadWrite}});
        if (!gpuAccess.granted())
        {
            return false;
        }

        int const firstWord = std::clamp(params.baseWord, 0, params.wordCount);
        int const lastWord = std::min(params.wordCount, firstWord + params.wordsToBuild);
        cl::Event lastEvent{};
        for (int baseWord = firstWord; baseWord < lastWord; baseWord += wordsPerBatch)
        {
            int const batchWordCount = std::min(wordsPerBatch, lastWord - baseWord);
            cl::NDRange const globalRange = {static_cast<size_t>(batchWordCount), 1, 1};

                        lastEvent = m_program.runNonBlockingWithWaitList(
                            queue,
                            "buildMeshSignCache",
                            origin,
                            globalRange,
                            gpuAccess.waitEvents(),
                            primitives.data.getBuffer(),
                            static_cast<cl_int>(params.headerStart),
                            static_cast<cl_int>(params.signCacheDataOffset),
                            static_cast<cl_int>(params.nodesOffset),
                            static_cast<cl_int>(params.trianglesOffset),
                            static_cast<cl_int>(params.normalsOffset),
                            static_cast<cl_int>(params.indicesOffset),
                            static_cast<cl_int>(params.edgeNeighborsOffset),
                            static_cast<cl_int>(params.fwnAggregatesOffset),
                            static_cast<cl_int>(params.nodeCount),
                            static_cast<cl_int>(params.triCount),
                            static_cast<cl_int>(params.vertexNormalCount),
                            static_cast<cl_int>(params.resolution),
                            static_cast<cl_int>(baseWord),
                            static_cast<cl_int>(params.wordCount),
                            static_cast<cl_float>(params.fwnBeta));
                        if (!lastEvent())
            {
                return false;
            }
        }

        if (!params.completesBuild)
        {
                        gpuAccess.complete(lastEvent);
            return true;
        }

        cl::Event const readyEvent =
                    m_program.runNonBlockingWithWaitList(queue,
                                                                                             "markMeshSignCacheReady",
                                                                                             origin,
                                                                                             cl::NDRange{1, 1, 1},
                                                                                             gpuAccess.waitEvents(),
                                                                                             primitives.data.getBuffer(),
                                                                                             static_cast<cl_int>(params.signCacheReadyOffset),
                                                                                             static_cast<cl_int>(params.signCacheDataOffset),
                                                                                             static_cast<cl_int>(params.signCacheBetaOffset),
                                                                                             static_cast<cl_float>(params.fwnBeta));
        if (!readyEvent())
        {
            return false;
        }

            gpuAccess.complete(readyEvent);

        return true;
    }
}
