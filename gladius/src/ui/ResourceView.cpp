#include "ResourceView.h"
#include "io/3mf/ResourceDependencyGraph.h"
#include "io/3mf/ResourceIdUtil.h"
#include "nodes/Builder.h"

#include "FileChooser.h"
#include "ImageStackResource.h"
#include "ImageStackView.h"
#include "MeshResource.h"
#include "ModelEditor.h"
#include "ResourceManager.h"
#include "Widgets.h"

#include "imgui.h"
#include <imgui_stdlib.h>      // For InputText with std::string
#include <lib3mf_implicit.hpp> // For VolumeData interfaces

namespace gladius::ui
{

    // Implementation of renderVolumeDataDropdown to show available VolumeData resources for a mesh
    bool ResourceView::renderVolumeDataDropdown(SharedDocument document,
                                                Lib3MF::PModel model3mf,
                                                std::shared_ptr<Lib3MF::CMeshObject> mesh) const
    {
        bool propertiesChanged = false;

        ImGui::PushID("VolumeDataDropdown");

        // Get current VolumeData for this mesh if it exists
        Lib3MF::PVolumeData currentVolumeData;
        std::string currentVolumeDataName = "None";

        try
        {
            currentVolumeData = mesh->GetVolumeData();
            if (currentVolumeData)
            {
                currentVolumeDataName =
                  fmt::format("VolumeData #{}", currentVolumeData->GetResourceID());
            }
        }
        catch (...)
        {
            // Handle errors silently - no VolumeData exists
        }

        if (ImGui::BeginCombo("##VolumeData", currentVolumeDataName.c_str()))
        {
            // Add "None" option to remove VolumeData
            bool isSelected = !currentVolumeData;
            if (ImGui::Selectable("None", isSelected))
            {
                try
                {
                    document->update3mfModel();
                    // Set VolumeData to nullptr to remove the association
                    mesh->SetVolumeData(nullptr);
                    document->markFileAsChanged();
                    propertiesChanged = true;
                }
                catch (...)
                {
                    // Handle errors silently
                }
            }

            if (isSelected)
            {
                ImGui::SetItemDefaultFocus();
            }

            // List all available VolumeData resources
            auto resourceIterator = model3mf->GetResources();
            while (resourceIterator->MoveNext())
            {
                auto resource = resourceIterator->GetCurrent();
                if (!resource)
                {
                    continue;
                }

                auto volumeData = std::dynamic_pointer_cast<Lib3MF::CVolumeData>(resource);
                if (!volumeData)
                {
                    continue;
                }

                auto name = fmt::format("VolumeData #{}", volumeData->GetResourceID());
                isSelected = currentVolumeData &&
                             (currentVolumeData->GetResourceID() == volumeData->GetResourceID());

                if (ImGui::Selectable(name.c_str(), isSelected))
                {
                    try
                    {
                        document->update3mfModel();
                        mesh->SetVolumeData(volumeData);
                        document->markFileAsChanged();
                        propertiesChanged = true;
                    }
                    catch (const std::exception & e)
                    {
                        if (document->getSharedLogger())
                        {
                            document->getSharedLogger()->addEvent(
                              {fmt::format("Failed to set VolumeData: {}", e.what()),
                               events::Severity::Error});
                        }
                    }
                }

                if (isSelected)
                {
                    ImGui::SetItemDefaultFocus();
                }
            }

            ImGui::EndCombo();
        }
        ImGui::PopID();

        return propertiesChanged;
    }

