#include "MeshExportDialog.h"
#include "Document.h"
#include "FileDialogService.h"
#include "ColorToThicknessDialog.h"
#include "BrowserDownload.h"
#include "FileSystemUtils.h"
#if defined(GLADIUS_ENABLE_OPENCL)
#include "io/3mf/PaletteExtractor.h"
#include "compute/ComputeCore.h"
#endif
#include "nodes/Model.h"
#include "nodes/nodesfwd.h"

#include "imgui.h"

#include <eigen3/Eigen/Core>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <fmt/format.h>
#include <set>
#include <stdexcept>
#include <future>
#include <vector>

#ifdef _WIN32
#include <shellapi.h>
#endif

namespace gladius::ui
{
    namespace
    {
        constexpr std::array<char const *, 2> FORMAT_LABELS{
            "STL (Standard Triangle Language)",
            "3MF (3D Manufacturing Format)"};
        
        constexpr std::array<char const *, 2> FORMAT_EXTENSIONS{
            ".stl",
            ".model.3mf"};

        constexpr std::array<char const *, 2> METHOD_LABELS{
            "Layered marching cubes (OpenVDB)",
            "Manifold dual contouring (GPU)"};

        /// Maps UI method index to actual SurfaceExtractionMethod enum values
        constexpr std::array<io::SurfaceExtractionMethod, 2> METHOD_VALUES{
            io::SurfaceExtractionMethod::LayeredMarchingCubes,
            io::SurfaceExtractionMethod::ManifoldDualContouring};

        /// Get UI index from SurfaceExtractionMethod
        constexpr int getMethodIndex(io::SurfaceExtractionMethod method)
        {
            for (std::size_t i = 0; i < METHOD_VALUES.size(); ++i)
            {
                if (METHOD_VALUES[i] == method)
                {
                    return static_cast<int>(i);
                }
            }
            return 0; // Default to marching cubes
        }

        constexpr std::array<char const *, 4> QUALITY_LABELS{
          "Draft (fast)",
          "Balanced",
          "High fidelity",
          "Ultra"};

        constexpr std::array<char const *, 4> DUAL_QUALITY_LABELS{
          "Draft",
          "Balanced",
          "Fine",
          "Ultra Fine"};

        constexpr std::array<char const *, 5> MANIFOLD_QUALITY_LABELS{
            "Draft",
            "Balanced",
            "Fine",
            "Ultra Fine",
            "Custom"};

        std::vector<Eigen::Vector3f> defaultThicknessPalette()
        {
            return {
                Eigen::Vector3f{1.0F, 0.0F, 0.0F},
                Eigen::Vector3f{0.0F, 1.0F, 0.0F},
                Eigen::Vector3f{0.0F, 0.0F, 1.0F},
                Eigen::Vector3f{1.0F, 1.0F, 1.0F}
            };
        }
        
        /// Strips compound extensions like .implicit.3mf or .model.3mf down to base stem
        /// e.g., "mypart.implicit.3mf" -> "mypart", "mypart.model.3mf" -> "mypart"
        std::filesystem::path stripCompoundExtensions(std::filesystem::path const& path)
        {
            auto stem = path.stem();
            // Remove known compound suffixes
            while (stem.extension() == ".implicit" || stem.extension() == ".model")
            {
                stem = stem.stem();
            }
            return stem;
        }

        /// True when the dialog runs in the browser, where exports are delivered as downloads
#if defined(__EMSCRIPTEN__)
        constexpr bool IS_BROWSER_BUILD = true;
#else
        constexpr bool IS_BROWSER_BUILD = false;
#endif

        /// Browser exports have no save dialog; the download name is derived from the target file
        std::string browserDownloadName(std::filesystem::path const & targetFile, MeshOutputFormat format)
        {
            std::string const stem = targetFile.empty()
                                       ? std::string{"mesh"}
                                       : stripCompoundExtensions(targetFile).string();
            return stem + FORMAT_EXTENSIONS[static_cast<std::size_t>(format)];
        }

        /// Scratch location in the browser virtual file system for the generated mesh
        std::filesystem::path makeBrowserExportPath(char const * extension)
        {
            auto const directory = getAppDir() / "browser-downloads";
            std::filesystem::create_directories(directory);
            return directory / (std::string{"mesh-export"} + extension);
        }

        char const * browserDownloadMimeType(MeshOutputFormat format) noexcept
        {
            return format == MeshOutputFormat::ThreeMF
                     ? "application/vnd.ms-package.3dmanufacturing-3mf"
                     : "model/stl";
        }

        /// Creates the export job; the browser steps the export coroutine so the UI thread never blocks
        std::unique_ptr<io::MeshExportJob> createMeshExportJob(std::shared_ptr<Document> const & document,
                                                               io::MeshExportRequest request)
        {
#if defined(GLADIUS_ENABLE_WEBGPU)
            if (IS_BROWSER_BUILD)
            {
                return std::make_unique<io::SteppedMeshExportJob>(
                  [document, request = std::move(request)](auto shouldCancel, auto reportProgress) {
                      return document->exportMeshAsync(request, std::move(shouldCancel), std::move(reportProgress));
                  });
            }
#endif
            return std::make_unique<io::AsyncMeshExportJob>(
              [document, request = std::move(request)](auto const & shouldCancel,
                                                       auto const & reportProgress) {
                  document->exportMesh(request, shouldCancel, reportProgress);
              });
        }

        [[nodiscard]] bool supportsShellExport(io::SurfaceExtractionMethod method) noexcept
        {
            return method == io::SurfaceExtractionMethod::LayeredMarchingCubes ||
                   method == io::SurfaceExtractionMethod::ManifoldDualContouring;
        }

