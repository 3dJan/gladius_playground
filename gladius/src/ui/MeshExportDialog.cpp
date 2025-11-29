#include "MeshExportDialog.h"
#include "FileDialogService.h"

#include "imgui.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace gladius::ui
{
    namespace
    {
                constexpr std::array<char const *, 4> METHOD_LABELS{
                    "Layered marching cubes (OpenVDB)",
                    "Dual contouring (octree)",
                    "Hierarchical dual contouring (adaptive)",
                    "Manifold dual contouring (GPU beta)"};

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

                constexpr std::array<char const *, 5> HIERARCHICAL_QUALITY_LABELS{
                    "Draft",
                    "Balanced",
                    "Fine",
                    "Ultra Fine",
                    "Custom"};

                constexpr std::array<char const *, 5> MANIFOLD_QUALITY_LABELS{
                    "Draft",
                    "Balanced",
                    "Fine",
                    "Ultra Fine",
                    "Custom"};
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

        // Always render the configuration dialog (it handles progress internally now)
        renderConfiguration(core);
    }

    std::string MeshExportDialog::getWindowTitle() const
    {
        return m_exportInProgress ? "Exporting STL" : "Export STL";
    }

    std::string MeshExportDialog::getExportMessage() const
    {
        switch (m_selectedMethod)
        {
        case io::SurfaceExtractionMethod::LayeredMarchingCubes:
            return "Exporting STL using layered marching cubes";
        case io::SurfaceExtractionMethod::DualContouring:
            return "Exporting STL using dual contouring";
        case io::SurfaceExtractionMethod::HierarchicalDualContouring:
            return "Exporting STL using hierarchical dual contouring";
        case io::SurfaceExtractionMethod::ManifoldDualContouring:
            return "Exporting STL using manifold dual contouring";
        default:
            return "Exporting STL";
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
        else if (m_activeExporter == &m_dualExporter)
        {
            m_dualExporter.finalize();
        }
        else if (m_activeExporter == &m_hierarchicalExporter)
        {
            m_hierarchicalExporter.finalize();
        }
        else if (m_activeExporter == &m_manifoldExporter)
        {
            m_manifoldExporter.finalize();
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
        if (m_activeExporter == &m_layeredExporter)
        {
            m_layeredExporter.finalize();
        }
        else if (m_activeExporter == &m_dualExporter)
        {
            m_dualExporter.finalize();
        }
        else if (m_activeExporter == &m_hierarchicalExporter)
        {
            m_hierarchicalExporter.finalize();
        }
        else if (m_activeExporter == &m_manifoldExporter)
        {
            m_manifoldExporter.finalize();
        }
        resetState();
    }

    void MeshExportDialog::onExportCompleted()
    {
        m_exportCompleted = true;
        m_exportInProgress = false;
        m_statusMessage = "Export completed successfully!";
        m_statusIsError = false;
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
                finalizeExport();
                onExportCompleted();
            }
        }

        ImGui::SetNextWindowSize(ImVec2(550.0F, 0.0F), ImGuiCond_FirstUseEver);
        if (ImGui::Begin(getWindowTitle().c_str(), &m_visible))
        {
            // File selection section
            renderFileSelection();
            
            ImGui::Separator();

            // Disable settings during export
            ImGui::BeginDisabled(m_exportInProgress);
            
            ImGui::TextUnformatted("Select surface extraction method for STL export.");

            int methodIndex = static_cast<int>(m_selectedMethod);
            if (ImGui::BeginCombo("Method", METHOD_LABELS.at(static_cast<std::size_t>(methodIndex))))
            {
                for (int i = 0; i < static_cast<int>(METHOD_LABELS.size()); ++i)
                {
                    bool const selected = (i == methodIndex);
                    if (ImGui::Selectable(METHOD_LABELS[static_cast<std::size_t>(i)], selected))
                    {
                        methodIndex = i;
                        m_selectedMethod = static_cast<io::SurfaceExtractionMethod>(i);
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
            else if (m_selectedMethod == io::SurfaceExtractionMethod::HierarchicalDualContouring)
            {
                int qualityIndex = static_cast<int>(m_hierarchicalQualityPreset);
                qualityIndex = std::clamp(
                  qualityIndex, 0, static_cast<int>(HIERARCHICAL_QUALITY_LABELS.size()) - 1);
                if (ImGui::BeginCombo(
                      "Quality",
                      HIERARCHICAL_QUALITY_LABELS.at(static_cast<std::size_t>(qualityIndex))))
                {
                    for (int i = 0; i < static_cast<int>(HIERARCHICAL_QUALITY_LABELS.size()); ++i)
                    {
                        bool const selected = (i == qualityIndex);
                        if (ImGui::Selectable(HIERARCHICAL_QUALITY_LABELS[static_cast<std::size_t>(i)],
                                              selected))
                        {
                            qualityIndex = i;
                            m_hierarchicalQualityPreset =
                              static_cast<io::HierarchicalDualContouringQuality>(i);

                            if (m_hierarchicalQualityPreset !=
                                io::HierarchicalDualContouringQuality::Custom)
                            {
                                switch (m_hierarchicalQualityPreset)
                                {
                                case io::HierarchicalDualContouringQuality::Draft:
                                    m_hierarchicalEnableProgressiveRefinement = false;
                                    break;
                                case io::HierarchicalDualContouringQuality::Balanced:
                                case io::HierarchicalDualContouringQuality::Fine:
                                case io::HierarchicalDualContouringQuality::UltraFine:
                                    m_hierarchicalEnableProgressiveRefinement = true;
                                    break;
                                case io::HierarchicalDualContouringQuality::Custom:
                                    break;
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

                ImGui::Checkbox("Enable GPU acceleration", &m_hierarchicalEnableGpu);
                ImGui::Checkbox("Enable progressive refinement",
                                &m_hierarchicalEnableProgressiveRefinement);

                if (!m_hierarchicalEnableProgressiveRefinement)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("refinement passes will be skipped");
                }

                ImGui::Checkbox("Project vertices to surface", &m_hierarchicalProjectToSurface);
                if (m_hierarchicalProjectToSurface)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("GPU post-processing for smoother surfaces");
                }

                ImGui::Separator();
                ImGui::Checkbox("Enable coarsening (experimental)", &m_hierarchicalEnableCoarsening);
                if (m_hierarchicalEnableCoarsening)
                {
                    ImGui::SameLine();
                    ImGui::TextDisabled("merge cells where safe to reduce triangles");
                }

                ImGui::BeginDisabled(!m_hierarchicalEnableCoarsening);
                ImGui::InputFloat("Minimum feature size", &m_hierarchicalMinFeatureSize, 0.1F, 1.0F, "%.3f");
                if (m_hierarchicalMinFeatureSize < 0.0F)
                {
                    m_hierarchicalMinFeatureSize = 0.0F;
                }
                ImGui::SameLine();
                ImGui::TextDisabled("world units; smaller features may be simplified");
                ImGui::EndDisabled();

                ImGui::TextWrapped(
                  "Hierarchical dual contouring incrementally refines an adaptive octree. "
                  "Use progressive refinement for the smoothest surfaces; disable it for a faster preview.");
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
                
                ImGui::Checkbox("Enable simplification", &m_manifoldEnableSimplification);
                if (m_manifoldEnableSimplification)
                {
                    ImGui::Indent();
                    
                    ImGui::SliderFloat("Max SDF error", 
                                       &m_manifoldSimplificationMaxError, 
                                       0.001F, 0.1F, 
                                       "%.3f mm");
                    ImGui::SameLine();
                    ImGui::TextDisabled("maximum deviation from surface");
                    
                    ImGui::SliderFloat("Flat threshold", 
                                       &m_manifoldSimplificationFlatThreshold, 
                                       0.8F, 0.99F, 
                                       "cos(angle) = %.2f");
                    ImGui::SameLine();
                    ImGui::TextDisabled("higher = more aggressive (0.95 ≈ 18°)");
                    
                    ImGui::Unindent();
                }

                ImGui::TextWrapped(
                  "Manifold dual contouring is an experimental GPU path. Results may be incomplete "
                  "while the kernels are under active development.");
            }

            ImGui::EndDisabled(); // End disabled during export
            
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
            bool const canExport = !m_targetFile.empty() && !m_exportInProgress;
            
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
            
            if (m_exportInProgress)
            {
                if (ImGui::Button("Cancel Export"))
                {
                    onExportCancelled();
                    m_statusMessage = "Export cancelled";
                    m_statusIsError = false;
                }
            }
            else
            {
                if (ImGui::Button("Close"))
                {
                    resetState();
                    m_visible = false;
                }
            }
        }
        ImGui::End();

        if (!m_visible)
        {
            resetState();
        }
    }
    
    void MeshExportDialog::renderFileSelection()
    {
        ImGui::Text("Target File:");
        
        // Display the current filename (read-only text input for copying)
        std::string displayPath = m_targetFile.empty() ? "(no file selected)" : m_targetFile.string();
        
        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x - 100.0F);
        ImGui::BeginDisabled(m_exportInProgress);
        
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
            std::filesystem::path defaultPath = m_targetFile.empty() ? std::filesystem::path{"part.stl"} : m_targetFile;
            m_browseDialog.saveFile({"*.stl"}, defaultPath);
        }
        ImGui::EndDisabled();
        
        // Check for result AFTER button processing - this way the button
        // won't be clickable in the same frame we consume the result
        if (auto result = m_browseDialog.checkResult())
        {
            if (*result) // User selected a file
            {
                auto filename = **result;
                filename.replace_extension(".stl");
                m_targetFile = filename;
                // Clear any previous export status when changing file
                if (m_exportCompleted)
                {
                    m_exportCompleted = false;
                    m_statusMessage.clear();
                }
            }
        }
        
        ImGui::EndDisabled();
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

    void MeshExportDialog::startExport(ComputeCore & core)
    {
        if (m_targetFile.empty())
        {
            throw std::runtime_error("No target filename specified for STL export");
        }

        switch (m_selectedMethod)
        {
        case io::SurfaceExtractionMethod::LayeredMarchingCubes:
        {
            std::size_t const quality = std::min<std::size_t>(m_marchingCubesQuality, QUALITY_LABELS.size() - 1);
            m_layeredExporter.setQualityLevel(quality);
            m_layeredExporter.beginExport(m_targetFile, core);
            m_activeExporter = &m_layeredExporter;
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
        case io::SurfaceExtractionMethod::HierarchicalDualContouring:
        {
            io::HierarchicalDualContouringOptions options{};
            options.qualityPreset = m_hierarchicalQualityPreset;
            options.applyPreset();
            options.config.enableGpuAcceleration = m_hierarchicalEnableGpu;
            options.config.enableProgressiveRefinement = m_hierarchicalEnableProgressiveRefinement;
            options.config.projectVerticesToSurface = m_hierarchicalProjectToSurface;
            options.config.enableCoarsening = m_hierarchicalEnableCoarsening;
            options.config.minFeatureSize = m_hierarchicalMinFeatureSize;
            if (!options.config.enableProgressiveRefinement)
            {
                options.config.refinementIterations = 0U;
            }

            m_hierarchicalExporter.setOptions(options);
            m_hierarchicalExporter.beginExport(m_targetFile, core);
            m_activeExporter = &m_hierarchicalExporter;
            break;
        }
        case io::SurfaceExtractionMethod::ManifoldDualContouring:
        {
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
            // Sharp feature post-processing options
            options.enableSharpFeaturePostProcess = m_manifoldEnableSharpFeaturePostProcess;
            options.sharpFeatureAngleThreshold = m_manifoldSharpFeatureAngleThreshold;
            options.subdivisionIterations = m_manifoldSubdivisionIterations;
            options.projectToSurface = m_manifoldProjectToSurface;
            // Mesh simplification options
            options.enableSimplification = m_manifoldEnableSimplification;
            options.simplificationMaxError = m_manifoldSimplificationMaxError;
            options.simplificationFlatThreshold = m_manifoldSimplificationFlatThreshold;

            m_manifoldExporter.setOptions(options);
            m_manifoldExporter.beginExport(m_targetFile, core);
            m_activeExporter = &m_manifoldExporter;
            break;
        }
        default:
            throw std::runtime_error("Unsupported surface extraction method");
        }

        m_exportInProgress = true;
    }

    void MeshExportDialog::resetState()
    {
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
        m_activeExporter = nullptr;
        m_exportInProgress = false;
        m_exportCompleted = false;
        m_errorMessage.clear();
        m_statusMessage.clear();
        m_statusIsError = false;
        // Note: m_targetFile is preserved so user can re-export to same file
    }
}