    void ResourceView::render(SharedDocument document)
    {
        if (!document)
        {
            return;
        }
        if (!document->getCore())
        {
            return;
        }

        // Check if export is in progress - disable modifications
        bool const exportInProgress = m_exportState != nullptr && m_exportState->isExportInProgress();
        
        auto & resourceManager = document->getGeneratorContext().resourceManager;

        auto const & resources = resourceManager.getResourceMap();

        ImGuiTreeNodeFlags const baseFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                             ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                             ImGuiTreeNodeFlags_SpanAvailWidth;

        ImGuiTreeNodeFlags nodeFlags = baseFlags | ImGuiTreeNodeFlags_Leaf;
        ImGuiTreeNodeFlags infoNodeFlags = ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_DefaultOpen;

        ImGui::BeginGroup();
        if (ImGui::TreeNodeEx("Mesh Resources", baseFlags | ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent();
            ImGui::BeginDisabled(exportInProgress);
            if (ImGui::Button("Import STL"))
            {
                addMesh(document);
            }

            if (ImGui::Button("Add current bounding box"))
            {
                document->addBoundingBoxAsMesh();
            }

            if (ImGui::Button("Add custom box..."))
            {
                m_showCustomBoxDialog = true;
            }
            ImGui::EndDisabled();

            ImGui::Unindent();

            // Custom box creation dialog
            if (m_showCustomBoxDialog)
            {
                ImVec2 const center(ImGui::GetIO().DisplaySize.x * 0.5f,
                                    ImGui::GetIO().DisplaySize.y * 0.5f);
                ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                ImGui::OpenPopup("Create Custom Box");

                if (ImGui::BeginPopupModal("Create Custom Box",
                                           &m_showCustomBoxDialog,
                                           ImGuiWindowFlags_AlwaysAutoResize))
                {
                    ImGui::Text("Box Dimensions:");
                    ImGui::Separator();

                    ImGui::InputFloat("Width", &m_boxWidth, 1.0f, 10.0f, "%.2f");
                    ImGui::InputFloat("Height", &m_boxHeight, 1.0f, 10.0f, "%.2f");
                    ImGui::InputFloat("Depth", &m_boxDepth, 1.0f, 10.0f, "%.2f");

                    ImGui::Spacing();
                    ImGui::Text("Starting Position:");
                    ImGui::Separator();

                    ImGui::InputFloat("Start X", &m_boxStartX, 1.0f, 10.0f, "%.2f");
                    ImGui::InputFloat("Start Y", &m_boxStartY, 1.0f, 10.0f, "%.2f");
                    ImGui::InputFloat("Start Z", &m_boxStartZ, 1.0f, 10.0f, "%.2f");

                    ImGui::Spacing();
                    ImGui::Separator();

                    if (ImGui::Button("Create", ImVec2(120, 0)))
                    {
                        document->addCustomBoxMesh(m_boxWidth,
                                                   m_boxHeight,
                                                   m_boxDepth,
                                                   m_boxStartX,
                                                   m_boxStartY,
                                                   m_boxStartZ);
                        m_showCustomBoxDialog = false;
                        ImGui::CloseCurrentPopup();
                    }
                    ImGui::SetItemDefaultFocus();
                    ImGui::SameLine();
                    if (ImGui::Button("Cancel", ImVec2(120, 0)))
                    {
                        m_showCustomBoxDialog = false;
                        ImGui::CloseCurrentPopup();
                    }

                    ImGui::EndPopup();
                }
            }

            for (auto const & [key, res] : resources)
            {
                if (!res)
                {
                    continue;
                }
                auto const * mesh = dynamic_cast<MeshResource const *>(res.get());
                if (!mesh)
                {
                    continue;
                }

                auto name =
                  fmt::format("{} #{}", key.getDisplayName(), key.getResourceId().value_or(-1));
                ImGui::BeginGroup();
                if (ImGui::TreeNodeEx(name.c_str(), baseFlags))
                {
                    auto const & meshData = mesh->getMesh();

                    if (ImGui::BeginTable(
                          "MeshData", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
                    {
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted("Faces");
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(fmt::format("{}", meshData.polygonCount()).c_str());

                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted("Min");
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(fmt::format("({}, {}, {})",
                                                           meshData.getMin().x,
                                                           meshData.getMin().y,
                                                           meshData.getMin().z)
                                                 .c_str());
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted("Max");
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted(fmt::format("({}, {}, {})",
                                                           meshData.getMax().x,
                                                           meshData.getMax().y,
                                                           meshData.getMax().z)
                                                 .c_str());

                        // Add Part Number field
                        ImGui::TableNextColumn();
                        ImGui::TextUnformatted("Part Number:");
                        ImGui::TableNextColumn();
                        try
                        {
                            auto model3mf = document->get3mfModel();
                            if (model3mf && key.getResourceId().has_value())
                            {
                                auto lib3mfUniqueResourceId =
                                  gladius::io::resourceIdToUniqueResourceId(
                                    model3mf, key.getResourceId().value());

                                auto resource = model3mf->GetResourceByID(lib3mfUniqueResourceId);
                                if (resource)
                                {
                                    // Convert resource to Object since only Objects have part
                                    // numbers
                                    auto object =
                                      std::dynamic_pointer_cast<Lib3MF::CObject>(resource);
                                    if (object)
                                    {
                                        std::string partNumber = object->GetPartNumber();
                                        if (ImGui::InputText("##PartNumber",
                                                             &partNumber,
                                                             ImGuiInputTextFlags_None))
                                        {
                                            try
                                            {
                                                document->update3mfModel();
                                                object->SetPartNumber(partNumber);
                                                document->markFileAsChanged();
                                            }
                                            catch (...)
                                            {
                                                // Handle errors silently
                                            }
                                        }
                                    }
                                }
                            }

                            // Add dropdown for selecting a volume data
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted("Volume Data:");
                            ImGui::TableNextColumn();
                            try
                            {
                                auto model3mf = document->get3mfModel();
                                if (model3mf && key.getResourceId().has_value())
                                {
                                    auto lib3mfUniqueResourceId =
                                      gladius::io::resourceIdToUniqueResourceId(
                                        model3mf, key.getResourceId().value());

                                    auto resource =
                                      model3mf->GetResourceByID(lib3mfUniqueResourceId);
                                    if (resource)
                                    {
                                        auto meshObject =
                                          std::dynamic_pointer_cast<Lib3MF::CMeshObject>(resource);
                                        if (meshObject)
                                        {
                                            renderVolumeDataDropdown(
                                              document, model3mf, meshObject);
                                        }
                                    }
                                }
                            }

                            catch (...)
                            {
                                // Handle errors silently
                            }
                        }
                        catch (...)
                        {
                            // Handle errors silently
                        }

                        ImGui::EndTable();
                    }

                    // always show delete button, but indicate dependencies
                    auto safeResult = document->isItSafeToDeleteResource(key);
                    if (ImGui::Button("Delete"))
                    {
                        if (safeResult.canBeRemoved)
                        {
                            document->deleteResource(key);
                        }
                    }

                    if (!safeResult.canBeRemoved)
                    {
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::BeginTooltip();
                            ImGui::TextColored(
                              ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                              "Cannot delete, the resource is referenced by another item:");
                            for (auto const & depRes : safeResult.dependentResources)
                            {
                                ImGui::BulletText("Resource ID: %u", depRes->GetModelResourceID());
                            }
                            for (auto const & depItem : safeResult.dependentBuildItems)
                            {
                                ImGui::BulletText("Build item: %u", depItem->GetObjectResourceID());
                            }
                            ImGui::EndTooltip();
                        }
                    }

                    ImGui::TreePop();
                }
                ImGui::EndGroup();
                frameOverlay(ImVec4(1.0f, 1.0f, 1.0f, 0.2f),
                             "Mesh Resource Details\n\n"
                             "View vertices, triangles, and properties of this mesh.\n"
                             "Meshes define the shape of objects using triangular surfaces.");
            }
            ImGui::TreePop();
        }

        ImGui::EndGroup();
        frameOverlay(ImVec4(0.5f, 1.0f, 0.5f, 0.1f),
                     "Mesh Resources\n\n"
                     "Traditional 3D models made of triangles.\n"
                     "Meshes define the surface of your objects using connected triangles\n"
                     "and can include properties like color and texture.");

        // Process async file dialog results
        processAsyncFileDialog(document);

        bool const dialogActive = m_asyncFileDialog.isActive();

        // image stack
        ImGui::BeginGroup();
        if (ImGui::TreeNodeEx("Image Stacks", baseFlags | ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::Indent();
            // import image stack
            ImGui::BeginDisabled(dialogActive);
            if (ImGui::Button("Import from directory"))
            {
                m_asyncDialogOp = ResourceViewDialogOp::ImportImageStack;
                m_asyncFileDialog.selectDirectory();
            }
            ImGui::EndDisabled();

            ImGui::Unindent();

            for (auto const & [key, res] : resources)
            {
                auto const * stack = dynamic_cast<ImageStackResource const *>(res.get());
                auto const * grid = dynamic_cast<VdbResource const *>(res.get());
                if (!stack && !grid)
                {
                    continue;
                }

                ImGui::BeginGroup();
                if (ImGui::TreeNodeEx(key.getDisplayName().c_str(), baseFlags))
                {
                    (ImGui::TextUnformatted(fmt::format("# {} loaded as {}",
                                                        key.getResourceId().value_or(-1),
                                                        stack ? "image stack" : "vdb grid")
                                              .c_str()));

                    if (grid)
                    {
                        auto dimensions = grid->getGridSize();
                        if (ImGui::TreeNodeEx(
                              fmt::format(
                                "Size: {}x{}x{}", dimensions.x, dimensions.y, dimensions.z)
                                .c_str(),
                              infoNodeFlags))
                        {
                            ImGui::TreePop();
                        }
                    }

                    // Render ImageStackView for image stacks
                    if (stack)
                    {
                        auto imgStack = stack->getImageStack();
                        if (imgStack && !imgStack->empty())
                        {
                            // Get or create the view for this resource
                            auto & view = m_imageStackViews[key];
                            view.setImageStack(imgStack);

                            // T059/T060/T061: Set up transform callback with undo support
                            view.setTransformCallback(
                                [this, document, key, &view](ImageStackTransform transform)
                                {
                                    // Get mutable ImageStack
                                    auto & resourceManager =
                                        document->getGeneratorContext().resourceManager;
                                    auto const & resources = resourceManager.getResourceMap();

                                    io::ImageStack * mutableStack = nullptr;
                                    for (auto & [resKey, res] : resources)
                                    {
                                        if (resKey == key)
                                        {
                                            auto * stackRes =
                                                dynamic_cast<ImageStackResource *>(res.get());
                                            if (stackRes)
                                            {
                                                mutableStack = stackRes->getImageStack();
                                                break;
                                            }
                                        }
                                    }

                                    if (!mutableStack)
                                    {
                                        return;
                                    }

                                    // T060: Create undo restore point
                                    if (m_modelEditor)
                                    {
                                        switch (transform)
                                        {
                                        case ImageStackTransform::FlipHorizontal:
                                            m_modelEditor->createUndoRestorePoint(
                                                "Flip ImageStack Horizontal");
                                            break;
                                        case ImageStackTransform::FlipVertical:
                                            m_modelEditor->createUndoRestorePoint(
                                                "Flip ImageStack Vertical");
                                            break;
                                        case ImageStackTransform::Rotate90CW:
                                            m_modelEditor->createUndoRestorePoint(
                                                "Rotate ImageStack 90\xC2\xB0 CW");
                                            break;
                                        case ImageStackTransform::Rotate90CCW:
                                            m_modelEditor->createUndoRestorePoint(
                                                "Rotate ImageStack 90\xC2\xB0 CCW");
                                            break;
                                        }
                                    }

                                    // Apply the transform
                                    switch (transform)
                                    {
                                    case ImageStackTransform::FlipHorizontal:
                                        mutableStack->flipHorizontal();
                                        break;
                                    case ImageStackTransform::FlipVertical:
                                        mutableStack->flipVertical();
                                        break;
                                    case ImageStackTransform::Rotate90CW:
                                        mutableStack->rotate90CW();
                                        break;
                                    case ImageStackTransform::Rotate90CCW:
                                        mutableStack->rotate90CCW();
                                        break;
                                    }

                                    // Mark model as modified
                                    if (m_modelEditor)
                                    {
                                        m_modelEditor->markModelAsModified();
                                    }

                                    // T061: Refresh layer texture
                                    view.invalidateTexture();
                                });

                            ImGui::Separator();
                            ImGui::Text("Layer Preview:");
                            view.render();
                        }
                    }

                    // Add Part Number field for image resources
                    if (ImGui::TreeNodeEx("Properties", infoNodeFlags))
                    {
                        if (ImGui::BeginTable("ResourceProperties",
                                              2,
                                              ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg))
                        {
                            ImGui::TableNextColumn();
                            ImGui::TextUnformatted("Part Number:");
                            ImGui::TableNextColumn();
                            try
                            {
                                auto model3mf = document->get3mfModel();
                                if (model3mf && key.getResourceId().has_value())
                                {
                                    auto lib3mfUniqueResourceId =
                                      gladius::io::resourceIdToUniqueResourceId(
                                        model3mf, key.getResourceId().value());

                                    auto resource =
                                      model3mf->GetResourceByID(lib3mfUniqueResourceId);
                                    if (resource)
                                    {
                                        // Convert resource to Object since only Objects have part
                                        // numbers
                                        auto object =
                                          std::dynamic_pointer_cast<Lib3MF::CObject>(resource);
                                        if (object)
                                        {
                                            std::string partNumber = object->GetPartNumber();
                                            if (ImGui::InputText("##ImgPartNumber",
                                                                 &partNumber,
                                                                 ImGuiInputTextFlags_None))
                                            {
                                                try
                                                {
                                                    document->update3mfModel();
                                                    object->SetPartNumber(partNumber);
                                                    document->markFileAsChanged();
                                                }
                                                catch (...)
                                                {
                                                    // Handle errors silently
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            catch (...)
                            {
                                // Handle errors silently
                            }
                            ImGui::EndTable();
                        }
                        ImGui::TreePop();
                    }

                    // delete image stack
                    auto safeResult = document->isItSafeToDeleteResource(key);

                    // Create FunctionFromImage3D button
                    ImGui::BeginDisabled(exportInProgress);
                    if (ImGui::Button("Create Function"))
                    {
                        auto resourceId = key.getResourceId();
                        if (resourceId.has_value())
                        {
                            m_pendingImageStackId = resourceId.value();
                            m_newFunctionName = key.getDisplayName();
                            m_showCreateFunctionDialog = true;
                            ImGui::OpenPopup("Create FunctionFromImage3D");
                        }
                    }
                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
                    {
                        ImGui::SetTooltip(
                            "Create a FunctionFromImage3D that samples this ImageStack");
                    }

                    // Create FunctionFromImage3D dialog
                    if (ImGui::BeginPopupModal("Create FunctionFromImage3D", &m_showCreateFunctionDialog,
                                               ImGuiWindowFlags_AlwaysAutoResize))
                    {
                        ImGui::Text("Enter a name for the new function:");
                        ImGui::InputText("##FunctionName", &m_newFunctionName);

                        if (ImGui::Button("Create", ImVec2(120, 0)))
                        {
                            if (m_pendingImageStackId.has_value())
                            {
                                auto model3mf = document->get3mfModel();
                                if (model3mf)
                                {
                                    try
                                    {
                                        auto uniqueResId = gladius::io::resourceIdToUniqueResourceId(
                                            model3mf, m_pendingImageStackId.value());
                                        auto imageStack =
                                            model3mf->GetImageStackByID(uniqueResId);
                                        if (imageStack)
                                        {
                                            auto func =
                                                model3mf->AddFunctionFromImage3D(imageStack.get());
                                            if (func)
                                            {
                                                func->SetFilter(Lib3MF::eTextureFilter::Linear);
                                                func->SetTileStyles(Lib3MF::eTextureTileStyle::Wrap,
                                                                    Lib3MF::eTextureTileStyle::Wrap,
                                                                    Lib3MF::eTextureTileStyle::Wrap);
                                                func->SetOffset(0.0);
                                                func->SetScale(1.0);

                                                // Register the function in the assembly
                                                nodes::Builder builder;
                                                nodes::SamplingSettings settings;
                                                builder.createFunctionFromImage3D(
                                                    *document->getAssembly(),
                                                    func->GetModelResourceID(),
                                                    m_pendingImageStackId.value(),
                                                    settings);

                                                // Set the user-provided name
                                                auto function = document->getAssembly()->findModel(
                                                    func->GetModelResourceID());
                                                if (function)
                                                {
                                                    function->setDisplayName(m_newFunctionName);
                                                }

                                                document->update3mfModel();
                                                document->markFileAsChanged();

                                                // Refresh ModelEditor to show the new function
                                                if (m_modelEditor)
                                                {
                                                    m_modelEditor->refreshAssembly();
                                                }
                                            }
                                        }
                                    }
                                    catch (std::exception const & e)
                                    {
                                        auto logger = document->getSharedLogger();
                                        if (logger)
                                        {
                                            logger->addEvent(
                                                {fmt::format("Failed to create FunctionFromImage3D: {}",
                                                             e.what()),
                                                 events::Severity::Error});
                                        }
                                    }
                                }
                            }
                            m_showCreateFunctionDialog = false;
                            m_pendingImageStackId.reset();
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel", ImVec2(120, 0)))
                        {
                            m_showCreateFunctionDialog = false;
                            m_pendingImageStackId.reset();
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                    }

                    ImGui::SameLine();
                    if (ImGui::Button("Delete"))
                    {
                        if (safeResult.canBeRemoved)
                        {
                            document->deleteResource(key);
                        }
                    }

                    if (!safeResult.canBeRemoved)
                    {
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::BeginTooltip();
                            ImGui::TextColored(
                              ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                              "Cannot delete, the resource is referenced by another item:");
                            for (auto const & depRes : safeResult.dependentResources)
                            {
                                ImGui::BulletText("Resource ID: %u", depRes->GetModelResourceID());
                            }
                            for (auto const & depItem : safeResult.dependentBuildItems)
                            {
                                ImGui::BulletText("Build item: %u", depItem->GetObjectResourceID());
                            }
                            ImGui::EndTooltip();
                        }
                    }

                    ImGui::TreePop();
                }
                ImGui::EndGroup();
                frameOverlay(ImVec4(1.0f, 1.0f, 1.0f, 0.2f),
                             "Image Stack Details\n\n"
                             "View and edit the 3D image data used in volumetric models.\n"
                             "These stacked images create a full 3D representation.");
            }
            ImGui::TreePop();
        }
        ImGui::EndGroup();
        frameOverlay(ImVec4(1.0f, 0.65f, 0.0f, 0.1f),
                     "Image Stacks\n\n"
                     "3D image data for volumetric models.\n"
                     "Image stacks store information as voxels (3D pixels) and allow you to\n"
                     "represent object properties that vary throughout the volume.");
    }

    void ResourceView::addMesh(SharedDocument document)
    {
        if (!document || m_asyncFileDialog.isActive())
        {
            return;
        }

        m_asyncDialogOp = ResourceViewDialogOp::AddMesh;
        m_asyncFileDialog.openFile({{"*.stl"}});
    }

    void ResourceView::processAsyncFileDialog(SharedDocument document)
    {
        auto result = m_asyncFileDialog.checkResult();
        if (!result)
        {
            return; // No result yet
        }

        auto const operation = m_asyncDialogOp;
        m_asyncDialogOp = ResourceViewDialogOp::None;

        if (!result->has_value())
        {
            return; // User cancelled
        }

        std::filesystem::path const path = result->value();

        switch (operation)
        {
        case ResourceViewDialogOp::ImportImageStack:
            if (document)
            {
                auto importResult = document->addImageStackResourceWithPadding(path);

                // Show notification if any images were padded
                if (importResult.hasPaddedFiles())
                {
                    auto logger = document->getSharedLogger();
                    if (logger)
                    {
                        std::string message = fmt::format(
                            "ImageStack imported with padding to {}x{}. Padded {} file(s): ",
                            importResult.maxWidth,
                            importResult.maxHeight,
                            importResult.paddedFiles.size());

                        size_t const maxFilesToShow = 5;
                        for (size_t i = 0;
                             i < std::min(importResult.paddedFiles.size(), maxFilesToShow);
                             ++i)
                        {
                            if (i > 0)
                            {
                                message += ", ";
                            }
                            message += importResult.paddedFiles[i];
                        }
                        if (importResult.paddedFiles.size() > maxFilesToShow)
                        {
                            message += fmt::format(
                                " and {} more", importResult.paddedFiles.size() - maxFilesToShow);
                        }

                        logger->addEvent({message, events::Severity::Info});
                    }
                }
            }
            break;
        case ResourceViewDialogOp::AddMesh:
            if (document)
            {
                document->addMeshResource(path);
            }
            break;
        case ResourceViewDialogOp::None:
            break;
        }
    }
}