        [[nodiscard]] io::ShellGenerationMode shellGenerationModeFor(
            io::SurfaceExtractionMethod method) noexcept
        {
            return method == io::SurfaceExtractionMethod::LayeredMarchingCubes
                ? io::ShellGenerationMode::OpenVdbColorThickness
                : io::ShellGenerationMode::LegacyManifoldDualContouring;
        }

        [[nodiscard]] char const * shellBackendLabelFor(io::SurfaceExtractionMethod method) noexcept
        {
            return method == io::SurfaceExtractionMethod::LayeredMarchingCubes
                ? "OpenVDB color-thickness shells"
                : "Legacy manifold dual contouring shells";
        }

        [[nodiscard]] bool hasVolumetricColor(nodes::Model const & model)
        {
            auto * const endNode = const_cast<nodes::Model &>(model).getEndNode();
            if (endNode == nullptr)
            {
                return false;
            }
            auto & parameters = endNode->parameter();
            auto const color = parameters.find(nodes::FieldNames::Color);
            return color != parameters.end() && color->second.getConstSource().has_value();
        }
    }

    void MeshExportDialog::show(std::filesystem::path suggestedFilename)
    {
        if (!m_visible)
        {
            // Only reset state when opening fresh, not when already visible
            resetExportState();
        }
        m_visible = true;
        if (!suggestedFilename.empty())
        {
            m_targetFile = std::move(suggestedFilename);
        }
    }

    void MeshExportDialog::render(std::shared_ptr<Document> const & document)
    {
        pollExportJob();
        if (!m_visible)
        {
            return;
        }

        m_document = document;

        if (!m_paletteHandlerBound)
        {
            m_colorToThicknessDialog.setPaletteRequestHandler([this]() { derivePaletteFromMesh(); });
            m_paletteHandlerBound = true;
        }

        // Poll async palette derivation
        if (m_paletteDeriveInProgress.load())
        {
            if (m_paletteFuture.valid() &&
                m_paletteFuture.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready)
            {
                auto result = m_paletteFuture.get();
                m_paletteDeriveInProgress = false;
                if (result.success)
                {
                    m_colorToThicknessDialog.notifyPaletteDeriveSucceeded(std::move(result.palette));
                    m_statusMessage = "Derived palette from mesh colors.";
                    m_statusIsError = false;
                }
                else
                {
                    m_colorToThicknessDialog.notifyPaletteDeriveFailed(result.error);
                    m_statusMessage = std::string("Failed to derive palette: ") + result.error;
                    m_statusIsError = true;
                }
            }
        }

        // Always render the configuration dialog (it handles progress internally now)
        renderConfiguration(document);

        // Render auxiliary dialogs
        m_colorToThicknessDialog.render();
    }

    void MeshExportDialog::pollExportJob()
    {
        if (!m_exportInProgress || !m_exportJob)
        {
            return;
        }

        m_exportJob->poll();
        auto const status = m_exportJob->getStatus();
        if (status.state != io::MeshExportJobState::Completed &&
            status.state != io::MeshExportJobState::Cancelled &&
            status.state != io::MeshExportJobState::Failed)
        {
            return;
        }

        m_exportInProgress = false;
        m_exportJob.reset();
        if (m_exportState != nullptr)
        {
            m_exportState->endExport();
        }

        if (status.state == io::MeshExportJobState::Cancelled)
        {
            m_exportCompleted = false;
            m_statusMessage = "Export cancelled";
            m_statusIsError = false;
            m_errorMessage.clear();
        }
        else if (status.state == io::MeshExportJobState::Failed)
        {
            m_exportCompleted = false;
            m_statusMessage = "Export failed";
            m_statusIsError = true;
            m_errorMessage = status.errorMessage.empty()
                               ? "Export failed (no additional error information)"
                               : status.errorMessage;
        }
        else if (IS_BROWSER_BUILD)
        {
            deliverBrowserDownload();
        }
        else
        {
            std::error_code error;
            bool const exists = std::filesystem::exists(m_exportOutputPath, error);
            auto const size = exists && !error
                                ? std::filesystem::file_size(m_exportOutputPath, error)
                                : 0U;
            if (error || !exists || size == 0U)
            {
                m_exportCompleted = false;
                m_statusMessage = "Export failed";
                m_statusIsError = true;
                m_errorMessage = fmt::format(
                  "Export finished without a valid output file: {}", m_exportOutputPath.string());
            }
            else
            {
                m_exportCompleted = true;
                m_statusMessage = "Export completed successfully!";
                m_statusIsError = false;
                m_errorMessage.clear();
            }
        }

        if (IS_BROWSER_BUILD)
        {
            std::error_code ignored;
            std::filesystem::remove(m_exportOutputPath, ignored);
        }
    }

    void MeshExportDialog::deliverBrowserDownload()
    {
        std::error_code error;
        bool const exists = std::filesystem::exists(m_exportOutputPath, error);
        auto const size = exists && !error
                            ? std::filesystem::file_size(m_exportOutputPath, error)
                            : 0U;
        if (error || !exists || size == 0U)
        {
            m_exportCompleted = false;
            m_statusMessage = "Export failed";
            m_statusIsError = true;
            m_errorMessage = "Export finished without a valid output file.";
            return;
        }

        if (!startBrowserDownload(m_exportOutputPath, m_exportDownloadName,
                                  browserDownloadMimeType(m_outputFormat)))
        {
            m_exportCompleted = false;
            m_statusMessage = "Export failed";
            m_statusIsError = true;
            m_errorMessage = "The browser did not start the download.";
            return;
        }

        m_exportCompleted = true;
        m_statusMessage = "Download started.";
        m_statusIsError = false;
        m_errorMessage.clear();
    }

