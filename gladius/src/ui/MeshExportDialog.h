#pragma once
#include "BaseExportDialog.h"
#include "ExportState.h"
#include "FileDialogService.h"
#include "io/DualContouringStlExporter.h"
#include "io/HierarchicalDualContouringStlExporter.h"
#include "io/ManifoldDualContouringStlExporter.h"
#include "io/MeshExporter.h"
#include "io/MeshExporter3mf.h"
#include "io/SurfaceExtractionOptions.h"

#include <cstddef>
#include <filesystem>
#include <string>

// Forward declaration
namespace gladius
{
    class Document;
}

namespace gladius::ui
{
    /// @brief Output file format for mesh export
    enum class MeshOutputFormat
    {
        STL = 0,
        ThreeMF = 1
    };

    class MeshExportDialog : public BaseExportDialog
    {
      public:
        /// @brief Set the export state reference for blocking UI during export
        /// @param state Pointer to the ExportState (must remain valid)
        void setExportState(ExportState * state)
        {
            m_exportState = state;
        }

        /// @brief Set the document for 3MF thumbnail generation
        /// @param doc Pointer to the document (must remain valid during export)
        void setDocument(Document const * doc)
        {
            m_document = doc;
        }

        /// @brief Show the dialog with optional suggested filename
        /// @param suggestedFilename Optional initial filename to suggest
        void show(std::filesystem::path suggestedFilename = {});
        
        /// @brief Legacy beginExport - calls show() for backward compatibility
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
        void renderFileSelection();
        void renderStatusArea();
        void startExport(ComputeCore & core);
        void resetState();
        void resetExportState();

        std::filesystem::path m_targetFile;
        vdb::MeshExporter m_layeredExporter;
        vdb::MeshExporter3mf m_layeredExporter3mf;
        io::DualContouringStlExporter m_dualExporter;
        io::HierarchicalDualContouringStlExporter m_hierarchicalExporter;
        io::ManifoldDualContouringStlExporter m_manifoldExporter;
        io::IExporter * m_activeExporter = nullptr;
        ComputeCore * m_computeCore = nullptr;
        Document const * m_document = nullptr;
        MeshOutputFormat m_outputFormat = MeshOutputFormat::ThreeMF;
        io::SurfaceExtractionMethod m_selectedMethod =
          io::SurfaceExtractionMethod::ManifoldDualContouring;
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
          io::ManifoldDualContouringQuality::UltraFine;
        bool m_manifoldEnableGpu = true;
        bool m_manifoldAllowCpuFallback = true;
        bool m_manifoldEnableCaching = true;
        float m_manifoldIsoValue = 0.0F;
        std::size_t m_manifoldMaxDepth = 9U;  // Sync with UltraFine preset
        // Minimum feature size and chunking
        float m_manifoldMinFeatureSize = 0.0F;
        bool m_manifoldEnableChunking = true;
        // Sharp feature post-processing
        bool m_manifoldEnableSharpFeaturePostProcess = false;
        float m_manifoldSharpFeatureAngleThreshold = 0.5F;
        std::size_t m_manifoldSubdivisionIterations = 1U;
        bool m_manifoldProjectToSurface = true;
        // Mesh simplification
        bool m_manifoldEnableSimplification = false;
        int m_manifoldSimplificationMethod = 0;  ///< 0=None, 1=QemSdfAware, 2=MeshOptimizer
        float m_manifoldSimplificationMaxSdfError = 0.01F;
        float m_manifoldSimplificationSdfWeight = 0.5F;
        float m_manifoldSimplificationNormalWeight = 0.3F;
        float m_meshOptimizerTargetError = 0.01F;
        bool m_meshOptimizerUseSloppy = false;
        bool m_exportInProgress = false;
        std::string m_errorMessage;
        
        // Status display
        std::string m_statusMessage;
        bool m_statusIsError = false;
        bool m_exportCompleted = false;
        
        // File browse dialog
        AsyncFileDialog m_browseDialog;
        
        // Export state for blocking UI modifications during export
        ExportState * m_exportState = nullptr;
    };
} // namespace gladius::ui
