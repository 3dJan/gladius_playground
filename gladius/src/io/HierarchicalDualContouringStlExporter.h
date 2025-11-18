#pragma once

#include "IExporter.h"
#include "SurfaceExtractionOptions.h"

#include "../HierarchicalDualContouring.h"
#include "../EventLogger.h"
#include "../Mesh.h"

#include <Eigen/Core>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace gladius
{
    class ComputeCore;
}

namespace gladius::io
{
    class HierarchicalDualContouringStlExporter : public IExporter
    {
      public:
        HierarchicalDualContouringStlExporter();
        explicit HierarchicalDualContouringStlExporter(events::SharedLogger logger);

        void setOptions(HierarchicalDualContouringOptions options);

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
                             std::vector<Eigen::Vector3f> const & vertices,
                             std::vector<std::uint32_t> const & indices) const;

        events::SharedLogger m_logger;
        HierarchicalDualContouringOptions m_options{};
        std::filesystem::path m_targetFile;
        ComputeCore * m_computeCore{nullptr};
        State m_state{State::Idle};
        double m_progress{0.0};
        std::string m_errorMessage;
    };
}