    void MeshExportDialog::renderConfiguration(std::shared_ptr<Document> const & document)
    {
        if (!m_visible)
        {
            return;
        }

        ImGui::SetNextWindowSize(ImVec2(550.0F, 0.0F), ImGuiCond_FirstUseEver);
        auto const windowTitle = m_exportInProgress
                                   ? (m_outputFormat == MeshOutputFormat::ThreeMF
                                        ? "Exporting 3MF"
                                        : "Exporting STL")
                                   : "Export Mesh";
        if (ImGui::Begin(windowTitle, &m_visible))
        {
            // During export: show only progress and cancel button
            if (m_exportInProgress)
            {
                renderStatusArea();
                
                ImGui::Spacing();
                
                auto const status = m_exportJob->getStatus();
                bool const isCancelling = status.state == io::MeshExportJobState::Cancelling;
                
                if (isCancelling)
                {
                    // Show disabled "Cancelling..." button when cancellation is in progress
                    ImGui::BeginDisabled(true);
                    ImGui::Button("Cancelling...");
                    ImGui::EndDisabled();
                }
                else if (ImGui::Button("Cancel Export"))
                {
                    m_exportJob->requestCancellation();
                    if (m_exportState != nullptr)
                    {
                        m_exportState->requestCancellation();
                    }
                    m_statusMessage = "Cancelling export...";
                    m_statusIsError = false;
                }
                
                ImGui::End();
                return;
            }
            
            // File selection section
            renderFileSelection();
            
            ImGui::Separator();
            
            // Tab bar for settings
            if (ImGui::BeginTabBar("ExportSettingsTabs"))
            {
                if (ImGui::BeginTabItem("Mesh Extraction"))
                {
                    renderMeshExtractionTab();
                    ImGui::EndTabItem();
                }
                
                if (ImGui::BeginTabItem("Color / Material"))
                {
                    renderColorMaterialTab();
                    ImGui::EndTabItem();
                }
                
                ImGui::EndTabBar();
            }
            
            ImGui::Separator();
            
            // Status area
            renderStatusArea();

            // Error message (separate from status)
            if (!m_errorMessage.empty())
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4{1.0F, 0.3F, 0.3F, 1.0F}, "%s", m_errorMessage.c_str());
            }
            
            ImGui::Spacing();

            // Buttons section
            bool const hasTarget = IS_BROWSER_BUILD || !m_targetFile.empty();
            bool const canExport = hasTarget && document != nullptr;
            
            ImGui::BeginDisabled(!canExport);
            if (ImGui::Button("Start Export"))
            {
                // Clear previous status when starting new export
                m_exportCompleted = false;
                m_statusMessage.clear();
                m_statusIsError = false;
                m_errorMessage.clear();
                
                try
                {
                    startExport(document);
                }
                catch (std::exception const & ex)
                {
                    m_errorMessage = ex.what();
                    m_statusMessage = "Export failed";
                    m_statusIsError = true;
                }
            }
            ImGui::EndDisabled();
            
