#pragma once
#include "BaseExportDialog.h"
#include "io/DualContouringStlExporter.h"
#include "io/HierarchicalDualContouringStlExporter.h"
#include "io/MeshExporter.h"
#include "io/SurfaceExtractionOptions.h"

#include <cstddef>
#include <filesystem>
#include <string>

namespace gladius::ui
{
    class MeshExportDialog : public BaseExportDialog
    {
      public:
        void beginExport(std::filesystem::path const & stlFilename, ComputeCore & core) override;
        void render(ComputeCore & core) override;

      protected:
        [[nodiscard]] std::string getWindowTitle() const override;
        [[nodiscard]] std::string getExportMessage() const override;
        [[nodiscard]] io::IExporter & getExporter() override;

        void finalizeExport() override;
        void onExportCancelled() override;
        void onExportCompleted() override;

      private:
        void renderConfiguration(ComputeCore & core);
        void startExport(ComputeCore & core);
        void resetState();

        std::filesystem::path m_targetFile;
        vdb::MeshExporter m_layeredExporter;
        io::DualContouringStlExporter m_dualExporter;
        io::HierarchicalDualContouringStlExporter m_hierarchicalExporter;
        io::IExporter * m_activeExporter = nullptr;
        ComputeCore * m_computeCore = nullptr;
        io::SurfaceExtractionMethod m_selectedMethod =
          io::SurfaceExtractionMethod::LayeredMarchingCubes;
        std::size_t m_marchingCubesQuality = 1U;
        io::DualContouringQuality m_dualQualityPreset = io::DualContouringQuality::Balanced;
        bool m_dualForceUniform = false;
        io::HierarchicalDualContouringQuality m_hierarchicalQualityPreset =
          io::HierarchicalDualContouringQuality::Balanced;
        bool m_hierarchicalEnableGpu = true;
        bool m_hierarchicalEnableProgressiveRefinement = true;
        bool m_hierarchicalProjectToSurface = true;
        bool m_exportInProgress = false;
        std::string m_errorMessage;
    };
} // namespace gladius::ui
