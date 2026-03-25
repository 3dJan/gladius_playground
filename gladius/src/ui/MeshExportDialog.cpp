#include "MeshExportDialog.h"
#include "Document.h"
#include "FileDialogService.h"
#include "ColorToThicknessDialog.h"
#include "io/3mf/PaletteExtractor.h"
#include "Mesh.h"
#include "io/3mf/ShellGenerator.h"

#include "imgui.h"

#include <eigen3/Eigen/Core>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <compute/ManifoldDualContouringGpu.h>
#include <DualContouringSamplingProgram.h>
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

    void MeshExportDialog::beginExport(std::filesystem::path const & stlFilename,
                                       ComputeCore & core)
    {
        // Legacy method for backward compatibility
        m_computeCore = &core;
        show(stlFilename);
    }

    void MeshExportDialog::render(ComputeCore & core)
    {
        if (!m_visible)
        {
            return;
        }

        // Store compute core reference for export operations
        m_computeCore = &core;

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
        renderConfiguration(core);

        // Render auxiliary dialogs
        m_colorToThicknessDialog.render();
    }

    std::string MeshExportDialog::getWindowTitle() const
    {
        if (m_exportInProgress)
        {
            return m_outputFormat == MeshOutputFormat::ThreeMF ? "Exporting 3MF" : "Exporting STL";
        }
        return "Export Mesh";
    }

    std::string MeshExportDialog::getExportMessage() const
    {
        std::string const formatName = m_outputFormat == MeshOutputFormat::ThreeMF ? "3MF" : "STL";
        switch (m_selectedMethod)
        {
        case io::SurfaceExtractionMethod::LayeredMarchingCubes:
            return "Exporting " + formatName + " using layered marching cubes";
        case io::SurfaceExtractionMethod::DualContouring:
            return "Exporting " + formatName + " using dual contouring";
        case io::SurfaceExtractionMethod::ManifoldDualContouring:
            return "Exporting " + formatName + " using manifold dual contouring";
        default:
            return "Exporting " + formatName;
        }
    }

    io::IExporter & MeshExportDialog::getExporter()
    {
        if (m_activeExporter == nullptr)
        {
            throw std::runtime_error("Exporter requested before export was started");
        }
        return *m_activeExporter;
    }

    void MeshExportDialog::finalizeExport()
    {        
        if (m_activeExporter == &m_layeredExporter && m_computeCore != nullptr)
        {
            m_layeredExporter.finalizeExportSTL(*m_computeCore);
        }
        else if (m_activeExporter == &m_layeredExporter3mf)
        {
            m_layeredExporter3mf.finalize();
        }
        else if (m_activeExporter == &m_dualExporter)
        {
            m_dualExporter.finalize();
        }
        else if (m_activeExporter == &m_manifoldExporter)
        {
            m_manifoldExporter.finalize();
        }
        else if (m_activeExporter == &m_shellExporter)
        {
            m_shellExporter.finalize();
        }
        else
        {
            BaseExportDialog::finalizeExport();
        }

        // Only reset the exporter, not the whole dialog state
        m_activeExporter = nullptr;
    }

    void MeshExportDialog::onExportCancelled()
    {
        // Signal cancellation to the export worker - don't block!
        // The export loop will check isCancellationRequested() and exit early.
        // Cleanup happens when the export finishes (via finalizeExport/resetState).
        m_cancellationToken.requestCancellation();
        
        // Update ExportState to show "Cancelling..." phase in UI
        if (m_exportState != nullptr)
        {
            m_exportState->requestCancellation();
        }
    }

    void MeshExportDialog::onExportCompleted()
    {
        m_exportCompleted = true;
        m_exportInProgress = false;
        m_statusMessage = "Export completed successfully!";
        m_statusIsError = false;
        
        // Unlock UI modifications
        if (m_exportState != nullptr)
        {
            m_exportState->endExport();
        }
        // Dialog stays open - user can close manually or start another export
    }

    void MeshExportDialog::renderConfiguration(ComputeCore & core)
    {
        if (!m_visible)
        {
            return;
        }

        // Check if export is in progress and handle it
        if (m_exportInProgress && m_activeExporter != nullptr)
        {
            if (isExportFinished(core))
            {
                // IMPORTANT: some exporters reset their internal state in finalize().
                // Capture the error information BEFORE finalizeExport() runs.
                io::IExporter * const finishedExporter = m_activeExporter;

                bool failed = false;
                std::string failureMessage;
                if (finishedExporter == &m_dualExporter)
                {
                    failed = m_dualExporter.hasError();
                    failureMessage = m_dualExporter.errorMessage();
                }
                else if (finishedExporter == &m_manifoldExporter)
                {
                    failed = m_manifoldExporter.hasError();
                    failureMessage = m_manifoldExporter.errorMessage();
                }
                else if (finishedExporter == &m_shellExporter)
                {
                    failed = m_shellExporter.hasError();
                    failureMessage = m_shellExporter.errorMessage();
                }

                try
                {
                    finalizeExport();
                }
                catch (std::exception const & ex)
                {
                    failed = true;
                    if (failureMessage.empty())
                    {
                        failureMessage = ex.what();
                    }
                }

                // Final safety net: don't claim success unless the output exists.
                if (!failed && !m_targetFile.empty())
                {
                    std::error_code ec;
                    bool const exists = std::filesystem::exists(m_targetFile, ec);
                    if (ec || !exists)
                    {
                        failed = true;
                        failureMessage = fmt::format(
                          "Export finished but output file does not exist: {}",
                          m_targetFile.string());
                    }
                    else
                    {
                        auto const size = std::filesystem::file_size(m_targetFile, ec);
                        if (ec || size == 0U)
                        {
                            failed = true;
                            failureMessage = fmt::format(
                              "Export finished but output file is empty: {}",
                              m_targetFile.string());
                        }
                    }
                }

                // Check if export was cancelled - treat as distinct from failure
                bool const wasCancelled = m_cancellationToken.isCancelled();

                if (wasCancelled)
                {
                    // Export was cancelled by user - show cancellation message
                    m_exportInProgress = false;
                    m_exportCompleted = false;
                    m_statusMessage = "Export cancelled";
                    m_statusIsError = false;
                    m_errorMessage.clear();
                    if (m_exportState != nullptr)
                    {
                        m_exportState->endExport();
                    }
                    // Clean up any partial output file
                    if (!m_targetFile.empty())
                    {
                        std::error_code ec;
                        if (std::filesystem::exists(m_targetFile, ec))
                        {
                            std::filesystem::remove(m_targetFile, ec);
                            if (ec)
                            {
                                // Log warning but don't fail - the cancel itself succeeded
                                std::cerr << "Warning: Failed to delete partial export file: "
                                          << m_targetFile.string() << " (" << ec.message() << ")"
                                          << std::endl;
                            }
                        }
                    }
                }
                else if (failed)
                {
                    m_exportInProgress = false;
                    m_exportCompleted = false;
                    m_statusMessage = "Export failed";
                    m_statusIsError = true;
                    m_errorMessage =
                      failureMessage.empty() ? "Export failed (no additional error information)"
                                            : failureMessage;
                    if (m_exportState != nullptr)
                    {
                        m_exportState->endExport();
                    }
                }
                else
                {
                    onExportCompleted();
                }
            }
        }

        ImGui::SetNextWindowSize(ImVec2(550.0F, 0.0F), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(getWindowTitle().c_str(), &m_visible))
        {
            // During export: show only progress and cancel button
            if (m_exportInProgress)
            {
                renderStatusArea();
                
                ImGui::Spacing();
                
                // Check if cancellation is already requested
                bool const isCancelling = m_cancellationToken.isCancelled();
                
                if (isCancelling)
                {
                    // Show disabled "Cancelling..." button when cancellation is in progress
                    ImGui::BeginDisabled(true);
                    ImGui::Button("Cancelling...");
                    ImGui::EndDisabled();
                }
                else if (ImGui::Button("Cancel Export"))
                {
                    onExportCancelled();
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
            bool const canExport = !m_targetFile.empty();
            
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
                    startExport(core);
                }
                catch (std::exception const & ex)
                {
                    m_errorMessage = ex.what();
                    m_statusMessage = "Export failed";
                    m_statusIsError = true;
                }
            }
            ImGui::EndDisabled();
            
            if (m_targetFile.empty())
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
                if (ImGui::Selectable(METHOD_LABELS[static_cast<std::size_t>(i)], selected))
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
                        
                        // Sync maxDepth with the selected preset
                        io::ManifoldDualContouringOptions tempOpts{};
                        tempOpts.qualityPreset = m_manifoldQualityPreset;
                        tempOpts.applyPreset();
                        m_manifoldMaxDepth = tempOpts.maxDepth;
                    }
                    if (selected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }

            ImGui::Checkbox("Enable GPU acceleration", &m_manifoldEnableGpu);
            ImGui::Checkbox("Allow CPU fallback", &m_manifoldAllowCpuFallback);
            ImGui::Checkbox("Enable Morton caching", &m_manifoldEnableCaching);

            ImGui::InputFloat("ISO value", &m_manifoldIsoValue, 0.01F, 0.1F, "%.4f");

            int depthInput = static_cast<int>(m_manifoldMaxDepth);
            if (ImGui::InputInt("Maximum depth", &depthInput))
            {
                depthInput = std::max(depthInput, 1);
                m_manifoldMaxDepth = static_cast<std::size_t>(depthInput);
            }
            ImGui::SameLine();
            ImGui::TextDisabled("higher values capture more detail");

                        ImGui::Separator();
                        ImGui::Text("Watertightness (Experimental)");
                        ImGui::Checkbox("Enable hierarchical octree", &m_manifoldEnableHierarchicalOctree);
                        ImGui::SameLine();
                        ImGui::TextDisabled("global Morton octree with 2:1 balancing");
                        ImGui::TextWrapped(
                            "When enabled, Gladius uses a global balanced octree intended to improve watertightness/manifoldness. "
                            "This may increase memory usage and runtime.");

            ImGui::Separator();
            ImGui::Text("Minimum Feature Size (Thin Walls)");
            
            ImGui::InputFloat("Min feature size", &m_manifoldMinFeatureSize, 0.1F, 1.0F, "%.3f");
            if (m_manifoldMinFeatureSize < 0.0F)
            {
                m_manifoldMinFeatureSize = 0.0F;
            }
            ImGui::SameLine();
            ImGui::TextDisabled("world units; 0 = disabled");
            
            if (m_manifoldMinFeatureSize > 0.0F)
            {
                ImGui::Checkbox("Enable chunking", &m_manifoldEnableChunking);
                ImGui::SameLine();
                ImGui::TextDisabled("divide-and-conquer for memory efficiency");

                if (m_manifoldEnableHierarchicalOctree)
                {
                    ImGui::TextDisabled("Note: chunking is ignored when hierarchical octree is enabled.");
                }
            }

            ImGui::Separator();
            ImGui::Text("Sharp Feature Post-Processing");
            
            ImGui::Checkbox("Enable sharp feature refinement", &m_manifoldEnableSharpFeaturePostProcess);
            if (m_manifoldEnableSharpFeaturePostProcess)
            {
                ImGui::Indent();
                
                ImGui::SliderFloat("Angle threshold", 
                                   &m_manifoldSharpFeatureAngleThreshold, 
                                   0.0F, 1.0F, 
                                   "cos(angle) = %.2f");
                ImGui::SameLine();
                ImGui::TextDisabled("lower = more sensitive (0.5 = ~60°)");
                
                int subdivIters = static_cast<int>(m_manifoldSubdivisionIterations);
                if (ImGui::SliderInt("Subdivision iterations", &subdivIters, 1, 3))
                {
                    m_manifoldSubdivisionIterations = static_cast<std::size_t>(subdivIters);
                }
                
                ImGui::Checkbox("Project to surface", &m_manifoldProjectToSurface);
                
                ImGui::Unindent();
            }

            ImGui::Separator();
            ImGui::Text("Mesh Simplification");
            
            // Simplification method selection
            char const * const simplificationMethods[] = {"None", "QEM (SDF-aware)"};
            int const numMethods = 2;
            if (ImGui::BeginCombo("Simplification##simplmethod", simplificationMethods[m_manifoldSimplificationMethod]))
            {
                for (int i = 0; i < numMethods; ++i)
                {
                    bool const isSelected = (m_manifoldSimplificationMethod == i);
                    if (ImGui::Selectable(simplificationMethods[i], isSelected))
                    {
                        m_manifoldSimplificationMethod = i;
                    }
                    if (isSelected)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            
            if (m_manifoldSimplificationMethod == 1)  // QEM SDF-aware
            {
                ImGui::Indent();
                
                ImGui::SliderFloat("Max SDF error", 
                                   &m_manifoldSimplificationMaxSdfError, 
                                   0.001F, 0.1F, 
                                   "%.3f mm");
                ImGui::SameLine();
                ImGui::TextDisabled("maximum deviation from surface");
                
                ImGui::SliderFloat("SDF weight", 
                                   &m_manifoldSimplificationSdfWeight, 
                                   0.0F, 1.0F, 
                                   "%.2f");
                ImGui::SameLine();
                ImGui::TextDisabled("weight for position error");
                
                ImGui::SliderFloat("Normal weight", 
                                   &m_manifoldSimplificationNormalWeight, 
                                   0.0F, 1.0F, 
                                   "%.2f");
                ImGui::SameLine();
                ImGui::TextDisabled("weight for triangle orientation error");
                
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
        if (m_document != nullptr)
        {
            auto assembly = m_document->getAssembly();
            if (assembly != nullptr)
            {
                auto& model = assembly->assemblyModel();
                if (model != nullptr)
                {
                    m_modelHasVolumetricColor = io::FaceColorSampler::hasVolumetricColor(*model);
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
            ImGui::SliderInt("##PaletteSize", &m_maxPaletteSize, 2, 256, "%d colors");
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
                bool const shellExportSupported = colorExportAvailable &&
                    (m_selectedMethod == io::SurfaceExtractionMethod::ManifoldDualContouring);

                ImGui::BeginDisabled(!shellExportSupported);
                ImGui::Checkbox("Export shells with LUT", &m_enableShellBasedExport);
                if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && !shellExportSupported)
                {
                        ImGui::SetTooltip(
                            "Shell-based export requires 3MF format, volumetric color, "
                            "and the Manifold Dual Contouring method.");
                }
                ImGui::SameLine();
                ImGui::TextDisabled("One build item per shell with solid colors.");
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
        ImGui::Text("Target File:");
        
        // Display the current filename (read-only text input for copying)
        std::string displayPath = m_targetFile.empty() ? "(no file selected)" : m_targetFile.string();
        
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 100.0F);
        
        // Use InputText with read-only flag for the path display
        char pathBuffer[512] = {};
        std::strncpy(pathBuffer, displayPath.c_str(), sizeof(pathBuffer) - 1);
        ImGui::InputText("##filepath", pathBuffer, sizeof(pathBuffer), ImGuiInputTextFlags_ReadOnly);
        
        ImGui::SameLine();
        
        // Button first - if dialog is active, disable it
        bool const dialogActive = m_browseDialog.isActive();
        ImGui::BeginDisabled(dialogActive);
        if (ImGui::Button(dialogActive ? "Waiting..." : "Browse..."))
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
        if (m_exportInProgress && m_activeExporter != nullptr)
        {
            float const progress = static_cast<float>(m_activeExporter->getProgress());
            ImGui::ProgressBar(progress, ImVec2(-1.0F, 0.0F));
            ImGui::TextUnformatted(getExportMessage().c_str());
        }
        // Success message after export
        else if (m_exportCompleted)
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4{0.3F, 0.9F, 0.3F, 1.0F});
            ImGui::TextUnformatted(m_statusMessage.c_str());
            ImGui::PopStyleColor();
            
            ImGui::TextDisabled("Exported to: %s", m_targetFile.string().c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Open Folder"))
            {
                openFolderInFileManager(m_targetFile.parent_path());
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
        if (!colorExportAvailable || m_computeCore == nullptr)
        {
            return;
        }

        if (m_paletteDeriveInProgress.load())
        {
            return;
        }

        // Only support manifold dual contouring for palette derivation (matches color export path)
        if (m_selectedMethod != io::SurfaceExtractionMethod::ManifoldDualContouring)
        {
            m_colorToThicknessDialog.notifyPaletteDeriveFailed(
              "Palette derivation is only available with manifold dual contouring.");
            m_statusMessage = "Palette derivation currently supported only for Manifold dual contouring.";
            m_statusIsError = true;
            return;
        }

        io::ManifoldDualContouringOptions options{};
        options.qualityPreset = m_manifoldQualityPreset;
        options.applyPreset();
        options.enableGpu = m_manifoldEnableGpu;
        options.enableCpuFallback = m_manifoldAllowCpuFallback;
        options.enableCaching = m_manifoldEnableCaching;
        options.isoValue = m_manifoldIsoValue;
        if (m_manifoldMaxDepth > 0U)
        {
            options.maxDepth = m_manifoldMaxDepth;
            if (options.initialDepth > options.maxDepth)
            {
                options.initialDepth = options.maxDepth;
            }
        }
        options.minFeatureSize = m_manifoldMinFeatureSize;
        options.enableChunking = m_manifoldEnableChunking;
        options.enableHierarchicalOctree = m_manifoldEnableHierarchicalOctree;
        options.enableSharpFeaturePostProcess = m_manifoldEnableSharpFeaturePostProcess;
        options.sharpFeatureAngleThreshold = m_manifoldSharpFeatureAngleThreshold;
        options.subdivisionIterations = m_manifoldSubdivisionIterations;
        options.projectToSurface = m_manifoldProjectToSurface;
        options.simplificationMethod = static_cast<io::SimplificationMethod>(m_manifoldSimplificationMethod);
        options.enableSimplification = (m_manifoldSimplificationMethod != 0);
        options.simplificationMaxSdfError = m_manifoldSimplificationMaxSdfError;
        options.simplificationSdfWeight = m_manifoldSimplificationSdfWeight;
        options.simplificationNormalWeight = m_manifoldSimplificationNormalWeight;
        options.simplificationQemWeight = std::max(0.0F,
          1.0F - m_manifoldSimplificationSdfWeight - m_manifoldSimplificationNormalWeight);

        auto * core = m_computeCore;
        io::PaletteExtractionOptions paletteOptions{};
        paletteOptions.manifoldOptions = options;
        paletteOptions.convertToSrgb = m_convertToSrgb;

        m_paletteDeriveInProgress = true;
        m_colorToThicknessDialog.notifyPaletteDeriveStarted();

        m_paletteFuture = std::async(std::launch::async, [core, paletteOptions]() {
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
    }

    void MeshExportDialog::exportShellsTo3mf(ComputeCore & core)
    {
        if (m_document == nullptr)
        {
            throw std::runtime_error("Document is required for shell export");
        }

        auto stack = m_colorToThicknessDialog.getFilamentStack();
        if (stack.empty())
        {
            throw std::runtime_error("No materials defined for shell export");
        }

        int const lutResolution = m_colorToThicknessDialog.getLutResolution();
        if (lutResolution <= 1)
        {
            throw std::runtime_error("LUT resolution must be greater than 1 for shell export");
        }

        auto const & precomputedLuts = m_colorToThicknessDialog.getPrecomputedLuts();
        if (precomputedLuts.empty())
        {
            throw std::runtime_error("No precomputed LUTs available for shell export");
        }

        // Build MDC options from current settings
        io::ManifoldDualContouringOptions options{};
        options.qualityPreset = m_manifoldQualityPreset;
        options.applyPreset();
        options.enableGpu = m_manifoldEnableGpu;
        options.enableCpuFallback = m_manifoldAllowCpuFallback;
        options.enableCaching = m_manifoldEnableCaching;
        options.isoValue = m_manifoldIsoValue;
        if (m_manifoldMaxDepth > 0U)
        {
            options.maxDepth = m_manifoldMaxDepth;
            if (options.initialDepth > options.maxDepth)
            {
                options.initialDepth = options.maxDepth;
            }
        }
        options.minFeatureSize = m_manifoldMinFeatureSize;
        options.enableChunking = m_manifoldEnableChunking;
        options.enableHierarchicalOctree = m_manifoldEnableHierarchicalOctree;
        options.enableSharpFeaturePostProcess = m_manifoldEnableSharpFeaturePostProcess;
        options.sharpFeatureAngleThreshold = m_manifoldSharpFeatureAngleThreshold;
        options.subdivisionIterations = m_manifoldSubdivisionIterations;
        options.projectToSurface = m_manifoldProjectToSurface;
        options.simplificationMethod = static_cast<io::SimplificationMethod>(m_manifoldSimplificationMethod);
        options.enableSimplification = (m_manifoldSimplificationMethod != 0);
        options.simplificationMaxSdfError = m_manifoldSimplificationMaxSdfError;
        options.simplificationSdfWeight = m_manifoldSimplificationSdfWeight;
        options.simplificationNormalWeight = m_manifoldSimplificationNormalWeight;
        options.simplificationQemWeight = std::max(0.0F,
          1.0F - m_manifoldSimplificationSdfWeight - m_manifoldSimplificationNormalWeight);

        // Build shell export config
        io::ShellExportConfig config;
        config.filamentStack = std::move(stack);
        config.precomputedLuts = precomputedLuts;
        config.lutResolution = lutResolution;
        config.thicknessConstraints = m_colorToThicknessDialog.getConstraints();
        config.mdcOptions = std::move(options);
        config.useSurfaceColorSampling = m_useSurfaceColorSampling;

        // Reset cancellation token for the new export
        m_cancellationToken.reset();

        // Configure and start async shell export
        m_shellExporter.setConfig(std::move(config));
        m_shellExporter.setDocument(m_document);
        m_shellExporter.setCancellationToken(&m_cancellationToken);
        m_shellExporter.beginExport(m_targetFile, core);
        m_activeExporter = &m_shellExporter;

        // Enable progress tracking and UI lock
        m_exportInProgress = true;
        if (m_exportState != nullptr)
        {
            m_exportState->beginExport("3MF shell export");
        }
    }

    void MeshExportDialog::startExport(ComputeCore & core)
    {
        if (m_targetFile.empty())
        {
            throw std::runtime_error("No target filename specified for mesh export");
        }

                bool const is3mf = (m_outputFormat == MeshOutputFormat::ThreeMF);
                bool const wantsShellExport =
                    is3mf && m_enableShellBasedExport &&
                    m_selectedMethod == io::SurfaceExtractionMethod::ManifoldDualContouring;

                if (wantsShellExport && !m_colorToThicknessDialog.hasPrecomputedLuts())
                {
                        if (!m_colorToThicknessDialog.ensurePrecomputedLuts())
                        {
                                m_statusMessage = m_colorToThicknessDialog.getLutStatus();
                                if (m_statusMessage.empty())
                                {
                                        m_statusMessage =
                                            "Shell export requires materials and a generated LUT. "
                                            "Open the Color -> Shell Thickness dialog to define materials.";
                                }
                                m_statusIsError = true;
                                throw std::runtime_error(m_statusMessage);
                        }
                }

                bool const shellLutReady = m_colorToThicknessDialog.hasPrecomputedLuts();
                bool const is3mfMethodAllowed =
                    m_selectedMethod == io::SurfaceExtractionMethod::LayeredMarchingCubes ||
                    m_selectedMethod == io::SurfaceExtractionMethod::ManifoldDualContouring;

                if (is3mf && !is3mfMethodAllowed)
                {
                        throw std::runtime_error(
                            "3MF export is currently only supported with layered marching cubes, "
                            "manifold dual contouring, or shell-based hierarchical dual contouring when LUTs are "
                            "available.");
                }

        switch (m_selectedMethod)
        {
        case io::SurfaceExtractionMethod::LayeredMarchingCubes:
        {
            std::size_t const quality = std::min<std::size_t>(m_marchingCubesQuality, QUALITY_LABELS.size() - 1);
            if (is3mf)
            {
                m_layeredExporter3mf.setQualityLevel(quality);
                // Enable color export if checkbox is checked and model has color
                bool const exportColors = m_exportWithColors && m_modelHasVolumetricColor;
                m_layeredExporter3mf.setExportWithColors(exportColors);
                m_layeredExporter3mf.setConvertToSrgb(m_convertToSrgb);
                m_layeredExporter3mf.setColorMode(m_colorMode);
                m_layeredExporter3mf.setQuantizationMode(m_quantizationMode);
                m_layeredExporter3mf.setTargetApplication(m_targetApplication);
                if (m_overridePaletteSize)
                {
                    m_layeredExporter3mf.setMaxPaletteSize(
                        static_cast<std::uint32_t>(m_maxPaletteSize));
                }
                else
                {
                    m_layeredExporter3mf.setMaxPaletteSize(std::nullopt);
                }
                m_layeredExporter3mf.beginExport(m_targetFile, core, m_document);
                m_activeExporter = &m_layeredExporter3mf;
            }
            else
            {
                m_layeredExporter.setQualityLevel(quality);
                m_layeredExporter.beginExport(m_targetFile, core);
                m_activeExporter = &m_layeredExporter;
            }
            break;
        }
        case io::SurfaceExtractionMethod::DualContouring:
        {
            io::DualContouringOptions options{};
            options.qualityPreset = m_dualQualityPreset;
            
            // Apply preset to set all quality parameters (resolution, depth, curvature, etc.)
            options.applyPreset();
            
            // Override with user selections
            options.forceUniform = m_dualForceUniform;
            
            if (options.forceUniform && !std::has_single_bit(options.sdfResolution - 1U))
            {
                throw std::runtime_error(
                  "Uniform dual contouring requires resolution - 1 to be a power of two");
            }
            m_dualExporter.setOptions(options);
            m_dualExporter.beginExport(m_targetFile, core);
            m_activeExporter = &m_dualExporter;
            break;
        }
        case io::SurfaceExtractionMethod::ManifoldDualContouring:
        {
            if (is3mf && wantsShellExport)
            {
                exportShellsTo3mf(core);
                return;
            }

            io::ManifoldDualContouringOptions options{};
            options.qualityPreset = m_manifoldQualityPreset;
            options.applyPreset();
            options.enableGpu = m_manifoldEnableGpu;
            options.enableCpuFallback = m_manifoldAllowCpuFallback;
            options.enableCaching = m_manifoldEnableCaching;
            options.isoValue = m_manifoldIsoValue;
            if (m_manifoldMaxDepth > 0U)
            {
                options.maxDepth = m_manifoldMaxDepth;
                if (options.initialDepth > options.maxDepth)
                {
                    options.initialDepth = options.maxDepth;
                }
            }
            // Minimum feature size and chunking
            options.minFeatureSize = m_manifoldMinFeatureSize;
            options.enableChunking = m_manifoldEnableChunking;
            options.enableHierarchicalOctree = m_manifoldEnableHierarchicalOctree;
            // Sharp feature post-processing options
            options.enableSharpFeaturePostProcess = m_manifoldEnableSharpFeaturePostProcess;
            options.sharpFeatureAngleThreshold = m_manifoldSharpFeatureAngleThreshold;
            options.subdivisionIterations = m_manifoldSubdivisionIterations;
            options.projectToSurface = m_manifoldProjectToSurface;
            // Mesh simplification options
            options.simplificationMethod = static_cast<io::SimplificationMethod>(m_manifoldSimplificationMethod);
            options.enableSimplification = (m_manifoldSimplificationMethod != 0);  // Legacy support
            // QEM SDF-aware options
            options.simplificationMaxSdfError = m_manifoldSimplificationMaxSdfError;
            options.simplificationSdfWeight = m_manifoldSimplificationSdfWeight;
            options.simplificationNormalWeight = m_manifoldSimplificationNormalWeight;
            // QEM weight is the remainder after SDF and normal weights
            options.simplificationQemWeight = std::max(0.0F, 
                1.0F - m_manifoldSimplificationSdfWeight - m_manifoldSimplificationNormalWeight);

            m_manifoldExporter.setOptions(options);
            // Set output format and document for 3MF support
            m_manifoldExporter.setOutputFormat(is3mf ? io::MeshOutputFileFormat::ThreeMF 
                                                      : io::MeshOutputFileFormat::STL);
            m_manifoldExporter.setDocument(m_document);
            // Enable color export if checkbox is checked and model has color (3MF only)
            bool const exportColors = is3mf && m_exportWithColors && m_modelHasVolumetricColor;
            m_manifoldExporter.setExportWithColors(exportColors);
            m_manifoldExporter.setConvertToSrgb(m_convertToSrgb);
            m_manifoldExporter.setColorMode(m_colorMode);
            m_manifoldExporter.setQuantizationMode(m_quantizationMode);
            m_manifoldExporter.setTargetApplication(m_targetApplication);
            if (m_overridePaletteSize)
            {
                m_manifoldExporter.setMaxPaletteSize(
                    static_cast<std::uint32_t>(m_maxPaletteSize));
            }
            else
            {
                m_manifoldExporter.setMaxPaletteSize(std::nullopt);
            }
            m_manifoldExporter.beginExport(m_targetFile, core);
            m_activeExporter = &m_manifoldExporter;
            break;
        }
        default:
            throw std::runtime_error("Unsupported surface extraction method");
        }

        // Reset cancellation token for the new export and pass it to the exporter
        m_cancellationToken.reset();
        if (m_activeExporter != nullptr)
        {
            m_activeExporter->setCancellationToken(&m_cancellationToken);
        }

        m_exportInProgress = true;
        
        // Lock UI modifications during export
        if (m_exportState != nullptr)
        {
            std::string const formatName = is3mf ? "3MF" : "STL";
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
        
        m_activeExporter = nullptr;
        m_exportInProgress = false;
        m_exportCompleted = false;
        m_errorMessage.clear();
        m_statusMessage.clear();
        m_statusIsError = false;
        m_targetFile.clear();
    }
    
    void MeshExportDialog::resetExportState()
    {
        // Unlock UI if we were exporting
        if (m_exportInProgress && m_exportState != nullptr)
        {
            m_exportState->endExport();
        }
        
        m_activeExporter = nullptr;
        m_exportInProgress = false;
        m_exportCompleted = false;
        m_errorMessage.clear();
        m_statusMessage.clear();
        m_statusIsError = false;
        // Note: m_targetFile is preserved so user can re-export to same file
    }
}
