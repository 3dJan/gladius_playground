#include "Outline.h"
#include "ImageStackResource.h"
#include "nodes/Builder.h"
#include "ResourceManager.h"
#include "Widgets.h"
#include "imgui.h"
#include "nodes/BuildItem.h"
#include "nodes/Components.h"
#include "nodes/Object.h"

#include <fmt/format.h>

namespace gladius::ui
{
    void Outline::setDocument(SharedDocument document)
    {
        m_document = std::move(document);
    }

    bool Outline::render() const
    {
        if (!m_document)
        {
            return false;
        }

        ImGuiTreeNodeFlags const baseFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                             ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                             ImGuiTreeNodeFlags_SpanAvailWidth;

        bool propertiesChanged = false;

        // Metadata section
        ImGui::BeginGroup();

        if (ImGui::TreeNodeEx("Metadata", baseFlags))
        {
            // Render the metadata view
            MetaDataView metaDataView;
            if (metaDataView.render(m_document))
            {
                // If metadata was modified, mark the document as changed
                propertiesChanged = true;
            }
            ImGui::TreePop();
        }
        ImGui::EndGroup();
        frameOverlay(ImVec4(0.9f, 0.6f, 0.3f, 0.1f),
                     "Document Information\n\n"
                     "Add title, author, and other details about your design here.\n"
                     "This information helps identify your model when sharing with others or\n"
                     "when sending to manufacturing services.");

        ImGui::BeginGroup();
        // Build Items section
        if (ImGui::TreeNodeEx("Build Items", baseFlags | ImGuiTreeNodeFlags_DefaultOpen))
        {
            // Replace direct rendering with BuildItemView
            BuildItemView buildItemView;
            if (buildItemView.render(m_document))
            {
                // If build items were modified, mark the document as changed
                propertiesChanged = true;
            }
            ImGui::TreePop();
        }

        ImGui::EndGroup();
        frameOverlay(ImVec4(1.0f, 0.9f, 0.6f, 0.1f),
                     "Objects to Manufacture\n\n"
                     "This section shows the parts that will be sent to the printer.\n"
                     "You can:\n"
                     " Add new objects to your build\n"
                     " Position and rotate parts\n"
                     " Combine multiple objects in your design\n"
                     " Arrange items for optimal printing");

        // Image Stacks section
        if (m_document && m_document->getCore())
        {
            auto & resourceManager = m_document->getGeneratorContext().resourceManager;
            auto const & resources = resourceManager.getResourceMap();

            // Count ImageStack resources
            size_t imageStackCount = 0;
            for (auto const & [key, res] : resources)
            {
                if (dynamic_cast<ImageStackResource const *>(res.get()))
                {
                    ++imageStackCount;
                }
            }

            if (imageStackCount > 0)
            {
                ImGui::BeginGroup();
                if (ImGui::TreeNodeEx("Image Stacks", baseFlags))
                {
                    ImGuiTreeNodeFlags const leafFlags =
                      ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_SpanAvailWidth;

                    for (auto const & [key, res] : resources)
                    {
                        auto const * stack = dynamic_cast<ImageStackResource const *>(res.get());
                        if (!stack)
                        {
                            continue;
                        }

                        auto const displayName = fmt::format("{} ({}x{}x{})",
                                                             key.getDisplayName(),
                                                             stack->getWidth(),
                                                             stack->getHeight(),
                                                             stack->getNumSheets());

                        if (ImGui::TreeNodeEx(displayName.c_str(), leafFlags))
                        {
                            ImGui::TreePop();
                        }

                        // T062: Context menu for ImageStack items
                        if (ImGui::BeginPopupContextItem())
                        {
                            if (ImGui::MenuItem("Create FunctionFromImage3D"))
                            {
                                // T063/T064: Create function with default settings
                                auto imageStackId = key.getResourceId();
                                if (imageStackId.has_value() && m_document && m_document->getCore())
                                {
                                    auto model3mf = m_document->get3mfModel();
                                    if (model3mf)
                                    {
                                        // Create FunctionFromImage3D via Lib3MF API
                                        auto funcPkg = model3mf->AddFunctionFromImage3D(nullptr);
                                        if (funcPkg)
                                        {
                                            // Get the underlying ImageStack from 3MF
                                            auto imageStack =
                                              model3mf->GetImageStackByID(imageStackId.value());
                                            if (imageStack)
                                            {
                                                funcPkg->SetImage3D(imageStack.get());
                                            }

                                            // T064: Set default values (Linear filter, Repeat
                                            // tiling)
                                            funcPkg->SetFilter(Lib3MF::eTextureFilter::Linear);
                                            funcPkg->SetTileStyles(
                                              Lib3MF::eTextureTileStyle::Wrap,
                                              Lib3MF::eTextureTileStyle::Wrap,
                                              Lib3MF::eTextureTileStyle::Wrap);
                                            funcPkg->SetOffset(0.0);
                                            funcPkg->SetScale(1.0);

                                            // Update the document model
                                            m_document->update3mfModel();
                                            m_document->markFileAsChanged();

                                            // T065: Note - selection is handled by ModelEditor
                                            // when it detects new functions during refresh
                                        }
                                    }
                                }
                            }
                            ImGui::EndPopup();
                        }
                    }
                    ImGui::TreePop();
                }
                ImGui::EndGroup();
                frameOverlay(ImVec4(1.0f, 0.65f, 0.0f, 0.1f),
                             "Image Stacks\n\n"
                             "3D image data used for volumetric operations.\n"
                             "Select an image stack to view and edit its layers.");
            }
        }

        return propertiesChanged;
    }

} // namespace gladius::ui