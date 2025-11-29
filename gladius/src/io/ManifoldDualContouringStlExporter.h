#pragma once

#include "IExporter.h"
#include "SurfaceExtractionOptions.h"

#include "../EventLogger.h"
#include "../Mesh.h"

#include <Eigen/Core>

#include <atomic>
#include <filesystem>
#include <future>
#include <memory>
#include <string>
#include <vector>

namespace gladius
{
    class ComputeCore;
}

namespace gladius::io
{
    class ManifoldDualContouringStlExporter : public IExporter
    {
      public:
        ManifoldDualContouringStlExporter();
        explicit ManifoldDualContouringStlExporter(events::SharedLogger logger);

        void setOptions(ManifoldDualContouringOptions options);

        void beginExport(std::filesystem::path const & fileName, ComputeCore & generator) override;
        bool advanceExport(ComputeCore & generator) override;
        void finalize() override;
        [[nodiscard]] double getProgress() const override;

        [[nodiscard]] bool hasError() const;
        [[nodiscard]] std::string const & errorMessage() const;

      private:
        enum class State
        {
            Idle,
            Running,
            Completed,
            Failed
        };

        void performExport(ComputeCore & generator);
        void writeMeshToFile(ComputeCore & generator,
                             std::vector<Eigen::Vector3f> const & positions,
                             std::vector<std::uint32_t> const & indices,
                             std::vector<Eigen::Vector3f> const & normals) const;

        events::SharedLogger m_logger;
        ManifoldDualContouringOptions m_options{};
        std::filesystem::path m_targetFile;
        ComputeCore * m_computeCore{nullptr};
        std::atomic<State> m_state{State::Idle};
        std::atomic<double> m_progress{0.0};
        std::string m_errorMessage;
        
        // Background thread for non-blocking export
        std::future<void> m_exportFuture;
    };
}
