#include "LibraryExportDialog.h"

#include "imgui.h"
#include "io/3mf/LibraryMetadata.h"
#include "io/3mf/Writer3mf.h"

#include <algorithm>
#include <fmt/format.h>

namespace gladius::ui
{
    void LibraryExportDialog::open(SharedDocument doc, std::filesystem::path libraryRoot)
    {
        m_doc = std::move(doc);
        m_logger = m_doc ? m_doc->getSharedLogger() : nullptr;
        m_libraryRoot = std::move(libraryRoot);
        m_isOpen = true;
        m_exportCompleted = false;
        m_exportError = false;
        m_showOverwriteConfirm = false;
        m_selectedFunctionIndex = 0;
        m_selectedCategoryIndex = 0;
        m_useNewCategory = false;

        std::fill(std::begin(m_descriptionBuf), std::end(m_descriptionBuf), '\0');
        std::fill(std::begin(m_fileNameBuf), std::end(m_fileNameBuf), '\0');
        std::fill(std::begin(m_newCategoryBuf), std::end(m_newCategoryBuf), '\0');

        populateFunctionList();
        populateCategoryList();

        // Pre-fill filename from the first function's display name.
        if (!m_functions.empty())
        {
            auto safeName = m_functions[m_selectedFunctionIndex].displayName;
            std::replace(safeName.begin(), safeName.end(), ' ', '_');
            auto const r1 = fmt::format_to_n(m_fileNameBuf, FILENAME_BUF_SIZE - 1, "{}", safeName);
            *r1.out = '\0';
        }
    }

    bool LibraryExportDialog::wasExportCompleted()
    {
        if (m_exportCompleted)
        {
            m_exportCompleted = false;
            return true;
        }
        return false;
    }

    bool LibraryExportDialog::hadError()
    {
        if (m_exportError)
        {
            m_exportError = false;
            return true;
        }
        return false;
    }

    // ─── private helpers ──────────────────────────────────────────────

    void LibraryExportDialog::populateFunctionList()
    {
        m_functions.clear();
        if (!m_doc)
        {
            return;
        }

        auto assembly = m_doc->getAssembly();
        if (!assembly)
        {
            return;
        }

        auto const assemblyModelId = assembly->getAssemblyModelId();

        for (auto & [id, model] : assembly->getFunctions())
        {
            if (!model || id == assemblyModelId)
            {
                continue; // skip the composition root
            }

            FunctionEntry entry;
            entry.displayName =
              model->getDisplayName().value_or(fmt::format("Function #{}", id));
            entry.model = model;
            entry.resourceId = id;
            m_functions.push_back(std::move(entry));
        }
    }

    void LibraryExportDialog::populateCategoryList()
    {
        m_categories.clear();
        m_categories.emplace_back("(root)"); // index 0 → library root directly

        if (!std::filesystem::exists(m_libraryRoot))
        {
            return;
        }

        try
        {
            for (auto const & entry : std::filesystem::directory_iterator(m_libraryRoot))
            {
                if (entry.is_directory())
                {
                    m_categories.push_back(entry.path().filename().string());
                }
            }
        }
        catch (std::filesystem::filesystem_error const &)
        {
            // silently ignore scan errors
        }

        // Keep "(root)" first; sort the rest alphabetically.
        std::sort(m_categories.begin() + 1, m_categories.end());
    }

    // ─── render ───────────────────────────────────────────────────────