            if (!hasTarget)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("(select a file first)");
            }
            
            ImGui::SameLine();
            
            if (ImGui::Button("Close"))
            {
                resetState();
                m_visible = false;
            }
        }
        ImGui::End();

        if (!m_visible)
        {
            resetState();
        }
    }
    
    void MeshExportDialog::renderMeshExtractionTab()
    {
        ImGui::Spacing();
        ImGui::TextUnformatted("Select surface extraction method for mesh export.");

        int methodIndex = getMethodIndex(m_selectedMethod);
        if (ImGui::BeginCombo("Method", METHOD_LABELS.at(static_cast<std::size_t>(methodIndex))))
        {
            for (int i = 0; i < static_cast<int>(METHOD_LABELS.size()); ++i)
            {
                bool const selected = (i == methodIndex);
                bool const unavailable =
                  IS_BROWSER_BUILD &&
                  METHOD_VALUES[static_cast<std::size_t>(i)] == io::SurfaceExtractionMethod::ManifoldDualContouring;
                ImGui::BeginDisabled(unavailable);
                bool const clicked = ImGui::Selectable(METHOD_LABELS[static_cast<std::size_t>(i)], selected);
                ImGui::EndDisabled();
                if (unavailable && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip("Not available in the browser build yet.");
                }
                if (clicked)
                {
                    methodIndex = i;
                    m_selectedMethod = METHOD_VALUES[static_cast<std::size_t>(i)];
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        if (m_selectedMethod == io::SurfaceExtractionMethod::LayeredMarchingCubes)
        {
            int qualityIndex = static_cast<int>(m_marchingCubesQuality);
            qualityIndex =
              std::clamp(qualityIndex, 0, static_cast<int>(QUALITY_LABELS.size()) - 1);
            if (ImGui::BeginCombo("Quality",
                                   QUALITY_LABELS.at(static_cast<std::size_t>(qualityIndex))))
            {
                for (int i = 0; i < static_cast<int>(QUALITY_LABELS.size()); ++i)
                {
                    bool const selected = (i == qualityIndex);
                    if (ImGui::Selectable(QUALITY_LABELS[static_cast<std::size_t>(i)], selected))
                    {
                        qualityIndex = i;
                        m_marchingCubesQuality = static_cast<std::size_t>(i);
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::TextWrapped(
              "Uses OpenVDB volume-to-mesh extraction. Higher quality settings take more time and memory.");
        }
        else if (m_selectedMethod == io::SurfaceExtractionMethod::DualContouring)
        {
            int qualityIndex = static_cast<int>(m_dualQualityPreset);
            qualityIndex =
              std::clamp(qualityIndex, 0, static_cast<int>(DUAL_QUALITY_LABELS.size()) - 1);
            if (ImGui::BeginCombo("Quality",
                                   DUAL_QUALITY_LABELS.at(static_cast<std::size_t>(qualityIndex))))
            {
                for (int i = 0; i < static_cast<int>(DUAL_QUALITY_LABELS.size()); ++i)
                {
                    bool const selected = (i == qualityIndex);
                    if (ImGui::Selectable(DUAL_QUALITY_LABELS[static_cast<std::size_t>(i)], selected))
                    {
                        qualityIndex = i;
                        m_dualQualityPreset = static_cast<io::DualContouringQuality>(i);
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::Checkbox("Force uniform octree", &m_dualForceUniform);
            if (m_dualForceUniform)
            {
                ImGui::SameLine();
                ImGui::TextDisabled("all octree leaves will have the same size");
            }
            ImGui::TextWrapped(
              "Dual contouring builds an adaptive octree over the SDF. Higher quality settings enable curvature refinement and use finer gradients for smoother surfaces.");
        }
        else if (m_selectedMethod == io::SurfaceExtractionMethod::ManifoldDualContouring)
        {
            int qualityIndex = static_cast<int>(m_manifoldQualityPreset);
            qualityIndex = std::clamp(
              qualityIndex, 0, static_cast<int>(MANIFOLD_QUALITY_LABELS.size()) - 1);
            if (ImGui::BeginCombo("Quality",
                                   MANIFOLD_QUALITY_LABELS.at(static_cast<std::size_t>(qualityIndex))))
            {
                for (int i = 0; i < static_cast<int>(MANIFOLD_QUALITY_LABELS.size()); ++i)
                {
                    bool const selected = (i == qualityIndex);
                    if (ImGui::Selectable(MANIFOLD_QUALITY_LABELS[static_cast<std::size_t>(i)],
                                          selected))
                    {
                        qualityIndex = i;
                        m_manifoldQualityPreset =
                          static_cast<io::ManifoldDualContouringQuality>(i);
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::Separator();
            ImGui::Text("Minimum Feature Size (Thin Walls)");
            
            ImGui::InputFloat("Min feature size", &m_manifoldMinFeatureSize, 0.1F, 1.0F, "%.3f");
            if (m_manifoldMinFeatureSize < 0.0F)
            {
                m_manifoldMinFeatureSize = 0.0F;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("world units; 0 = disabled");

            ImGui::Separator();
            ImGui::Text("Mesh Simplification");
            
            ImGui::Checkbox("Enable mesh simplification", &m_manifoldEnableSimplification);
            if (m_exportWithColors && m_modelHasVolumetricColor)
            {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(1.0F, 0.7F, 0.0F, 1.0F), "(!)" );
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Color exports may look better with\n"
                                      "simplification disabled.");
                }
            }
            
            if (m_manifoldEnableSimplification)
            {
                ImGui::Indent();
                
                ImGui::SliderFloat("Tolerance##simpltol",
                                   &m_manifoldSimplificationTolerance,
                                   0.001F, 1.0F,
                                   "%.3f mm",
                                   ImGuiSliderFlags_Logarithmic);
                ImGui::SameLine();
                ImGui::TextDisabled("(?)");
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("Maximum surface deviation allowed during\n"
                                      "simplification. Set this to your printer's\n"
                                      "layer height or XY resolution.");
                }
                
                ImGui::Unindent();
            }

            ImGui::TextWrapped(
              "Manifold dual contouring is an experimental GPU path. Results may be incomplete "
              "while the kernels are under active development.");
        }
    }
    
    void MeshExportDialog::renderColorMaterialTab()
    {
        ImGui::Spacing();
        
        // Check model for volumetric color output when document is available
        if (auto document = m_document.lock())
        {
            auto assembly = document->getAssembly();
            if (assembly != nullptr)
            {
                auto& model = assembly->assemblyModel();
                if (model != nullptr)
                {
                    m_modelHasVolumetricColor = hasVolumetricColor(*model);
                }
            }
        }
        
        bool const is3mf = (m_outputFormat == MeshOutputFormat::ThreeMF);
        bool const colorExportAvailable = is3mf && m_modelHasVolumetricColor;
        
        // Color export section
        ImGui::Text("Volumetric Color Export");
        ImGui::Separator();
        
        if (!is3mf)
        {
            ImGui::TextDisabled("Color export is only available for 3MF format.");
            ImGui::TextDisabled("Select 3MF format to enable color options.");
        }
        else if (!m_modelHasVolumetricColor)
        {
            ImGui::TextDisabled("The current model does not have volumetric color output.");
            ImGui::TextDisabled("Connect a color source to the End node's Color input to enable.");
        }
        else
        {
            ImGui::TextWrapped(
                "Export per-face colors sampled from the volumetric model. "
                "Colors are sampled at face centroids using the GPU.");
        }
        
        ImGui::Spacing();
        
        ImGui::BeginDisabled(!colorExportAvailable);
        
        ImGui::Checkbox("Export with colors", &m_exportWithColors);
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !colorExportAvailable)
        {
            if (!is3mf)
            {
                ImGui::SetTooltip("Select 3MF format to enable color export");
            }
            else
            {
                ImGui::SetTooltip("Model has no volumetric color output");
            }
        }
        
        // Only show color options when color export is enabled
        if (m_exportWithColors && colorExportAvailable)
        {
            ImGui::Indent();
            
            ImGui::Checkbox("Convert to sRGB", &m_convertToSrgb);
            ImGui::SameLine();
            ImGui::TextDisabled("(recommended for display)");
            
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip(
                    "Apply gamma correction for accurate display on monitors.\n"
                    "Disable if colors are already in sRGB or for raw linear output.");
            }

            // Color mode selection
            ImGui::Text("Color Mode:");
            ImGui::SameLine();
            
            int mode = static_cast<int>(m_colorMode);
            if (ImGui::RadioButton("Per-face (Compatible)", mode == 0))
            {
                m_colorMode = io::ColorMode::PerFace;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Flat shading, one color per triangle. Most compatible with slicers.");
            }
            
            ImGui::SameLine();
            if (ImGui::RadioButton("Per-vertex (Smooth)", mode == 1))
            {
                m_colorMode = io::ColorMode::PerVertex;
            }
            if (ImGui::IsItemHovered())
            {
                ImGui::SetTooltip("Smooth shading, three colors per triangle. May not be supported by all viewers.");
            }
            
            ImGui::Unindent();
        }
        
        ImGui::EndDisabled();
        
        ImGui::Spacing();
        ImGui::Spacing();

        // Compatibility tuning section
        ImGui::Text("Compatibility");
        ImGui::Separator();

        ImGui::BeginDisabled(!colorExportAvailable || !m_exportWithColors);

        // Target application selector
        ImGui::Text("Target Application:");
        ImGui::SameLine();
        int targetIdx = static_cast<int>(m_targetApplication);
        char const* targetLabels[] = {"None (portable)", "PrusaSlicer", "OrcaSlicer"};
        if (ImGui::Combo("##TargetApp", &targetIdx, targetLabels, 3))
        {
            m_targetApplication = static_cast<io::TargetApplication>(targetIdx);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Select a target slicer for optimized export.\n"
                "'None' produces standard-only output portable to any 3MF viewer.");
        }
        if (m_targetApplication != io::TargetApplication::None)
        {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.2f, 1.0f),
                "Warning: Portability to other slicers may be reduced.");
        }

        // Quantization mode
        ImGui::Text("Quantization:");
        ImGui::SameLine();
        int quantMode = static_cast<int>(m_quantizationMode);
        char const* quantLabels[] = {"Disabled", "Adaptive"};
        if (ImGui::Combo("##QuantMode", &quantMode, quantLabels, 2))
        {
            m_quantizationMode = static_cast<io::QuantizationMode>(quantMode);
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Adaptive: Automatically reduce colors to fit a palette for slicer compatibility.\n"
                "Disabled: Preserve all unique colors (may not produce printable regions).");
        }

        // Optional palette size override
        ImGui::Checkbox("Override palette size", &m_overridePaletteSize);
        if (m_overridePaletteSize)
        {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(100);
            int const maxSlider = (m_targetApplication != io::TargetApplication::None) ? 16 : 256;
            m_maxPaletteSize = std::min(m_maxPaletteSize, maxSlider);
            ImGui::SliderInt("##PaletteSize", &m_maxPaletteSize, 2, maxSlider, "%d colors");
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip(
                "Limit the number of distinct colors in the export.\n"
                "When unchecked, the palette is sized automatically.");
        }

        ImGui::EndDisabled();

        ImGui::Spacing();
        ImGui::Spacing();

        // HueForge-style color → thickness exploration dialog
        ImGui::BeginDisabled(!colorExportAvailable);
        if (ImGui::Button("Color \u2192 Shell Thickness..."))
        {
              m_colorToThicknessDialog.open();
        }
        ImGui::SameLine();
        ImGui::TextDisabled("Play with color-to-shell-thickness mapping before export.");
        ImGui::EndDisabled();
        
        ImGui::Spacing();
        ImGui::Spacing();

                auto const & precomputedLuts = m_colorToThicknessDialog.getPrecomputedLuts();
                bool const lutReady = !precomputedLuts.empty();
                int const lutResolution = m_colorToThicknessDialog.getLutResolution();
                bool const shellExportSupported = colorExportAvailable && supportsShellExport(m_selectedMethod);

                ImGui::BeginDisabled(!shellExportSupported);
                ImGui::Checkbox("Export shells with LUT", &m_enableShellBasedExport);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !shellExportSupported)
                {
                        ImGui::SetTooltip(
                            "Shell-based export requires 3MF format, volumetric color, "
                            "and a shell-capable extraction method selected in the Mesh Extraction tab.");
                }
                ImGui::SameLine();
                ImGui::TextDisabled("One build item per shell with solid colors.");
                ImGui::EndDisabled();

                ImGui::BeginDisabled(!shellExportSupported || !m_enableShellBasedExport);
                ImGui::Indent();
                ImGui::Text("Shell backend: %s", shellBackendLabelFor(m_selectedMethod));
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip(
                        "Shell export uses the method selected in the Mesh Extraction tab.\n"
                        "Choose layered marching cubes for the OpenVDB path or manifold dual contouring for the legacy shell path.");
                }
                if (m_selectedMethod == io::SurfaceExtractionMethod::LayeredMarchingCubes)
                {
                    ImGui::TextColored(ImVec4(1.0F, 0.8F, 0.2F, 1.0F),
                                       "Current scope: constant-thickness shell bands and surface-driven variable shell depths.");
                }
                else
                {
                    ImGui::TextDisabled("Current scope: legacy manifold shell generation.");
                }
                ImGui::Unindent();
                ImGui::EndDisabled();

                // Surface color sampling option (nested under shell export)
                ImGui::BeginDisabled(!shellExportSupported || !m_enableShellBasedExport);
                ImGui::Indent();
                ImGui::Checkbox("Use surface color sampling", &m_useSurfaceColorSampling);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                {
                    ImGui::SetTooltip(
                        "Sample colors at the model surface (SDF=0) instead of interior.\n"
                        "This fixes color accuracy for projected images and textures.\n"
                        "Recommended for HueForge-style multi-color prints.");
                }
                ImGui::Unindent();
                ImGui::EndDisabled();

                ImGui::Indent();
                if (lutReady)
                {
                        ImGui::TextDisabled("LUT ready: %zu layers @ %d^3",
                                                                precomputedLuts.size(),
                                                                lutResolution);
                }
                else
                {
                        ImGui::TextDisabled("No LUT yet. It will be generated automatically at export.");
                }
                auto const & lutStatus = m_colorToThicknessDialog.getLutStatus();
                if (!lutStatus.empty())
                {
                        ImGui::TextDisabled("%s", lutStatus.c_str());
                }
                ImGui::Unindent();

                ImGui::Spacing();
                ImGui::Spacing();
        
        // Future: Material settings placeholder
        ImGui::Text("Material Properties");
        ImGui::Separator();
        ImGui::TextDisabled("Material property export is not yet implemented.");
        ImGui::TextDisabled("Future versions will support PBR materials.");
    }
    
    void MeshExportDialog::renderFileSelection()
    {
        // Output format selection
        int formatIndex = static_cast<int>(m_outputFormat);
        if (ImGui::BeginCombo("Format", FORMAT_LABELS.at(static_cast<std::size_t>(formatIndex))))
        {
            for (int i = 0; i < static_cast<int>(FORMAT_LABELS.size()); ++i)
            {
                bool const selected = (i == formatIndex);
                if (ImGui::Selectable(FORMAT_LABELS[static_cast<std::size_t>(i)], selected))
                {
                    formatIndex = i;
                    auto const newFormat = static_cast<MeshOutputFormat>(i);
                    if (newFormat != m_outputFormat)
                    {
                        m_outputFormat = newFormat;
                        // Update file extension if a file is already selected
                        if (!m_targetFile.empty())
                        {
                            auto stem = stripCompoundExtensions(m_targetFile);
                            m_targetFile = m_targetFile.parent_path() / (stem.string() + FORMAT_EXTENSIONS[static_cast<std::size_t>(i)]);
                        }
                    }
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }
        
        ImGui::Spacing();
        ImGui::Text(IS_BROWSER_BUILD ? "Download name:" : "Target File:");
        
        // Display the current filename (read-only text input for copying)
        std::string const displayPath = IS_BROWSER_BUILD
                                          ? browserDownloadName(m_targetFile, m_outputFormat)
                                          : (m_targetFile.empty() ? "(no file selected)" : m_targetFile.string());
        
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 100.0F);
        
        // Use InputText with read-only flag for the path display
        char pathBuffer[512] = {};
        std::strncpy(pathBuffer, displayPath.c_str(), sizeof(pathBuffer) - 1);
        ImGui::InputText("##filepath", pathBuffer, sizeof(pathBuffer), ImGuiInputTextFlags_ReadOnly);
        
        ImGui::SameLine();
        
        // Button first - if dialog is active, disable it
        bool const dialogActive = m_browseDialog.isActive();
        ImGui::BeginDisabled(dialogActive);
        if (!IS_BROWSER_BUILD && ImGui::Button(dialogActive ? "Waiting..." : "Browse..."))
        {
            std::string const extension = FORMAT_EXTENSIONS[static_cast<std::size_t>(m_outputFormat)];
            std::string const filter = "*" + extension;
            std::filesystem::path defaultPath = m_targetFile.empty() 
                ? std::filesystem::path{"part" + extension} 
                : m_targetFile;
            // Remove compound extensions (.implicit.3mf, .model.3mf) before adding new one
            auto stem = stripCompoundExtensions(defaultPath);
            defaultPath = defaultPath.parent_path() / (stem.string() + extension);
            m_browseDialog.saveFile({filter}, defaultPath);
        }
        ImGui::EndDisabled();
        
        // Check for result AFTER button processing - this way the button
        // won't be clickable in the same frame we consume the result
        if (auto result = m_browseDialog.checkResult())
        {
            if (*result) // User selected a file
            {
                auto filename = **result;
                std::string const extension = FORMAT_EXTENSIONS[static_cast<std::size_t>(m_outputFormat)];
                // Remove compound extensions (.implicit.3mf, .model.3mf) before adding new one
                auto stem = stripCompoundExtensions(filename);
                filename = filename.parent_path() / (stem.string() + extension);
                m_targetFile = filename;
                // Clear any previous export status when changing file
                if (m_exportCompleted)
                {
                    m_exportCompleted = false;
                    m_statusMessage.clear();
                }
            }
        }
    }
    
    namespace
    {
        /// Opens the given folder in the system's default file manager
        void openFolderInFileManager(std::filesystem::path const & folderPath)
        {
#ifdef _WIN32
            ShellExecuteW(nullptr, L"open", folderPath.wstring().c_str(), nullptr, nullptr, SW_SHOW);
#elif defined(__APPLE__)
            std::string const command = "open \"" + folderPath.string() + "\"";
            std::system(command.c_str());
#else
            // Linux and other Unix-like systems
            std::string const command = "xdg-open \"" + folderPath.string() + "\" &";
            std::system(command.c_str());
#endif
        }
    }

    void MeshExportDialog::renderStatusArea()
    {
        // Progress bar during export
        if (m_exportInProgress && m_exportJob != nullptr)
        {
            auto const status = m_exportJob->getStatus();
            ImGui::ProgressBar(static_cast<float>(status.progress), ImVec2(-1.0F, 0.0F));
            ImGui::TextUnformatted(status.phase.c_str());
        }
        // Success message after export
        else if (m_exportCompleted)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.3F, 0.9F, 0.3F, 1.0F});
            ImGui::TextUnformatted(m_statusMessage.c_str());
            ImGui::PopStyleColor();
            
            if (IS_BROWSER_BUILD)
            {
                ImGui::TextDisabled("Check your browser downloads for: %s", m_exportDownloadName.c_str());
            }
            else
            {
                ImGui::TextDisabled("Exported to: %s", m_targetFile.string().c_str());
                ImGui::SameLine();
                if (ImGui::SmallButton("Open Folder"))
                {
                    openFolderInFileManager(m_targetFile.parent_path());
                }
            }
        }
        // Error message
        else if (m_statusIsError && !m_statusMessage.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{1.0F, 0.3F, 0.3F, 1.0F});
            ImGui::TextUnformatted(m_statusMessage.c_str());
            ImGui::PopStyleColor();
        }
        // Idle state - show hint
        else if (!m_statusMessage.empty())
        {
            ImGui::TextDisabled("%s", m_statusMessage.c_str());
        }
    }

    void MeshExportDialog::derivePaletteFromMesh()
    {
        bool const is3mf = (m_outputFormat == MeshOutputFormat::ThreeMF);
        bool const colorExportAvailable = is3mf && m_modelHasVolumetricColor;
        if (!colorExportAvailable)
        {
            return;
        }

        if (m_paletteDeriveInProgress.load())
        {
            return;
        }

#if !defined(GLADIUS_ENABLE_OPENCL)
        std::string const error =
          "Palette derivation is not implemented for the selected WebGPU backend.";
        m_colorToThicknessDialog.notifyPaletteDeriveFailed(error);
        m_statusMessage = error;
        m_statusIsError = true;
        return;
#else
        auto document = m_document.lock();
        auto core = document ? document->getCore() : nullptr;
        if (!core)
        {
            std::string const error =
              "Palette derivation requires the OpenCL mesh sampling backend.";
            m_colorToThicknessDialog.notifyPaletteDeriveFailed(error);
            m_statusMessage = error;
            m_statusIsError = true;
            return;
        }
        if (m_selectedMethod == io::SurfaceExtractionMethod::DualContouring)
        {
            std::string const error = "Palette derivation is not implemented for dual contouring.";
            m_colorToThicknessDialog.notifyPaletteDeriveFailed(error);
            m_statusMessage = error;
            m_statusIsError = true;
            return;
        }

        io::PaletteExtractionOptions paletteOptions{};
        paletteOptions.method = m_selectedMethod;
        paletteOptions.marchingCubesQualityLevel = m_marchingCubesQuality;
        paletteOptions.manifoldOptions = makeManifoldOptions();
        paletteOptions.convertToSrgb = true;

        m_paletteDeriveInProgress = true;
        m_colorToThicknessDialog.notifyPaletteDeriveStarted();

        m_paletteFuture = std::async(std::launch::async, [core = std::move(core), paletteOptions]() {
            PaletteDeriveResult result;
            try
            {
                auto palette = gladius::io::derivePaletteFromMesh(*core, paletteOptions);
                result.palette = std::move(palette);
                result.success = true;
            }
            catch (std::exception const & e)
            {
                result.error = e.what();
                result.success = false;
            }
            return result;
        });
#endif
    }

    io::ManifoldDualContouringOptions MeshExportDialog::makeManifoldOptions() const
    {
        io::ManifoldDualContouringOptions options{};
        options.qualityPreset = m_manifoldQualityPreset;
        options.applyPreset();
        options.enableGpu = m_manifoldEnableGpu;
        options.enableCpuFallback = m_manifoldAllowCpuFallback;
        options.enableCaching = m_manifoldEnableCaching;
        options.minFeatureSize = m_manifoldMinFeatureSize;
        options.enableHierarchicalOctree = m_manifoldEnableHierarchicalOctree;
        options.enableSharpFeaturePostProcess = m_manifoldEnableSharpFeaturePostProcess;
        options.sharpFeatureAngleThreshold = m_manifoldSharpFeatureAngleThreshold;
        options.subdivisionIterations = m_manifoldSubdivisionIterations;
        options.projectToSurface = m_manifoldProjectToSurface;
        options.simplificationMethod = m_manifoldEnableSimplification
                                         ? io::SimplificationMethod::QemFast
                                         : io::SimplificationMethod::None;
        options.enableSimplification = m_manifoldEnableSimplification;
        options.simplificationTerminationMode = io::SimplificationTerminationMode::ErrorBounded;
        options.simplificationMaxError = m_manifoldSimplificationTolerance *
                                         m_manifoldSimplificationTolerance;
        return options;
    }

    void MeshExportDialog::startExport(std::shared_ptr<Document> const & document)
    {
        if (!document)
        {
            throw std::runtime_error("No active document is available for mesh export");
        }
        if (!IS_BROWSER_BUILD && m_targetFile.empty())
        {
            throw std::runtime_error("No target filename specified for mesh export");
        }

        bool const is3mf = m_outputFormat == MeshOutputFormat::ThreeMF;
        bool const wantsShellExport =
          is3mf && m_enableShellBasedExport && supportsShellExport(m_selectedMethod);
        if (wantsShellExport && !m_colorToThicknessDialog.hasPrecomputedLuts() &&
            !m_colorToThicknessDialog.ensurePrecomputedLuts())
        {
            m_statusMessage = m_colorToThicknessDialog.getLutStatus();
            if (m_statusMessage.empty())
            {
                m_statusMessage =
                  "Shell export requires materials and a generated LUT. Open the Color -> Shell "
                  "Thickness dialog to define materials.";
            }
            m_statusIsError = true;
            throw std::runtime_error(m_statusMessage);
        }

        if (is3mf && !supportsShellExport(m_selectedMethod))
        {
            throw std::runtime_error(
              "3MF export is currently supported with layered marching cubes or manifold dual "
              "contouring.");
        }

        if (IS_BROWSER_BUILD)
        {
            if (m_selectedMethod == io::SurfaceExtractionMethod::ManifoldDualContouring)
            {
                throw std::runtime_error(
                  "Manifold dual contouring is not available in the browser build yet. Select "
                  "layered marching cubes.");
            }
            m_exportDownloadName = browserDownloadName(m_targetFile, m_outputFormat);
            m_exportOutputPath =
              makeBrowserExportPath(FORMAT_EXTENSIONS[static_cast<std::size_t>(m_outputFormat)]);
        }
        else
        {
            m_exportOutputPath = m_targetFile;
        }

        io::MeshExportRequest request;
        request.filename = m_exportOutputPath;
        request.format = is3mf ? io::MeshExportFormat::ThreeMf : io::MeshExportFormat::Stl;
        request.extraction.method = m_selectedMethod;
        request.extraction.marchingCubesQualityLevel =
          std::min<std::size_t>(m_marchingCubesQuality, QUALITY_LABELS.size() - 1U);
        request.extraction.manifoldDualContouring = makeManifoldOptions();
        request.color.exportWithColors = is3mf && m_exportWithColors && m_modelHasVolumetricColor;
        request.color.convertToSrgb = m_convertToSrgb;
        request.color.preferredColorMode = m_colorMode;
        request.color.quantizationMode = m_quantizationMode;
        request.color.targetApplication = m_targetApplication;
        if (m_overridePaletteSize)
        {
            request.color.maxPaletteSize = static_cast<std::uint32_t>(m_maxPaletteSize);
        }

        if (m_selectedMethod == io::SurfaceExtractionMethod::DualContouring)
        {
            request.extraction.dualContouring.qualityPreset = m_dualQualityPreset;
            request.extraction.dualContouring.applyPreset();
            request.extraction.dualContouring.forceUniform = m_dualForceUniform;
            auto const resolution = request.extraction.dualContouring.sdfResolution;
            if (m_dualForceUniform &&
                (resolution < 2U || !std::has_single_bit(resolution - 1U)))
            {
                throw std::runtime_error(
                  "Uniform dual contouring requires resolution - 1 to be a power of two");
            }
        }

        if (wantsShellExport)
        {
            auto stack = m_colorToThicknessDialog.getFilamentStack();
            if (stack.empty())
            {
                throw std::runtime_error("No materials defined for shell export");
            }
            auto const & precomputedLuts = m_colorToThicknessDialog.getPrecomputedLuts();
            auto const lutResolution = m_colorToThicknessDialog.getLutResolution();
            if (lutResolution <= 1 || precomputedLuts.empty())
            {
                throw std::runtime_error("Shell export requires valid precomputed thickness LUTs");
            }

            io::ShellExportConfig config;
            config.generationMode = shellGenerationModeFor(m_selectedMethod);
            config.filamentStack = std::move(stack);
            config.precomputedLuts = precomputedLuts;
            config.lutResolution = lutResolution;
            config.thicknessConstraints = m_colorToThicknessDialog.getConstraints();
            config.mdcOptions = request.extraction.manifoldDualContouring;
            config.useSurfaceColorSampling = m_useSurfaceColorSampling;
            request.shell = std::move(config);
        }

        std::string const formatName = is3mf ? "3MF" : "STL";
        m_exportJob = createMeshExportJob(document, std::move(request));
        m_exportInProgress = true;
        m_exportCompleted = false;
        if (m_exportState != nullptr)
        {
            m_exportState->beginExport(formatName + " mesh export");
        }
    }

    void MeshExportDialog::resetState()
    {
        // Unlock UI if we were exporting
        if (m_exportInProgress && m_exportState != nullptr)
        {
            m_exportState->endExport();
        }
        
        if (m_exportJob)
        {
            m_exportJob->requestCancellation();
            m_exportJob.reset();
        }
        m_exportInProgress = false;
        m_exportCompleted = false;
        m_errorMessage.clear();
        m_statusMessage.clear();
        m_statusIsError = false;
        m_targetFile.clear();
        m_document.reset();
    }
    
    void MeshExportDialog::resetExportState()
    {
        // Unlock UI if we were exporting
        if (m_exportInProgress && m_exportState != nullptr)
        {
            m_exportState->endExport();
        }
        
        m_exportJob.reset();
        m_exportInProgress = false;
        m_exportCompleted = false;
        m_errorMessage.clear();
        m_statusMessage.clear();
        m_statusIsError = false;
        // Note: m_targetFile is preserved so user can re-export to same file
    }
}
