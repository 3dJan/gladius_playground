#include "ThreemfFileViewer.h"
#include "../FileSystemUtils.h"
#include "../IconFontCppHeaders/IconsFontAwesome5.h"
#include "LibraryDragPayload.h"
#include "imgui.h"
#include <algorithm>
#include <fmt/format.h>

namespace gladius::ui
{
    ThreemfFileViewer::ThreemfFileViewer(events::SharedLogger logger)
        : m_logger(std::move(logger))
    {
        m_thumbnailExtractor = std::make_unique<ThreemfThumbnailExtractor>(m_logger);
        m_asyncLoader = std::make_unique<AsyncThumbnailLoader>(m_logger);
    }

    ThreemfFileViewer::~ThreemfFileViewer()
    {
        if (m_asyncLoader)
        {
            m_asyncLoader->cancelAll();
        }

        if (m_thumbnailExtractor)
        {
            for (auto & info : m_files)
            {
                m_thumbnailExtractor->releaseThumbnail(info);
            }
        }
    }

    void ThreemfFileViewer::setDirectory(const std::filesystem::path & directory)
    {
        if (m_directory != directory)
        {
            m_directory = directory;
            m_needsRefresh = true;
        }
    }

    void ThreemfFileViewer::refreshDirectory()
    {
        m_needsRefresh = true;
    }

#if defined(GLADIUS_UI_BACKEND_WEBGPU)
    void ThreemfFileViewer::setWebGPUContext(
      std::shared_ptr<webgpu::WebGPUComputeContext> context)
    {
        m_thumbnailExtractor->setWebGPUContext(std::move(context));
    }
#endif

    void ThreemfFileViewer::scanDirectory()
    {
        if (!m_needsRefresh)
        {
            return;
        }

        if (m_asyncLoader)
        {
            m_asyncLoader->cancelAll();
        }

        if (m_thumbnailExtractor)
        {
            for (auto & info : m_files)
            {
                m_thumbnailExtractor->releaseThumbnail(info);
            }
        }
        m_files.clear();

        if (!std::filesystem::exists(m_directory) || !std::filesystem::is_directory(m_directory))
        {
            m_needsRefresh = false;
            return;
        }

        try
        {
            for (const auto & entry : std::filesystem::directory_iterator(m_directory))
            {
                if (entry.is_regular_file() && entry.path().extension() == ".3mf")
                {
                    if (m_thumbnailExtractor)
                    {
                        auto info = m_thumbnailExtractor->createThumbnailInfo(entry.path(), 0);
                        m_files.push_back(std::move(info));
                        if (m_asyncLoader)
                        {
                            m_asyncLoader->requestLoad(m_files.back());
                        }
                    }
                }
            }
        }
        catch (const std::filesystem::filesystem_error & e)
        {
            if (m_logger)
            {
                m_logger->addEvent({e.what(), events::Severity::Error});
            }
        }

        m_needsRefresh = false;
    }

