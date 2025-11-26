#pragma once
#include "BaseExportDialog.h"
#include "io/DualContouringStlExporter.h"
#include "io/HierarchicalDualContouringStlExporter.h"
#include "io/ManifoldDualContouringStlExporter.h"
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
        io::ManifoldDualContouringStlExporter m_manifoldExporter;
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
        bool m_hierarchicalEnableCoarsening = false;
        float m_hierarchicalMinFeatureSize = 0.0F;
        io::ManifoldDualContouringQuality m_manifoldQualityPreset =
          io::ManifoldDualContouringQuality::Balanced;
        bool m_manifoldEnableGpu = true;
        bool m_manifoldAllowCpuFallback = true;
        bool m_manifoldEnableCaching = true;
        float m_manifoldIsoValue = 0.0F;
        std::size_t m_manifoldMaxDepth = 7U;
        // Sharp feature post-processing
        bool m_manifoldEnableSharpFeaturePostProcess = false;
        float m_manifoldSharpFeatureAngleThreshold = 0.5F;
        std::size_t m_manifoldSubdivisionIterations = 1U;
        bool m_manifoldProjectToSurface = true;
        // Mesh simplification
        bool m_manifoldEnableSimplification = false;
        float m_manifoldSimplificationMaxError = 0.01F;
        float m_manifoldSimplificationFlatThreshold = 0.95F;
        bool m_exportInProgress = false;
        std::string m_errorMessage;
    };
} // namespace gladius::ui
