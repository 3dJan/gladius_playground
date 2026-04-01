#pragma once
#include "BaseExportDialog.h"
#include "ExportState.h"
#include "FileDialogService.h"
#include "io/3mf/FaceColorSampler.h"
#include "ColorToThicknessDialog.h"
#include "io/CancellationToken.h"
#include "io/DualContouringStlExporter.h"
#include "io/ManifoldDualContouringStlExporter.h"
#include "io/MeshExporter.h"
#include "io/MeshExporter3mf.h"
#include "io/ShellExporter.h"
#include "io/SurfaceExtractionOptions.h"

#include <cstddef>
#include <filesystem>
#include <string>
#include <atomic>
#include <future>
#include <vector>

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
        void renderMeshExtractionTab();
        void renderColorMaterialTab();
        void renderStatusArea();
          void derivePaletteFromMesh();
        void exportShellsTo3mf(ComputeCore & core);
        void startExport(ComputeCore & core);
        void resetState();
        void resetExportState();

        template <typename Exporter>
        void applyColorSettings(Exporter& exporter, bool exportColors);

        std::filesystem::path m_targetFile;
        vdb::MeshExporter m_layeredExporter;
        vdb::MeshExporter3mf m_layeredExporter3mf;
        io::DualContouringStlExporter m_dualExporter;
        io::ManifoldDualContouringStlExporter m_manifoldExporter;
        io::ShellExporter m_shellExporter;
        io::IExporter * m_activeExporter = nullptr;
        ComputeCore * m_computeCore = nullptr;
        Document const * m_document = nullptr;
        MeshOutputFormat m_outputFormat = MeshOutputFormat::ThreeMF;
        io::SurfaceExtractionMethod m_selectedMethod =
          io::SurfaceExtractionMethod::LayeredMarchingCubes;
        std::size_t m_marchingCubesQuality = 1U;
        io::DualContouringQuality m_dualQualityPreset = io::DualContouringQuality::Balanced;
        bool m_dualForceUniform = false;
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
        // Hierarchical octree (watertight mesh generation)
        bool m_manifoldEnableHierarchicalOctree = true;
        // Sharp feature post-processing
        bool m_manifoldEnableSharpFeaturePostProcess = false;
        float m_manifoldSharpFeatureAngleThreshold = 0.5F;
        std::size_t m_manifoldSubdivisionIterations = 1U;
        bool m_manifoldProjectToSurface = true;
        // Mesh simplification
        bool m_manifoldEnableSimplification = false;
        int m_manifoldSimplificationMethod = 0;  ///< 0=None, 1=QemFast, 2=QemSdfAware
        int m_manifoldSimplificationTerminationMode = 0;  ///< 0=TargetCount, 1=ReductionPercent, 2=ErrorBounded
        float m_manifoldSimplificationMaxError = 1.0F;  ///< Max error for error-bounded mode
        float m_manifoldSimplificationMaxSdfError = 0.01F;
        float m_manifoldSimplificationSdfWeight = 0.5F;
        float m_manifoldSimplificationNormalWeight = 0.3F;
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
        
        // Color export options
        bool m_exportWithColors = true;  ///< Export with volumetric colors (3MF only)
        bool m_convertToSrgb = true;  ///< Convert linear RGB to sRGB for display
        io::ColorMode m_colorMode = io::ColorMode::PerFace; ///< Color export mode
        bool m_modelHasVolumetricColor = false;  ///< Cached: does model have color output?
        bool m_enableShellBasedExport = false; ///< Use shell-based export with LUTs when available
        bool m_useSurfaceColorSampling = true;  ///< Sample colors at surface instead of interior

        // Compatibility tuning options
        io::QuantizationMode m_quantizationMode = io::QuantizationMode::Adaptive;
        bool m_overridePaletteSize = false;  ///< Whether user has overridden palette size
        int m_maxPaletteSize = 16;           ///< Maximum palette colors (when overridden)
        io::TargetApplication m_targetApplication = io::TargetApplication::None;

        ColorToThicknessDialog m_colorToThicknessDialog;

        // Async palette derivation
        struct PaletteDeriveResult
        {
          std::vector<Eigen::Vector3f> palette;
          std::string error;
          bool success{false};
        };
        std::future<PaletteDeriveResult> m_paletteFuture;
        std::atomic<bool> m_paletteDeriveInProgress{false};
        bool m_paletteHandlerBound{false};

        // Cooperative cancellation token for async export
        io::CancellationToken m_cancellationToken;
    };
} // namespace gladius::ui