    void ThreemfFileViewer::render(SharedDocument doc)
    {
        // Scan the directory if needed
        scanDirectory();

        // Poll async loader and create textures for decoded thumbnails
        if (m_asyncLoader)
        {
            m_asyncLoader->update();
            m_asyncLoader->processPendingTextures();

            if (m_thumbnailExtractor)
            {
                for (auto & info : m_files)
                {
                    if (info.loadState == ThumbnailLoadState::DecodedPending)
                    {
                        m_thumbnailExtractor->createTextureFromPixels(info);
                    }
                }
            }
        }

        if (m_files.empty())
        {
            ImGui::TextUnformatted("No 3MF files found in the specified directory");
            return;
        }

        // Calculate grid layout
        float const availWidth = ImGui::GetContentRegionAvail().x - 10.0f;
        float const spacing = ImGui::GetStyle().ItemSpacing.x;
        float cellWidth = m_thumbnailSize + 20.0f;
        m_columns = std::max(1, static_cast<int>(std::floor(availWidth / cellWidth)));
        cellWidth = (availWidth - spacing * (m_columns - 1)) / m_columns;
        float const cellHeight = m_thumbnailSize + 60.0f;

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(5, 5));
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(10, 10));

        int itemIdx = 0;
        for (auto & info : m_files)
        {
            ImGui::PushID(itemIdx);

            if (itemIdx % m_columns != 0)
            {
                ImGui::SameLine();
            }

            ImVec2 const itemPos = ImGui::GetCursorPos();
            renderThumbnailItem(info, doc, cellWidth, cellHeight, itemPos);

            ImGui::PopID();
            itemIdx++;
        }

        ImGui::PopStyleVar(2);
    }

    void ThreemfFileViewer::renderThumbnailItem(ThreemfThumbnailExtractor::ThumbnailInfo & info,
                                                SharedDocument doc,
                                                float cellWidth,
                                                float cellHeight,
                                                const ImVec2 & itemPos)
    {
        ImGui::BeginGroup();

        // Selectable background button
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.2f, 0.2f, 0.1f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                              ImGui::GetStyleColorVec4(ImGuiCol_FrameBgHovered));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                              ImGui::GetStyleColorVec4(ImGuiCol_FrameBgActive));

        bool const isDoubleClicked =
          ImGui::Button("##thumb", ImVec2(cellWidth, cellHeight)) &&
          ImGui::IsMouseDoubleClicked(0);

        if (isDoubleClicked && doc)
        {
            try
            {
                doc->merge(info.filePath);
                if (m_logger)
                {
                    m_logger->addEvent(
                      {fmt::format("Loaded file: {}", info.fileName), events::Severity::Info});
                }
            }
            catch (const std::exception & e)
            {
                if (m_logger)
                {
                    m_logger->addEvent(
                      {fmt::format("Failed to load file {}: {}", info.fileName, e.what()),
                       events::Severity::Error});
                }
            }
        }

        // Tooltip
        if (ImGui::IsItemHovered())
        {
            ImGui::BeginTooltip();
            ImGui::TextUnformatted(info.filePath.string().c_str());
            if (!info.description.empty())
            {
                ImGui::Separator();
                ImGui::TextUnformatted(info.description.c_str());
            }
            if (!info.libraryFunctionNames.empty())
            {
                ImGui::Separator();
                ImGui::TextUnformatted("Functions:");
                for (const auto & name : info.libraryFunctionNames)
                {
                    ImGui::BulletText("%s", name.c_str());
                }
            }
            ImGui::Separator();
            ImGui::TextUnformatted("Double-click to import");
            ImGui::EndTooltip();
        }

        ImGui::PopStyleColor(3);

        // Drag-and-drop source
        if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_None))
        {
            ThreemfThumbnailExtractor::ThumbnailInfo const * payloadPtr = &info;
            ImGui::SetDragDropPayload(LIBRARY_DND_TYPE, &payloadPtr, sizeof(payloadPtr));

            if (info.hasLibraryMetadata && !info.libraryFunctionNames.empty())
            {
                ImGui::TextUnformatted(info.libraryFunctionNames.front().c_str());
            }
            else
            {
                ImGui::TextUnformatted(info.fileName.c_str());
            }
            ImGui::EndDragDropSource();
        }

        // Right-click context menu
        if (ImGui::BeginPopupContextItem("##entryCtx"))
        {
            bool const shipped = m_isShipped ? m_isShipped(info.filePath) : false;

            if (m_onDelete)
            {
                if (shipped)
                {
                    ImGui::BeginDisabled();
                    ImGui::MenuItem(ICON_FA_TRASH_ALT "  Delete");
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    {
                        ImGui::SetTooltip("Shipped entries cannot be deleted");
                    }
                }
                else if (ImGui::MenuItem(ICON_FA_TRASH_ALT "  Delete"))
                {
                    if (m_onDelete(info.filePath))
                    {
                        m_needsRefresh = true;
                    }
                }
            }

            if (m_onRestore && ImGui::MenuItem(ICON_FA_UNDO "  Restore"))
            {
                if (m_onRestore(info.filePath))
                {
                    m_needsRefresh = true;
                }
            }

            if (m_onPermanentDelete && ImGui::MenuItem(ICON_FA_TIMES "  Delete permanently"))
            {
                if (m_onPermanentDelete(info.filePath))
                {
                    m_needsRefresh = true;
                }
            }

            ImGui::Separator();

            if (ImGui::MenuItem(ICON_FA_EXTERNAL_LINK_ALT "  Open in Gladius"))
            {
                openFileInNewInstance(info.filePath);
            }

            ImGui::EndPopup();
        }

        // Draw thumbnail / placeholder over the button
        ImGui::SetItemAllowOverlap();
        ImGui::SetCursorPos(itemPos);

        float const thumbPosX = itemPos.x + (cellWidth - m_thumbnailSize) * 0.5f;
        ImGui::SetCursorPos(ImVec2(thumbPosX, itemPos.y + 5.0f));

        if (info.hasThumbnail && info.thumbnailTextureId != 0)
        {
            float displayWidth = m_thumbnailSize;
            float displayHeight = m_thumbnailSize;

            if (info.thumbnailWidth > 0 && info.thumbnailHeight > 0)
            {
                float const aspectRatio = static_cast<float>(info.thumbnailWidth) /
                                          static_cast<float>(info.thumbnailHeight);
                if (aspectRatio > 1.0f)
                {
                    displayHeight = m_thumbnailSize / aspectRatio;
                }
                else
                {
                    displayWidth = m_thumbnailSize * aspectRatio;
                }
            }

            float const centerX = thumbPosX + (m_thumbnailSize - displayWidth) * 0.5f;
            ImGui::SetCursorPos(ImVec2(
              centerX, itemPos.y + 5.0f + (m_thumbnailSize - displayHeight) * 0.5f));

            ImGui::Image(
                            static_cast<ImTextureID>(static_cast<uintptr_t>(info.thumbnailTextureId)),
              ImVec2(displayWidth, displayHeight));
        }
        else if (info.loadState == ThumbnailLoadState::Loading ||
                 info.loadState == ThumbnailLoadState::DecodedPending)
        {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.15f, 0.15f, 0.2f, 0.5f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.15f, 0.15f, 0.2f, 0.5f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.15f, 0.15f, 0.2f, 0.5f));
            char const * icon = (info.loadState == ThumbnailLoadState::Loading)
                                  ? ICON_FA_SPINNER
                                  : ICON_FA_IMAGE;
            ImGui::Button(icon, ImVec2(m_thumbnailSize, m_thumbnailSize));
            ImGui::PopStyleColor(3);
        }
        else
        {
            // Placeholder: teal tint for library items
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.2f, 0.4f, 0.5f, 0.5f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.3f, 0.5f, 0.6f, 0.6f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.4f, 0.6f, 0.7f, 0.7f));
            ImGui::Button(reinterpret_cast<const char *>(ICON_FA_FILE_IMPORT),
                          ImVec2(m_thumbnailSize, m_thumbnailSize));
            ImGui::PopStyleColor(3);
        }

        // File name
        renderFileName(info.fileName, cellWidth, itemPos);

        // Library function names (if any)
        if (info.hasLibraryMetadata && !info.libraryFunctionNames.empty())
        {
            std::string funcLabel;
            for (size_t i = 0; i < info.libraryFunctionNames.size(); ++i)
            {
                if (i > 0)
                {
                    funcLabel += ", ";
                }
                funcLabel += info.libraryFunctionNames[i];
            }
            if (funcLabel.size() > 30)
            {
                funcLabel = funcLabel.substr(0, 27) + "...";
            }

            float const funcWidth = ImGui::CalcTextSize(funcLabel.c_str()).x;
            float const funcX = itemPos.x + (cellWidth - funcWidth) * 0.5f;
            ImGui::SetCursorPos(ImVec2(funcX, ImGui::GetCursorPosY()));
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
            ImGui::TextUnformatted(funcLabel.c_str());
            ImGui::PopStyleColor();
        }

        ImGui::EndGroup();
    }

    void ThreemfFileViewer::renderFileName(const std::string & fileName,
                                           float cellWidth,
                                           const ImVec2 & itemPos)
    {
        float const textY = itemPos.y + m_thumbnailSize + 15.0f;
        ImVec2 textSize = ImGui::CalcTextSize(fileName.c_str());

        if (textSize.x > cellWidth - 10.0f)
        {
            std::string truncated = fileName.substr(0, 20) + "...";
            textSize = ImGui::CalcTextSize(truncated.c_str());
            ImGui::SetCursorPos(ImVec2(itemPos.x + (cellWidth - textSize.x) * 0.5f, textY));
            ImGui::TextUnformatted(truncated.c_str());
        }
        else
        {
            ImGui::SetCursorPos(ImVec2(itemPos.x + (cellWidth - textSize.x) * 0.5f, textY));
            ImGui::TextUnformatted(fileName.c_str());
        }
    }

} // namespace gladius::ui
