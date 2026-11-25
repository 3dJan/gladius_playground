// Attachment handling for the Design Agent composer: clipboard and file imports, the thumbnail strip
// inside the prompt card and its enlarge/comment modal.

#include "AgentPanels.h"
#include "ClipboardImageHelper.h"
#include "../Document.h"

#include <IconsFontAwesome5.h>
#include <imgui.h>
#include <tinyfiledialogs/tinyfiledialogs.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace gladius::ui
{
    void AgentPanels::renderImageControls(float buttonSize)
    {
        if (ImGui::Button(reinterpret_cast<char const *>(ICON_FA_IMAGE),
                          ImVec2(buttonSize, buttonSize)))
        {
            char const * filters[] = {"*.png", "*.jpg", "*.jpeg"};
            char const * selected =
              tinyfd_openFileDialog("Attach Reference Image", "", 3, filters, "Image files", 0);
            if (selected != nullptr)
            {
                (void) attachImageFile(selected);
            }
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Attach a reference image to the next prompt");
        }
        ImGui::SameLine();
        if (ImGui::Button(reinterpret_cast<char const *>(ICON_FA_PASTE),
                          ImVec2(buttonSize, buttonSize)))
        {
            attachClipboardImage();
        }
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("Attach the clipboard image (Ctrl/Cmd+V in the prompt field)");
        }
    }

    void AgentPanels::attachClipboardImage()
    {
        auto clip = ClipboardImageHelper::readClipboardImage();
        if (!clip.success || clip.pngBytes.empty())
        {
            return;
        }
        std::string const key = "att_" + std::to_string(m_nextAttachmentId++);
        m_pendingImages.push_back(PendingImageAttachment{
          std::move(clip.filename), std::move(clip.pngBytes), std::move(clip.mimeType), "", key});
    }

    void AgentPanels::insertClipboardOrFileImageIntoDocument()
    {
        if (m_document == nullptr)
        {
            return;
        }
        char const * filters[] = {"*.png", "*.jpg", "*.jpeg"};
        char const * selected =
          tinyfd_openFileDialog("Insert Image Into 3MF Document", "", 3, filters, "Image files", 0);
        if (selected == nullptr)
        {
            return;
        }
        try
        {
            (void) m_document->addImageStackResource(selected);
            m_document->markFileAsChanged();
            if (m_onDocumentModified)
            {
                m_onDocumentModified();
            }
        }
        catch (std::exception const & ex)
        {
            m_lastError = std::string("Failed to insert image: ") + ex.what();
        }
    }

    void AgentPanels::renderPendingAttachments()
    {
        for (std::size_t i = 0; i < m_pendingImages.size(); ++i)
        {
            if (i > 0)
            {
                ImGui::SameLine();
            }
            ImGui::PushID(static_cast<int>(i));
            auto & att = m_pendingImages[i];
            if (att.previewKey.empty())
            {
                att.previewKey = "att_" + std::to_string(m_nextAttachmentId++);
            }
            int w = 0;
            int h = 0;
            unsigned int const texId = getOrCreatePreviewTexture(att.previewKey, att.bytes, w, h);
            float const thumb = m_prompt.config().attachmentThumbSize;
            if (texId != 0u)
            {
                if (ImGui::ImageButton("##thumb",
                                       static_cast<ImTextureID>(static_cast<std::uintptr_t>(texId)),
                                       ImVec2(thumb, thumb)))
                {
                    m_inspectedImageIndex = static_cast<int>(i);
                    ImGui::OpenPopup("AttachedImageModal");
                }
                if (ImGui::IsItemHovered())
                {
                    ImGui::SetTooltip("%s\nClick to enlarge & add comment%s",
                                      att.filename.c_str(),
                                      att.comment.empty()
                                        ? ""
                                        : ("\nComment: " + att.comment).c_str());
                }
            }
            else if (ImGui::SmallButton(att.filename.c_str()))
            {
                m_inspectedImageIndex = static_cast<int>(i);
                ImGui::OpenPopup("AttachedImageModal");
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("x"))
            {
                if (m_inspectedImageIndex == static_cast<int>(i))
                {
                    m_inspectedImageIndex = -1;
                }
                m_pendingImages.erase(m_pendingImages.begin() + static_cast<long>(i));
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }

        if (ImGui::BeginPopup("AttachedImageModal"))
        {
            if (m_inspectedImageIndex >= 0
                && static_cast<std::size_t>(m_inspectedImageIndex) < m_pendingImages.size())
            {
                auto & att = m_pendingImages[static_cast<std::size_t>(m_inspectedImageIndex)];
                ImGui::Text("File: %s (%zu bytes, %s)",
                            att.filename.c_str(),
                            att.bytes.size(),
                            att.mimeType.c_str());
                int w = 0;
                int h = 0;
                unsigned int const texId = getOrCreatePreviewTexture(att.previewKey, att.bytes, w, h);
                if (texId != 0u && w > 0 && h > 0)
                {
                    float const maxDim = 280.f;
                    float const scale = std::min(maxDim / static_cast<float>(w),
                                                 maxDim / static_cast<float>(h));
                    ImGui::Image(static_cast<ImTextureID>(static_cast<std::uintptr_t>(texId)),
                                 ImVec2(static_cast<float>(w) * scale,
                                        static_cast<float>(h) * scale));
                }
                char commentBuf[512];
                std::snprintf(commentBuf, sizeof(commentBuf), "%s", att.comment.c_str());
                if (ImGui::InputText("Comment / Feedback", commentBuf, sizeof(commentBuf)))
                {
                    att.comment = commentBuf;
                }
                if (ImGui::Button("Close"))
                {
                    ImGui::CloseCurrentPopup();
                }
            }
            else
            {
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }
    }

} // namespace gladius::ui
