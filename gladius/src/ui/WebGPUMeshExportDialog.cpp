#include "WebGPUMeshExportDialog.h"

#include "../Document.h"
#include "ExportState.h"
#include "io/SurfaceExtractionOptions.h"

#include <imgui.h>

#include <algorithm>
#include <exception>
#include <utility>

namespace gladius::ui
{
    WebGPUMeshExportDialog::~WebGPUMeshExportDialog()
    {
        if (m_exportJob)
        {
            m_exportJob->requestCancellation();
            m_exportJob->wait();
        }

        if (m_exportInProgress && m_exportState != nullptr)
        {
            m_exportState->endExport();
        }
    }

    void WebGPUMeshExportDialog::setExportState(ExportState * exportState) noexcept
    {
        m_exportState = exportState;
    }

    void WebGPUMeshExportDialog::show(std::filesystem::path suggestedFilename)
    {
        m_suggestedFilename = std::move(suggestedFilename);
        m_visible = true;
        if (!m_exportInProgress)
        {
            m_statusMessage.clear();
            m_errorMessage.clear();
        }
    }

    void WebGPUMeshExportDialog::render(std::shared_ptr<Document> const & document)
    {
        pollExport();

        if (auto const result = m_fileDialog.checkResult(); result.has_value() && result->has_value())
        {
            startExport(**result, document, m_pendingThreeMf);
        }

        if (!m_visible)
        {
            return;
        }

        ImGui::SetNextWindowSize(ImVec2(520.0F, 300.0F), ImGuiCond_FirstUseEver);
        if (!ImGui::Begin("Mesh Export (WebGPU)", &m_visible))
        {
            ImGui::End();
            return;
        }

        ImGui::TextWrapped(
                    "WebGPU exports standard dual-contouring meshes as binary STL or geometry-only 3MF. "
                    "Manifold extraction, color/material, and shell export are not available yet.");
        ImGui::Separator();

                constexpr char const * FORMAT_LABELS[] = {"Binary STL", "Geometry-only 3MF"};
        constexpr char const * QUALITY_LABELS[] = {
          "Draft (65 x 65 x 65)", "Balanced (129 x 129 x 129)", "Fine (257 x 257 x 257)"};
                ImGui::BeginDisabled(m_exportInProgress || m_fileDialog.isActive());
                ImGui::Combo("File format", &m_formatIndex, FORMAT_LABELS, 2);
        ImGui::Combo("SDF sampling quality", &m_qualityIndex, QUALITY_LABELS, 3);
        ImGui::EndDisabled();

        if (m_exportInProgress)
        {
            auto const status = m_exportJob->getStatus();
            ImGui::TextWrapped(
              "Generating the mesh on WebGPU and meshing on the CPU. Model editing is disabled "
              "until export completes.");
            if (!status.phase.empty())
            {
                ImGui::TextDisabled("%s", status.phase.c_str());
            }
            if (status.state == io::MeshExportJobState::Cancelling)
            {
                ImGui::BeginDisabled();
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
            }
        }
        else if (m_fileDialog.isActive())
        {
            ImGui::TextDisabled("Waiting for the save-file dialog...");
        }

        bool const exportUnavailable = !document || m_exportInProgress || m_fileDialog.isActive();
        ImGui::BeginDisabled(exportUnavailable);
        if (ImGui::Button("Export mesh..."))
        {
            m_errorMessage.clear();
            m_statusMessage.clear();
            m_pendingThreeMf = m_formatIndex == 1;
            auto suggestedFilename = m_suggestedFilename;
            suggestedFilename.replace_extension(m_pendingThreeMf ? ".3mf" : ".stl");
            m_fileDialog.saveFile({m_pendingThreeMf ? "*.3mf" : "*.stl"},
                                  std::move(suggestedFilename));
        }
        ImGui::EndDisabled();

        if (!document)
        {
            ImGui::TextDisabled("No active document is available for export.");
        }
        if (!m_statusMessage.empty())
        {
            ImGui::TextWrapped("%s", m_statusMessage.c_str());
        }
        if (!m_errorMessage.empty())
        {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0F, 0.35F, 0.3F, 1.0F));
            ImGui::TextWrapped("%s", m_errorMessage.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::End();
    }

