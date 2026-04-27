#pragma once

#include "CLProgram.h"
#include "MeshVoxelGridManager.h"
#include "Primitives.h"

#include <mutex>

namespace gladius
{
    class MeshPreparationProgram
    {
      public:
        MeshPreparationProgram(SharedComputeContext context, SharedResources resources);

        void setLogger(events::SharedLogger logger);
        void setDebugLabel(std::string label);
        void setCacheDirectory(std::filesystem::path const & path);

        void ensureCompiled();
        [[nodiscard]] bool isValid() const;
        [[nodiscard]] bool isCompilationInProgress() const;
        void waitForCompilation();
        void requestShutdown();

        bool buildMeshVoxelGrid(Primitives & primitives, MeshVoxelGridBuildParams const & params);
        bool buildMeshFwnAggregates(Primitives & primitives,
                                    MeshFwnAggregateBuildParams const & params);
        bool buildMeshSignCache(Primitives & primitives, MeshSignCacheBuildParams const & params);

      private:
        SharedComputeContext m_computeContext;
        SharedResources m_resources;
        CLProgram m_program;
        std::mutex m_compileMutex;
        std::mutex m_queueMutex;
    };
}
