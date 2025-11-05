#pragma once

#include "IExporter.h"
#include "SurfaceExtractionOptions.h"

#include "../DualContouringMesher.h"
#include "../EventLogger.h"
#include "../Mesh.h"

#include <filesystem>
#include <string>

namespace gladius
{
    class ComputeCore;
}

namespace gladius::io
{
    class DualContouringStlExporter : public IExporter
    {
      public:
        DualContouringStlExporter();
        explicit DualContouringStlExporter(events::SharedLogger logger);

        void setOptions(DualContouringOptions options);

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
                             dual_contouring::DualContouringMesh const & mesh) const;
        [[nodiscard]] dual_contouring::OctreeBuildConfig makeConfig() const;

        events::SharedLogger m_logger;
        DualContouringOptions m_options{};
        std::filesystem::path m_targetFile;
        ComputeCore * m_computeCore{nullptr};
        State m_state{State::Idle};
        double m_progress{0.0};
        std::string m_errorMessage;
    };
}