    bool WebGPUMeshExportDialog::isBusy() const
    {
        return m_exportInProgress || m_fileDialog.isActive();
    }

    bool WebGPUMeshExportDialog::isVisible() const noexcept
    {
        return m_visible;
    }

    void WebGPUMeshExportDialog::startExport(std::filesystem::path filename,
                                             std::shared_ptr<Document> document,
                                             bool const exportThreeMf)
    {
        if (!document || m_exportInProgress)
        {
            return;
        }

        auto const extension = exportThreeMf ? ".3mf" : ".stl";
        if (filename.extension() != extension)
        {
            filename.replace_extension(extension);
        }

        io::StlExportOptions options;
        options.method = io::SurfaceExtractionMethod::DualContouring;
        options.dualContouring.forceUniform = true;

        switch (std::clamp(m_qualityIndex, 0, 2))
        {
        case 0:
            options.dualContouring.qualityPreset = io::DualContouringQuality::Draft;
            options.dualContouring.sdfResolution = 65U;
            break;
        case 2:
            options.dualContouring.qualityPreset = io::DualContouringQuality::Fine;
            options.dualContouring.sdfResolution = 257U;
            break;
        default:
            options.dualContouring.qualityPreset = io::DualContouringQuality::Balanced;
            options.dualContouring.sdfResolution = 129U;
            break;
        }

        m_targetFilename = filename;
        m_statusMessage = "Exporting " + filename.filename().string() + "...";
        m_errorMessage.clear();

        if (m_exportState != nullptr)
        {
            m_exportState->beginExport(exportThreeMf ? "WebGPU 3MF mesh export"
                                                    : "WebGPU STL mesh export");
        }

        try
        {
            m_exportJob = std::make_unique<io::AsyncMeshExportJob>(
              [document = std::move(document),
               filename = std::move(filename),
               options,
               exportThreeMf](auto const& shouldCancel, auto const& reportProgress)
              {
                  reportProgress(0.0, "Preparing WebGPU export");
                  if (shouldCancel())
                  {
                      throw io::MeshExportCancelled{};
                  }
                  reportProgress(0.0, "Sampling and meshing the model");
                  if (exportThreeMf)
                  {
                      document->exportAs3mf(filename, options, shouldCancel);
                  }
                  else
                  {
                      document->exportAsStl(filename, options, shouldCancel);
                  }
              });
            m_exportInProgress = true;
        }
        catch (std::exception const & error)
        {
            if (m_exportState != nullptr)
            {
                m_exportState->endExport();
            }
            m_statusMessage.clear();
            m_errorMessage = error.what();
        }
    }

    void WebGPUMeshExportDialog::pollExport()
    {
        if (!m_exportInProgress || !m_exportJob)
        {
            return;
        }

        m_exportJob->poll();
        auto const status = m_exportJob->getStatus();
        if (status.state == io::MeshExportJobState::Running ||
            status.state == io::MeshExportJobState::Cancelling)
        {
            return;
        }

        if (status.state == io::MeshExportJobState::Completed)
        {
            m_statusMessage = "Export completed: " + m_targetFilename.string();
            m_errorMessage.clear();
        }
        else if (status.state == io::MeshExportJobState::Cancelled)
        {
            m_statusMessage = "Export cancelled";
            m_errorMessage.clear();
        }
        else
        {
            m_statusMessage.clear();
            m_errorMessage = status.errorMessage.empty()
                               ? "Unknown error while exporting the WebGPU mesh."
                               : status.errorMessage;
        }

        m_exportInProgress = false;
        m_exportJob.reset();
        if (m_exportState != nullptr)
        {
            m_exportState->endExport();
        }
    }
} // namespace gladius::ui