    void LibraryExportDialog::render()
    {
        if (!m_isOpen)
        {
            return;
        }

        auto constexpr TITLE = "Export to Library";
        if (!ImGui::IsPopupOpen(TITLE))
        {
            ImGui::OpenPopup(TITLE);
        }

        auto const flags =
          ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings;

        if (!ImGui::BeginPopupModal(TITLE, &m_isOpen, flags))
        {
            return;
        }

        // Early-out when no exportable functions exist.
        if (m_functions.empty())
        {
            ImGui::TextUnformatted("No functions available to export.");
            if (ImGui::Button("Close"))
            {
                m_isOpen = false;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
            return;
        }

        bool closePopup = false;

        // ── Function selector ────────────────────────────────────────
        ImGui::TextUnformatted("Function:");
        if (ImGui::BeginCombo("##func",
                              m_functions[m_selectedFunctionIndex].displayName.c_str()))
        {
            for (int i = 0; i < static_cast<int>(m_functions.size()); ++i)
            {
                bool const selected = (i == m_selectedFunctionIndex);
                if (ImGui::Selectable(m_functions[i].displayName.c_str(), selected))
                {
                    m_selectedFunctionIndex = i;
                    auto safeName = m_functions[i].displayName;
                    std::replace(safeName.begin(), safeName.end(), ' ', '_');
                    auto const r2 = fmt::format_to_n(m_fileNameBuf, FILENAME_BUF_SIZE - 1, "{}", safeName);
                    *r2.out = '\0';
                }
                if (selected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImGui::Spacing();

        // ── Description ──────────────────────────────────────────────
        ImGui::TextUnformatted("Description:");
        ImGui::InputTextMultiline(
          "##desc", m_descriptionBuf, DESCRIPTION_BUF_SIZE, ImVec2(350.0f, 60.0f));

        ImGui::Spacing();

        // ── Category selector ────────────────────────────────────────
        ImGui::TextUnformatted("Category:");
        if (!m_useNewCategory)
        {
            if (ImGui::BeginCombo("##cat",
                                  m_categories[m_selectedCategoryIndex].c_str()))
            {
                for (int i = 0; i < static_cast<int>(m_categories.size()); ++i)
                {
                    bool const sel = (i == m_selectedCategoryIndex);
                    if (ImGui::Selectable(m_categories[i].c_str(), sel))
                    {
                        m_selectedCategoryIndex = i;
                    }
                    if (sel)
                    {
                        ImGui::SetItemDefaultFocus();
                    }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine();
            if (ImGui::Button("New..."))
            {
                m_useNewCategory = true;
            }
        }
        else
        {
            ImGui::InputText("##newcat", m_newCategoryBuf, CATEGORY_BUF_SIZE);
            ImGui::SameLine();
            if (ImGui::Button("Existing"))
            {
                m_useNewCategory = false;
            }
        }

        ImGui::Spacing();

        // ── Filename ─────────────────────────────────────────────────
        ImGui::TextUnformatted("Filename:");
        ImGui::InputText("##fname", m_fileNameBuf, FILENAME_BUF_SIZE);
        ImGui::SameLine();
        ImGui::TextUnformatted(".3mf");

        ImGui::Spacing();
        ImGui::Separator();
        ImGui::Spacing();

        // Inform user that the full project is included in the export
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.85f, 0.4f, 1.0f));
        ImGui::TextWrapped(
          "Note: The exported file will contain the complete project. "
          "Only the selected function will be importable via the library.");
        ImGui::PopStyleColor();

        ImGui::Spacing();
        bool const canExport = m_fileNameBuf[0] != '\0';

        if (!canExport)
        {
            ImGui::BeginDisabled();
        }

        if (ImGui::Button("Export", ImVec2(120, 0)))
        {
            // Build target path.
            std::string categoryDir;
            if (m_useNewCategory && m_newCategoryBuf[0] != '\0')
            {
                categoryDir = m_newCategoryBuf;
            }
            else if (m_selectedCategoryIndex > 0)
            {
                categoryDir = m_categories[m_selectedCategoryIndex];
            }

            auto targetDir = m_libraryRoot;
            if (!categoryDir.empty())
            {
                targetDir /= categoryDir;
            }

            std::string fileName = m_fileNameBuf;
            if (fileName.size() < 4 ||
                fileName.substr(fileName.size() - 4) != ".3mf")
            {
                fileName += ".3mf";
            }

            m_targetPath = targetDir / fileName;

            if (std::filesystem::exists(m_targetPath))
            {
                m_showOverwriteConfirm = true;
            }
            else
            {
                performExport();
                closePopup = m_exportCompleted || m_exportError;
            }
        }

        if (!canExport)
        {
            ImGui::EndDisabled();
        }

        ImGui::SameLine();

        if (ImGui::Button("Cancel", ImVec2(120, 0)))
        {
            closePopup = true;
        }

        // ── Overwrite confirmation (nested popup) ────────────────────
        if (m_showOverwriteConfirm)
        {
            ImGui::OpenPopup("Overwrite?");
            m_showOverwriteConfirm = false; // open exactly once
        }

        if (ImGui::BeginPopupModal(
              "Overwrite?", nullptr, ImGuiWindowFlags_AlwaysAutoResize))
        {
            ImGui::Text("File '%s' already exists. Overwrite?",
                        m_targetPath.filename().string().c_str());
            ImGui::Spacing();

            if (ImGui::Button("Yes", ImVec2(120, 0)))
            {
                ImGui::CloseCurrentPopup();
                performExport();
                closePopup = m_exportCompleted || m_exportError;
            }
            ImGui::SameLine();
            if (ImGui::Button("No", ImVec2(120, 0)))
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // ── Close main popup if needed ───────────────────────────────
        if (closePopup)
        {
            m_isOpen = false;
            ImGui::CloseCurrentPopup();
        }

        ImGui::EndPopup();
    }

    // ─── export flow ──────────────────────────────────────────────────

    void LibraryExportDialog::performExport()
    {
        try
        {
            auto const & selectedFunc = m_functions[m_selectedFunctionIndex];

            // 1. Stamp library metadata on the source model before serialization.
            //    We export the full model (no pruning) because lib3mf's
            //    RemoveResource breaks internal state on models with cross-function
            //    ResourceIdNode references. Selective import at merge time already
            //    prunes correctly using the metadata tags.
            auto sourceModel = m_doc->get3mfModel();

            std::vector<Lib3MF_uint32> taggedIds;
            taggedIds.push_back(
              static_cast<Lib3MF_uint32>(selectedFunc.resourceId));

            io::LibraryMetadata metadata;
            metadata.libraryFunctions = io::serializeResourceIds(taggedIds);
            metadata.libraryDescription = std::string(m_descriptionBuf);
            io::writeLibraryMetadata(sourceModel, metadata);

            // 2. Ensure the target directory exists.
            auto const targetDir = m_targetPath.parent_path();
            if (!std::filesystem::exists(targetDir))
            {
                std::filesystem::create_directories(targetDir);
            }

            // 3. Write the model via Writer3mf (syncs graph, renders thumbnail,
            //    writes to disk).
            io::saveTo3mfFile(m_targetPath, *m_doc);

            // 4. Remove library metadata from the live source model
            //    so it doesn't persist across regular saves.
            io::removeLibraryMetadata(sourceModel);

            if (m_logger)
            {
                m_logger->addEvent(
                  {fmt::format("Exported '{}' to library: {}",
                               selectedFunc.displayName,
                               m_targetPath.string()),
                   events::Severity::Info});
            }

            m_exportCompleted = true;
        }
        catch (std::exception const & e)
        {
            // Ensure library metadata is cleaned up from the source model
            // even if the export failed.
            try
            {
                if (m_doc)
                {
                    io::removeLibraryMetadata(m_doc->get3mfModel());
                }
            }
            catch (...)
            {
            }

            if (m_logger)
            {
                m_logger->addEvent(
                  {fmt::format("Export to library failed: {}", e.what()),
                   events::Severity::Error});
            }
            m_exportError = true;
        }
    }

} // namespace gladius::ui
