#include <exception>
#include <imgui_internal.h>
#include <imgui_stdlib.h>
#include <imguinodeeditor.h>

#include <algorithm>
#include <cctype>
#include <fmt/format.h>
#include <limits>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "../ExpressionParser.h"
#include "../ExpressionToGraphConverter.h"
#include "../FunctionArgument.h"

#include "../CLMath.h"
#include "../IconFontCppHeaders/IconsFontAwesome5.h"
#include "BeamLatticeView.h"
#include "ComponentsObjectView.h"
#include "Document.h"
#include "MeshResource.h"
#include "ModelEditor.h"
#include "NodeLayoutEngine.h"
#include "NodeView.h"
#include "Port.h"
#include "ResourceView.h"
#include "Style.h"
#include "ValidationUtils.h"
#include "Widgets.h"
#include "imgui.h"
#include "nodesfwd.h"
#include "ui/LevelSetView.h"
#include "ui/VolumeDataView.h"
#include <nodes/Assembly.h>
#include <nodes/Model.h>

namespace gladius::ui
{
    using namespace nodes;

    // Layout and positioning constants
    namespace
    {
        /// Horizontal offset when placing a new node relative to a selected node
        constexpr float kNodePlacementOffsetX = 400.0f;
        /// Default fallback node size when actual size is unavailable
        constexpr ImVec2 kDefaultNodeSize{500.0f, 400.0f};
        /// Width for input text fields in dialogs
        constexpr float kInputTextWidth = 260.0f;
        /// Standard button width in dialogs
        constexpr float kDialogButtonWidth = 120.0f;
        /// Standard dialog child window height for scrollable lists
        constexpr float kDialogListHeight = 150.0f;
        /// Standard dialog child window width
        constexpr float kDialogListWidth = 500.0f;
        /// Filter input width in popups
        constexpr float kFilterInputWidth = 200.0f;
    } // namespace

    std::vector<ModelEditor::LayoutStrategyDescriptor> ModelEditor::layoutStrategyDescriptors()
    {
        using Descriptor = LayoutStrategyDescriptor;
        using Choice = LayoutStrategyChoice;

        return {Descriptor{Choice::OptimizedLayeredMedian, "OptimizedLayered Median"},
                Descriptor{Choice::BalancedGridCompact, "BalancedGrid Compact"},
                Descriptor{Choice::MedianSweepTightY, "MedianSweep TightY"},
                Descriptor{Choice::LayeredStackClassic, "LayeredStack Classic"},
                Descriptor{Choice::LayeredRowSweep, "LayeredRow Sweep"},
                Descriptor{Choice::ForceRefinedHybrid, "ForceRefined Hybrid"}};
    }

    const char * ModelEditor::layoutStrategyLabel(LayoutStrategyChoice choice)
    {
        switch (choice)
        {
            case LayoutStrategyChoice::Auto:
                return "Auto (tournament)";
            case LayoutStrategyChoice::OptimizedLayeredMedian:
                return "OptimizedLayered Median";
            case LayoutStrategyChoice::BalancedGridCompact:
                return "BalancedGrid Compact";
            case LayoutStrategyChoice::MedianSweepTightY:
                return "MedianSweep TightY";
            case LayoutStrategyChoice::LayeredStackClassic:
                return "LayeredStack Classic";
            case LayoutStrategyChoice::LayeredRowSweep:
                return "LayeredRow Sweep";
            case LayoutStrategyChoice::ForceRefinedHybrid:
                return "ForceRefined Hybrid";
        }

        return "Unknown";
    }

    NodeLayoutEngine::LayoutStrategy
    ModelEditor::makeLayoutStrategy(LayoutStrategyChoice choice)
    {
        switch (choice)
        {
            case LayoutStrategyChoice::OptimizedLayeredMedian:
            {
                return NodeLayoutEngine::LayoutStrategy{"OptimizedLayered Median",
                                                        NodeLayoutEngine::GroupLayoutMode::VerticalStack,
                                                        0.95F,  // nodeDistanceScale
                                                        1.05F,  // layerSpacingScale
                                                        180,    // maxOptimizationIterations
                                                        0.8F,   // convergenceScale
                                                        true,   // enableExtraRelaxation
                                                        2,      // extraRelaxationPasses
                                                        true,   // enableGroupCompaction
                                                        true};  // alignToOrigin
            }
            case LayoutStrategyChoice::BalancedGridCompact:
            {
                return NodeLayoutEngine::LayoutStrategy{"BalancedGrid Compact",
                                                        NodeLayoutEngine::GroupLayoutMode::BalancedGrid,
                                                        1.0F,   // nodeDistanceScale
                                                        0.95F,  // layerSpacingScale
                                                        140,    // maxOptimizationIterations
                                                        0.85F,  // convergenceScale
                                                        true,   // enableExtraRelaxation
                                                        2,      // extraRelaxationPasses
                                                        true,   // enableGroupCompaction
                                                        true};  // alignToOrigin
            }
            case LayoutStrategyChoice::MedianSweepTightY:
            {
                return NodeLayoutEngine::LayoutStrategy{"MedianSweep TightY",
                                                        NodeLayoutEngine::GroupLayoutMode::VerticalStack,
                                                        1.0F,   // nodeDistanceScale
                                                        1.0F,   // layerSpacingScale
                                                        140,    // maxOptimizationIterations
                                                        0.8F,   // convergenceScale
                                                        true,   // enableExtraRelaxation
                                                        3,      // extraRelaxationPasses
                                                        true,   // enableGroupCompaction
                                                        true};  // alignToOrigin
            }
            case LayoutStrategyChoice::LayeredStackClassic:
            {
                NodeLayoutEngine::LayoutStrategy strategy{};
                strategy.name = "LayeredStack Classic";
                return strategy;
            }
            case LayoutStrategyChoice::LayeredRowSweep:
            {
                return NodeLayoutEngine::LayoutStrategy{"LayeredRow Sweep",
                                                        NodeLayoutEngine::GroupLayoutMode::HorizontalRow,
                                                        1.0F,   // nodeDistanceScale
                                                        1.1F,   // layerSpacingScale
                                                        120,    // maxOptimizationIterations
                                                        0.9F,   // convergenceScale
                                                        true,   // enableExtraRelaxation
                                                        1,      // extraRelaxationPasses
                                                        true,   // enableGroupCompaction
                                                        true};  // alignToOrigin
            }
            case LayoutStrategyChoice::ForceRefinedHybrid:
            {
                return NodeLayoutEngine::LayoutStrategy{"ForceRefined Hybrid",
                                                        NodeLayoutEngine::GroupLayoutMode::VerticalStack,
                                                        1.0F,   // nodeDistanceScale
                                                        1.05F,  // layerSpacingScale
                                                        160,    // maxOptimizationIterations
                                                        0.7F,   // convergenceScale
                                                        true,   // enableExtraRelaxation
                                                        3,      // extraRelaxationPasses
                                                        true,   // enableGroupCompaction
                                                        true};  // alignToOrigin
            }
            case LayoutStrategyChoice::Auto:
                break;
        }

        return NodeLayoutEngine::LayoutStrategy{"Unknown"};
    }

    ModelEditor::ModelEditor()
    {
        m_editorContext = ed::CreateEditor();
        m_nodeTypeToColor = createNodeTypeToColors();

        // Setup expression dialog callbacks
        m_expressionDialog.setOnApplyCallback(
          [this](std::string const & functionName,
                 std::string const & expression,
                 std::vector<FunctionArgument> const & arguments,
                 FunctionOutput const & output)
          { onCreateFunctionFromExpression(functionName, expression, arguments, output); });

        m_expressionDialog.setOnPreviewCallback(
          [this](std::string const & expression)
          {
              // TODO: Preview the expression (maybe show variable values or graph structure)
              // For now, this is a placeholder
          });
    }

    ModelEditor::~ModelEditor()
    {
        DestroyEditor(m_editorContext);
    }

    void ModelEditor::resetEditorContext()
    {
        if (m_editorContext != nullptr)
        {
            DestroyEditor(m_editorContext);
            m_editorContext = nullptr;
        }
        m_editorContext = ed::CreateEditor();
    }

    void ModelEditor::outline()
    {
        ImGui::Begin("Outline", nullptr, ImGuiWindowFlags_MenuBar);

        if (!m_doc || !m_currentModel || !m_assembly)
        {
            ImGui::End();
            return;
        }

        if (ImGui::BeginMenuBar())
        {
            if (ImGui::MenuItem(
                  reinterpret_cast<const char *>(ICON_FA_TRASH "\tDelete unused resources")))
            {
                m_unusedResources = m_doc->findUnusedResources();

                if (!m_unusedResources.empty())
                {
                    m_showDeleteUnusedResourcesConfirmation = true;
                }
                else if (auto logger = m_doc->getSharedLogger())
                {
                    logger->addEvent(
                      {"No unused resources found in the model", events::Severity::Info});
                }
            }
            ImGui::EndMenuBar();
        }

        if (m_outline.render())
        {
            markModelAsModified();
        }

        ImGuiTreeNodeFlags const baseFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                             ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                             ImGuiTreeNodeFlags_SpanAvailWidth;

        ImGui::BeginGroup();
        if (ImGui::TreeNodeEx("Resources", baseFlags | ImGuiTreeNodeFlags_DefaultOpen))
        {
            ImGui::BeginGroup();
            if (ImGui::TreeNodeEx("ComponentsObjects", baseFlags | ImGuiTreeNodeFlags_DefaultOpen))
            {
                ComponentsObjectView componentsObjectView;
                if (componentsObjectView.render(m_doc))
                {
                    markModelAsModified();
                }
                ImGui::TreePop();
            }
            ImGui::EndGroup();
            frameOverlay(ImVec4(0.0f, 0.8f, 0.8f, 0.1f),
                         "Components Objects\n\n"
                         "Design elements that form your model's structure and features.\n"
                         "Use components to reuse repetitive features and to\n"
                         "composite complex parts.\n");

            ImGui::BeginGroup();
            if (ImGui::TreeNodeEx("VolumeData", baseFlags | ImGuiTreeNodeFlags_DefaultOpen))
            {
                VolumeDataView volumeDataView;
                if (volumeDataView.render(m_doc))
                {
                    markModelAsModified();
                }
                ImGui::TreePop();
            }
            ImGui::EndGroup();
            frameOverlay(
              ImVec4(1.0f, 0.0f, 1.0f, 0.1f),
              "Volume Data\n\n"
              "Define spatially varying properties inside your 3D models.\n"
              "Volume data lets you specify how material properties change throughout\n"
              "the interior of an object, not just on its surface.\n\n"
              "Common uses:\n"
              " Gradual color transitions and material blending\n"
              " Variable density or infill structures\n"
              " Physical properties like elasticity or conductivity\n"
              " Temperature or stress distribution for simulation\n\n"
              "Apply volume data to meshes or level sets using functions with \"pos\" input\n"
              "and appropriate scalar (custom property) or vector (color) outputs for your desired "
              "property.");

            ImGui::BeginGroup();
            if (ImGui::TreeNodeEx("LevelSet", baseFlags | ImGuiTreeNodeFlags_DefaultOpen))
            {
                LevelSetView levelSetView;
                if (levelSetView.render(m_doc))
                {
                    markModelAsModified();
                }
                ImGui::TreePop();
            }
            ImGui::EndGroup();
            frameOverlay(
              ImVec4(1.0f, 1.0f, 0.0f, 0.1f),
              "Level Sets\n\n"
              "Define your 3D shape using mathematical boundaries instead of triangles.\n"
              "Level sets are perfect for creating smooth, organic shapes and\n"
              "allow for easier mixing between different shapes.\n\n"
              "For a level set you need a function with a \"pos\" vector as input and a scalar "
              "output.\n");

            ImGui::BeginGroup();
            if (ImGui::TreeNodeEx("Beam Lattices", baseFlags | ImGuiTreeNodeFlags_DefaultOpen))
            {
                if (m_beamLatticeView.render(m_doc))
                {
                    markModelAsModified();
                }
                ImGui::TreePop();
            }
            ImGui::EndGroup();
            frameOverlay(ImVec4(0.8f, 0.4f, 1.0f, 0.1f),
                         "Beam Lattices\n\n"
                         "Complex structural geometries made of interconnected beams and nodes.\n"
                         "Beam lattices are ideal for lightweight structures, supports,\n"
                         "and metamaterials with specific mechanical properties.\n\n"
                         "Import beam lattices from 3MF files with embedded beam lattice data,\n"
                         "or create them programmatically using beam and ball primitive data.\n");

            resourceOutline();

            ImGui::BeginGroup();
            if (ImGui::TreeNodeEx("Functions", baseFlags | ImGuiTreeNodeFlags_DefaultOpen))
            {
                functionOutline();
                ImGui::TreePop();
            }
            ImGui::EndGroup();
            frameOverlay(ImVec4(0.0f, 0.5f, 1.0f, 0.1f),
                         "Functions\n\n"
                         "These are the building blocks for creating implicit surfaces.\n"
                         "Think of them as tools that let you combine basic shapes like\n"
                         "spheres and cubes into more complex models.\n\n"
                         "You can reference functions in a Level Set to define a geometry\n"
                         "or in a Volume data to define the inner properties of your model.\n");

            ImGui::TreePop();
        }
        ImGui::EndGroup();
        frameOverlay(
          ImVec4(0.5f, 0.5f, 0.5f, 0.1f),
          "Outline\n\n"
          "Explore resources and functions that make up the 3MF document.\n"
          "Use these sections to inspect meshes, volume data, level sets, beam lattices,\n"
          "and procedural functions available in the current model.");

        ImGui::End();
    }

    void ModelEditor::resourceOutline()
    {
        m_resourceView.render(m_doc);
    }

    void ModelEditor::functionOutline()
    {
        ImGuiTreeNodeFlags const baseFlags = ImGuiTreeNodeFlags_OpenOnArrow |
                                             ImGuiTreeNodeFlags_OpenOnDoubleClick |
                                             ImGuiTreeNodeFlags_SpanAvailWidth;

        ImGui::Indent();

        if (ImGui::Button(reinterpret_cast<const char *>(ICON_FA_PLUS "\tAdd function")))
        {
            ImGui::OpenPopup("Add Function");
            m_showAddModel = true;
        }

        ImGui::SameLine();
        if (ImGui::Button(reinterpret_cast<const char *>(ICON_FA_CALCULATOR "\tExpression")))
        {
            showExpressionDialog();
        }

        ImGui::Unindent();

        for (auto & model : m_assembly->getFunctions())
        {
            if (!model.second)
            {
                continue;
            }

            auto const isAssembly =
              model.second->getResourceId() == m_assembly->assemblyModel()->getResourceId();

            // if (isAssembly)
            // {
            //     continue;
            // }

            auto & modelName = model.first;
            auto uid = &modelName;
            ImGui::PushID(uid);

            bool const isModelSelected =
              m_currentModel->getResourceId() == model.second->getResourceId();

            if (m_outlineNodeColorLines)
            {
                int i = 0;
                for (auto & node : *model.second)
                {
                    auto & nodeRef = *node.second;
                    auto const colorIter = m_nodeTypeToColor.find(typeid(nodeRef));
                    if (colorIter != std::end(m_nodeTypeToColor))
                    {
                        auto const color = colorIter->second;

                        auto * window = ImGui::GetCurrentWindow();
                        auto const start = ImVec2(i * 2.f, ImGui::GetCursorScreenPos().y);
                        auto const end =
                          ImVec2(start.x + 2.f, start.y + ImGui::GetTextLineHeightWithSpacing());
                        window->DrawList->AddRectFilled(start, end, ImColor(color));
                    }
                    ++i;
                }
            }

            auto modelDisplayName = model.second->getDisplayName();

            std::string const asssemblyLabel = fmt::format("internal graph from builditems");
            std::string const nodeLabel = (isAssembly)
                                            ? asssemblyLabel
                                            : fmt::format("{} #{}",
                                                          modelDisplayName.value_or("function"),
                                                          model.second->getResourceId());
            std::string editableName = nodeLabel;

            if (!model.second->isValid())
            {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 0.f, 0.f, 1.f));
            }

            ImGui::BeginGroup();

            bool nodeOpen =
              ImGui::TreeNodeEx("",
                                baseFlags | (isModelSelected ? ImGuiTreeNodeFlags_Selected : 0),
                                "%s",
                                nodeLabel.c_str());

            if (!model.second->isValid())
            {
                ImGui::PopStyleColor();
            }

            if (ImGui::IsItemClicked())
            {
                readBackNodePositions();
                // Use history-aware navigation
                navigateToFunction(model.second->getResourceId());
                m_nodePositionsNeedUpdate = true;
            }

            if (nodeOpen)
            {
                for (auto & [nodeId, node] : *model.second)
                {
                    auto & nodeRef = *node;
                    auto const colorIter = m_nodeTypeToColor.find(typeid(nodeRef));
                    if (colorIter != std::end(m_nodeTypeToColor))
                    {
                        auto const color = colorIter->second;

                        auto * window = ImGui::GetCurrentWindow();
                        auto const start = ImVec2(ImGui::GetCursorScreenPos().x + 10.f,
                                                  ImGui::GetCursorScreenPos().y);
                        auto const end =
                          ImVec2(start.x + 5.f, start.y + ImGui::GetTextLineHeightWithSpacing());
                        window->DrawList->AddRectFilled(start, end, ImColor(color));
                    }

                    ImGuiTreeNodeFlags nodeFlags = baseFlags | ImGuiTreeNodeFlags_Leaf;
                    if (isNodeSelected(nodeId) && isModelSelected)
                    {
                        nodeFlags |= ImGuiTreeNodeFlags_Selected;
                    }

                    bool const isLeafOpen =
                      ImGui::TreeNodeEx(node->getDisplayName().c_str(), nodeFlags);
                    if (ImGui::IsItemClicked())
                    {
                        navigateToFunction(model.second->getResourceId());
                        m_nodePositionsNeedUpdate = true;
                        ed::SelectNode(nodeId);
                        ed::NavigateToSelection(true);
                    }
                    if (isLeafOpen)
                    {
                        ImGui::TreePop();
                    }
                }

                if (!isAssembly && !model.second->isManaged())
                {
                    // Check if function can be safely deleted
                    auto safeResult =
                      m_doc->isItSafeToDeleteResource(ResourceKey(model.second->getResourceId()));
                    if (ImGui::Button("Delete"))
                    {
                        if (safeResult.canBeRemoved)
                        {
                            m_doc->deleteFunction(model.second->getResourceId());
                            m_currentModel = m_assembly->assemblyModel();
                            m_dirty = true;
                        }
                    }

                    // Display tooltip with dependency information if function cannot be deleted
                    if (!safeResult.canBeRemoved)
                    {
                        if (ImGui::IsItemHovered())
                        {
                            ImGui::BeginTooltip();
                            ImGui::TextColored(
                              ImVec4(1.0f, 0.0f, 0.0f, 1.0f),
                              "Cannot delete, the function is referenced by another item:");
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

                    ImGui::SameLine();
                    if (ImGui::Button("Rename"))
                    {
                        m_outlineRenaming = true;
                        ImGui::SetKeyboardFocusHere();
                        ImGui::OpenPopup("Rename");
                        m_newModelName = model.second->getDisplayName().value_or("New function");
                    }

                    if (ImGui::BeginPopup("Rename"))
                    {
                        ImGui::InputText("New Name", &m_newModelName);
                        if (ImGui::Button("Confirm"))
                        {
                            model.second->setDisplayName(m_newModelName);
                            m_outlineRenaming = false;
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::SameLine();
                        if (ImGui::Button("Cancel"))
                        {
                            m_outlineRenaming = false;
                            ImGui::CloseCurrentPopup();
                        }
                        ImGui::EndPopup();
                    }
                }

                ImGui::TreePop();
            }

            ImGui::EndGroup();
            frameOverlay(ImVec4(1.0f, 1.0f, 1.0f, isModelSelected ? 0.2f : 0.1f));

            ImGui::PopID();
        }
    }

    void ModelEditor::newModelDialog()
    {
        if (m_showAddModel)
        {
            ImVec2 const center(ImGui::GetIO().DisplaySize.x * 0.5f,
                                ImGui::GetIO().DisplaySize.y * 0.5f);
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::OpenPopup("Add Function");
            if (ImGui::BeginPopupModal(
                  "Add Function", &m_showAddModel, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("Create a new function");
                ImGui::Separator();

                // Function name input
                ImGui::InputText("Function name", &m_newModelName);

                // Check for duplicate name
                bool nameExists = false;
                for (auto & [id, model] : m_assembly->getFunctions())
                {
                    if (model && model->getDisplayName().has_value() &&
                        model->getDisplayName().value() == m_newModelName)
                    {
                        nameExists = true;
                        break;
                    }
                }
                if (nameExists)
                {
                    ImGui::Spacing();
                    ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.0f, 1.0f),
                                       "Warning: This name is already used for another function.");
                }

                // Function type selection
                static const char * functionTypes[] = {"Empty function",
                                                       "Copy existing function",
                                                       "Levelset template",
                                                       "Wrap existing function"};
                int functionType = static_cast<int>(m_selectedFunctionType);
                ImGui::Combo(
                  "Function type", &functionType, functionTypes, IM_ARRAYSIZE(functionTypes));
                m_selectedFunctionType = static_cast<FunctionType>(functionType);

                // If copy or wrap, show list of available functions
                int availableFunctionCount = 0;
                std::vector<nodes::Model *> availableFunctions;
                std::vector<std::string> availableFunctionNames;
                if (m_selectedFunctionType == FunctionType::CopyExisting ||
                    m_selectedFunctionType == FunctionType::WrapExisting)
                {
                    for (auto & [id, model] : m_assembly->getFunctions())
                    {
                        if (!model || model->isManaged() || model == m_currentModel)
                            continue;
                        availableFunctions.push_back(model.get());
                        availableFunctionNames.push_back(
                          model->getDisplayName().value_or("function"));
                    }
                    availableFunctionCount = static_cast<int>(availableFunctions.size());
                    if (availableFunctionCount == 0)
                    {
                        ImGui::TextColored(ImVec4(1, 0, 0, 1),
                                           "No user functions available to copy.");
                    }
                    else
                    {
                        if (m_selectedSourceFunctionIndex >= availableFunctionCount)
                            m_selectedSourceFunctionIndex = 0;
                        std::vector<const char *> cstrNames;
                        for (auto & s : availableFunctionNames)
                            cstrNames.push_back(s.c_str());
                        ImGui::Combo("Source function",
                                     &m_selectedSourceFunctionIndex,
                                     cstrNames.data(),
                                     availableFunctionCount);
                    }
                }

                bool canCreate = !m_newModelName.empty() &&
                                 ((m_selectedFunctionType != FunctionType::CopyExisting &&
                                   m_selectedFunctionType != FunctionType::WrapExisting) ||
                                  availableFunctionCount > 0);

                if (canCreate && ImGui::Button("Create", ImVec2(kDialogButtonWidth, 0)))
                {
                    nodes::Model * newModel = nullptr;
                    switch (m_selectedFunctionType)
                    {
                    case FunctionType::Empty:
                    default:
                        newModel = &m_doc->createNewFunction();
                        break;
                    case FunctionType::CopyExisting:
                        if (availableFunctionCount > 0)
                            newModel = &m_doc->copyFunction(
                              *availableFunctions[m_selectedSourceFunctionIndex], m_newModelName);
                        break;
                    case FunctionType::WrapExisting:
                        if (availableFunctionCount > 0)
                            newModel = &m_doc->wrapExistingFunction(
                              *availableFunctions[m_selectedSourceFunctionIndex], m_newModelName);
                        break;
                    case FunctionType::LevelsetTemplate:
                        newModel = &m_doc->createLevelsetFunction(m_newModelName);
                        break;
                    }
                    if (newModel)
                    {
                        newModel->setDisplayName(m_newModelName);
                        // Refresh assembly reference from document - it may have been replaced
                        setAssembly(m_doc->getAssembly());
                        m_currentModel = m_assembly->findModel(newModel->getResourceId());
                        switchModel();
                        m_showAddModel = false;
                        m_visible = true; // Ensure ModelEditor remains visible after creating function
                        ImGui::CloseCurrentPopup();
                    }
                }
                ImGui::SetItemDefaultFocus();
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(kDialogButtonWidth, 0)))
                {
                    ImGui::CloseCurrentPopup();
                    m_showAddModel = false;
                }
                ImGui::EndPopup();
            }
        }
    }

    bool ModelEditor::isNodeSelected(nodes::NodeId nodeId)
    {
        // Query selection state from the node editor
        return ed::IsNodeSelected(ed::NodeId(static_cast<uint64_t>(nodeId)));
    }

    void ModelEditor::onCreateNode()
    {
        // Block node creation during export
        if (m_exportState != nullptr && m_exportState->isExportInProgress())
        {
            return;
        }
        
        // Re-introduce the node-editor creation block so the node-editor can
        // query new-node/link gestures. The implementation was unintentionally
        // removed in a refactor — this restores the behavior that opens the
        // "Create Node" popup on right-click in the editor background and
        // ensures pin-based new-node queries are handled.
        if (ed::BeginCreate())
        {
            ed::PinId inputPinId, outputPinId;
            if (ed::QueryNewLink(&inputPinId, &outputPinId))
            {
                // A new link is being created between two pins.
                // Attempt to add the link if both pins are valid.
                if (inputPinId && outputPinId)
                {
                    auto const inId = static_cast<nodes::ParameterId>(inputPinId.Get());
                    auto const outId = static_cast<nodes::PortId>(outputPinId.Get());

                    if (ed::AcceptNewItem())
                    {
                        createUndoRestorePoint("Add link");
                        if (!m_currentModel->addLink(inId, outId) &&
                            !m_currentModel->addLink(outId, inId))
                        {
                            ed::RejectNewItem();
                        }
                        else
                        {
                            markModelAsModified();
                        }
                    }
                }
            }

            // Delegate pin-based "create node while dragging from a pin"
            // to the dedicated helper that opens the Create Node popup when
            // appropriate.
            onQueryNewNode();
        }
        ed::EndCreate();

        // Handle background (canvas) context menu. This is what allows the
        // user to right-click on empty space in the node editor and choose
        // a node to create.
        if (ed::ShowBackgroundContextMenu())
        {
            // Suspend editor input to safely query ImGui mouse position,
            // then resume editor interaction.
            ed::Suspend();
            auto const currentMousePos = ImGui::GetMousePos();
            ed::Resume();

            showPopupMenu([&, currentMousePos]() { createNodePopup(-1, currentMousePos); });
            m_showCreateNodePopUp = true;
            ImGui::OpenPopup("Create Node");
        }
    }

    void ModelEditor::onDeleteNode()
    {
        // Block node deletion during export
        if (m_exportState != nullptr && m_exportState->isExportInProgress())
        {
            return;
        }
        
        if (ed::BeginDelete())
        {
            ed::NodeId deletedNodeId = 0;
            while (ed::QueryDeletedNode(&deletedNodeId))
            {
                if (ed::AcceptDeletedItem())
                {
                    m_currentModel->remove(static_cast<nodes::NodeId>(deletedNodeId.Get()));
                    markModelAsModified();
                }
            }
        }
        ed::EndDelete();
    }

    void ModelEditor::switchModel()
    {
        // Mark the editor to re-apply node positions and refresh view on next frame
        m_nodePositionsNeedUpdate = true;
        m_dirty = true;
        // Defer selection clearing until an editor is active to avoid calling NodeEditor APIs out
        // of context
        m_pendingClearSelection = true;

        // Schedule an initial auto-layout for models that have no meaningful positions yet,
        // but only once per function to preserve user edits.
        if (m_currentModel)
        {
            m_pendingAutoLayout = !m_currentModel->hasBeenLayouted();
        }
        else
        {
            m_pendingAutoLayout = false;
        }
    }

    void ModelEditor::onQueryNewNode()
    {
        ed::PinId pinId{0};
        if (ed::QueryNewNode(&pinId))
        {
            if (ed::AcceptNewItem())
            {
                auto const portId = static_cast<nodes::PortId>(pinId.Get());
                auto const currentMousePos = ImGui::GetMousePos();
                showPopupMenu([&, portId, currentMousePos]()
                              { createNodePopup(portId, currentMousePos); });
                m_showCreateNodePopUp = true;
                ImGui::OpenPopup("Create Node");
            }
        }
    }

    void ModelEditor::createNodePopup(nodes::PortId srcPortId, ImVec2 mousePos)
    {
        if (m_showCreateNodePopUp)
        {
            ImGui::OpenPopup("Create Node");
            m_showCreateNodePopUp = false;
            // Clear filter text when opening popup
            m_nodeFilterText.clear();
        }

        if (m_currentModel == nullptr)
        {
            throw std::runtime_error("ModelEditor: No model selected");
        }

        static nodes::NodeTypes nodeTypes;
        auto srcPortIter = m_currentModel->getPortRegistry().find(srcPortId);
        bool showOnlyLinkableNodes = true;
        if (srcPortIter == std::end(m_currentModel->getPortRegistry()))
        {
            showOnlyLinkableNodes = false;
        }

        if (ImGui::BeginPopup("Create Node"))
        {
            // Add filter text box at the top of the popup
            ImGui::TextUnformatted(ICON_FA_SEARCH);
            ImGui::SameLine();
            ImGui::PushItemWidth(200.0f * m_uiScale);

            // Auto-focus on the filter input when popup opens
            bool isFirstFrame = ImGui::IsWindowAppearing();

            // Check if any key is pressed and focus the filter input
            bool needsFocus = isFirstFrame;
            auto & io = ImGui::GetIO();
            bool isAnyKeyTyped = io.InputQueueCharacters.Size > 0;

            // Check if backspace is pressed (Backspace isn't in InputQueueCharacters)
            bool isBackspacePressed = io.KeysDown[ImGui::GetKeyIndex(ImGuiKey_Backspace)];

            // Check if a key was pressed and the filter input doesn't already have focus
            if ((isAnyKeyTyped || isBackspacePressed) && !ImGui::IsItemActive())
            {
                needsFocus = true;

                // If any character was typed, update the filter text with it
                if (!isFirstFrame)
                {
                    if (isBackspacePressed)
                    {
                        // Clear filter text on backspace
                        m_nodeFilterText.clear();
                    }
                    else
                    {
                        // Set filter text to the first typed character
                        for (int i = 0; i < io.InputQueueCharacters.Size; i++)
                        {
                            char c = (char) io.InputQueueCharacters[i];
                            if (c >= 32)
                            { // Ignore control characters
                                m_nodeFilterText = c;
                                break; // Only use the first typed character
                            }
                        }
                    }
                }
            }

            if (needsFocus)
            {
                ImGui::SetKeyboardFocusHere();
            }

            if (ImGui::InputText(
                  "##NodeFilter", &m_nodeFilterText, ImGuiInputTextFlags_AutoSelectAll))
            {
                // Filter text changed
            }
            ImGui::PopItemWidth();
            ImGui::Separator();

            // Filter function and mesh resources using the filter text
            functionToolBox(mousePos);
            meshResourceToolBox(mousePos);
            beamLatticeResourceToolBox(mousePos);

            for (auto & [cat, catName] : nodes::CategoryNames)
            {
                if (cat == nodes::Category::Internal)
                {
                    continue;
                }
                auto const category = cat;
                auto const styleIter = NodeColors.find(category);
                if (styleIter != std::end(NodeColors))
                {
                    auto const style = styleIter->second;

                    ImGui::PushStyleColor(ImGuiCol_Button, ImU32(style.color));
                    ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImU32(style.activeColor));
                    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImU32(style.hoveredColor));
                    ImGui::PushStyleColor(ImGuiCol_Header, ImU32(style.color));
                    ImGui::PushStyleColor(ImGuiCol_HeaderActive, ImU32(style.activeColor));
                    ImGui::PushStyleColor(ImGuiCol_HeaderHovered, ImU32(style.hoveredColor));
                    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.f, 1.f, 1.f, 1.f));
                }
                auto requiredFieldName =
                  (showOnlyLinkableNodes) ? srcPortIter->second->getShortName() : std::string{};

                staticFor(
                  nodeTypes,
                  [&](auto, auto & node)
                  {
                      auto targetParameterIter = node.parameter().find(requiredFieldName);
                      bool hasRequiredField = targetParameterIter != std::end(node.parameter());

                      pushNodeColor(node);
                      // Check if node matches filter
                      std::string nodeName = node.name();
                      bool matchesFilter = matchesNodeFilter(nodeName);

                      if (matchesFilter && node.getCategory() == category &&
                          (hasRequiredField || !showOnlyLinkableNodes))
                      {
                          if (ImGui::Button(nodeName.c_str()))
                          {
                              createUndoRestorePoint("Create node");
                              auto createdNode = m_currentModel->create(node);
                              auto posOnCanvas = ed::ScreenToCanvas(mousePos);
                              ed::SetNodePosition(createdNode->getId(), posOnCanvas);
                              if (showOnlyLinkableNodes)
                              {
                                  m_currentModel->addLink(
                                    srcPortId, createdNode->parameter()[requiredFieldName].getId());
                              }

                              // Request focus on the newly created node for keyboard-driven
                              // workflow
                              requestNodeFocus(createdNode->getId());

                              markModelAsModified();
                              closePopupMenu();
                          }
                      }
                      popNodeColor(node);
                  });

                if (styleIter != std::end(NodeColors))
                {
                    ImGui::PopStyleColor(7);
                }
            }
            ImGui::EndPopup();
        }
    }

    void ModelEditor::invalidateEverything()
    {
        markModelAsModified();
        m_parameterDirty = true;
        m_dirty = true;
        m_nodePositionsNeedUpdate = true;
    }

    auto ModelEditor::showAndEdit() -> bool
    {
        m_uiScale = ImGui::GetIO().FontGlobalScale * 2.0f;
        if (!m_currentModel || !m_assembly)
        {
            return false;
        }

        bool parameterChanged = false;

        outline();
        newModelDialog();
        showDeleteUnusedResourcesDialog();
        try
        {

            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0., 0.));
            if (ImGui::Begin("Model Editor", &m_visible, ImGuiWindowFlags_MenuBar))
            {
                SetCurrentEditor(m_editorContext);

                if (ImGui::BeginMenuBar())
                {
                    // Function extraction refactoring
                    {
                        auto selection = selectedNodes(m_editorContext);
                        bool canExtract = !selection.empty();
                        if (!canExtract)
                        {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.5f));
                            ImGui::MenuItem(reinterpret_cast<const char *>(ICON_FA_CODE_BRANCH
                                                                           "\tExtract Function"));
                            ImGui::PopStyleColor();
                        }
                        else
                        {
                            if (ImGui::MenuItem(reinterpret_cast<const char *>(
                                  ICON_FA_CODE_BRANCH "\tExtract Function")))
                            {
                                m_showExtractDialog = true;
                                m_extractFunctionName = "ExtractedFunction";
                            }
                        }
                    }

                    ImGui::SameLine();

                    if (ImGui::MenuItem("Autolayout"))
                    {
                        autoLayout();
                    }
                    if (ImGui::MenuItem(reinterpret_cast<const char *>(ICON_FA_COMPRESS_ARROWS_ALT
                                                                       "\tCenter View")))
                    {
                        ed::NavigateToContent();
                    }

                    // Block Undo/Redo/Copy/Paste during export
                    bool const exportLocked = m_exportState && m_exportState->isExportInProgress();
                    if (exportLocked)
                    {
                        ImGui::BeginDisabled();
                    }

                    m_stateApplyingUndo = false;
                    if (m_history.canUnDo())
                    {
                        if (ImGui::MenuItem(reinterpret_cast<const char *>(ICON_FA_UNDO "\tUndo")))
                        {
                            undo();
                        }
                    }
                    else
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.5f));
                        ImGui::MenuItem(reinterpret_cast<const char *>(ICON_FA_UNDO "\tUndo"));
                        ImGui::PopStyleColor();
                    }

                    if (m_history.canReDo())
                    {
                        if (ImGui::MenuItem(reinterpret_cast<const char *>(ICON_FA_REDO "\tRedo")))
                        {
                            redo();
                        }
                    }
                    else
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.5f));
                        ImGui::MenuItem(reinterpret_cast<const char *>(ICON_FA_REDO "\tRedo"));
                        ImGui::PopStyleColor();
                    }

                    // Copy / Paste
                    auto selectionForCopy = selectedNodes(m_editorContext);
                    if (selectionForCopy.empty())
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.5f));
                        ImGui::MenuItem(reinterpret_cast<const char *>(ICON_FA_COPY "\tCopy"));
                        ImGui::PopStyleColor();
                    }
                    else
                    {
                        if (ImGui::MenuItem(reinterpret_cast<const char *>(ICON_FA_COPY "\tCopy")))
                        {
                            copySelectionToClipboard();
                        }
                    }

                    if (!hasClipboard())
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.5f));
                        ImGui::MenuItem(reinterpret_cast<const char *>(ICON_FA_PASTE "\tPaste"));
                        ImGui::PopStyleColor();
                    }
                    else
                    {
                        if (ImGui::MenuItem(
                              reinterpret_cast<const char *>(ICON_FA_PASTE "\tPaste")))
                        {
                            // Defer paste until editor is active
                            m_pendingPasteRequest = true;
                        }
                    }

                    if (exportLocked)
                    {
                        ImGui::EndDisabled();
                    }

                    toggleButton(
                      {reinterpret_cast<const char *>(ICON_FA_ROBOT "\tCompile automatically")},
                      &m_autoCompile);

                    if (!m_autoCompile)
                    {
                        if (ImGui::MenuItem(
                              reinterpret_cast<const char *>(ICON_FA_HAMMER "\tCompile")))
                        {
                            m_isManualCompileRequested = true;
                        }
                    }

                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted("Compile the model");
                        ImGui::Separator();
                        ImGui::TextUnformatted(
                          "If this option is enabled, the model will be compiled automatically "
                          "when it is modified.\n"
                          "If this option is disabled, you have to compile the model manually.");
                        ImGui::EndTooltip();
                    }

                    auto core = m_doc->getCore();
                    bool optimized = core->getCodeGenerator() == CodeGenerator::Code;
                    auto optimizedNewState = optimized;

                    // The command stream is currently not working
                    // toggleButton(
                    //   {reinterpret_cast<const char *>(ICON_FA_STOPWATCH "\tJIT optimized")},
                    //   &optimizedNewState);

                    if (optimizedNewState != optimized)
                    {
                        core->setCodeGenerator((optimizedNewState) ? CodeGenerator::Code
                                                                   : CodeGenerator::CommandStream);
                        invalidateEverything();
                    }

                    // automatic update of the bounding box
                    bool autoUpdateBoundingBox = core->isAutoUpdateBoundingBoxEnabled();
                    toggleButton(
                      {reinterpret_cast<const char *>(ICON_FA_BOXES "\tAuto update bounding box")},
                      &autoUpdateBoundingBox);
                    // Tooltip for auto update bounding box
                    if (ImGui::IsItemHovered())
                    {
                        ImGui::BeginTooltip();
                        ImGui::TextUnformatted("Auto update bounding box");
                        ImGui::Separator();
                        ImGui::TextUnformatted(
                          "If enabled, the bounding box will be updated automatically when the "
                          "model is modified.\n"
                          "Deactivate this option to speed up the preview of parameter changes.");
                        ImGui::EndTooltip();
                    }
                    core->setAutoUpdateBoundingBox(autoUpdateBoundingBox);

                    if (!autoUpdateBoundingBox)
                    {
                        if (ImGui::MenuItem("Update bounding box"))
                        {
                            core->resetBoundingBox();
                            core->updateBBox();
                            invalidateEverything();
                        }
                    }

                    bool showResourceNodes = m_nodeViewVisitor.areResourceNodesVisible();
                    toggleButton(
                      {reinterpret_cast<const char *>(ICON_FA_DATABASE "\tResource Nodes")},
                      &showResourceNodes);
                    m_nodeViewVisitor.setResourceNodesVisible(showResourceNodes);

                    // Add group assignment functionality
                    auto selection = selectedNodes(m_editorContext);
                    if (!selection.empty())
                    {
                        if (ImGui::MenuItem(
                              reinterpret_cast<const char *>(ICON_FA_TAGS "\tAdd to Group")))
                        {
                            // TODO: Implement group assignment
                            m_showGroupAssignmentDialog = true;
                        }

                        if (ImGui::IsItemHovered())
                        {
                            ImGui::BeginTooltip();
                            ImGui::TextUnformatted("Assign selected nodes to a group/tag");
                            ImGui::Separator();
                            ImGui::TextUnformatted(
                              fmt::format("Selected nodes: {}", selection.size()).c_str());
                            ImGui::EndTooltip();
                        }
                    }
                    else
                    {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 0.5f, 0.5f, 0.5f));
                        ImGui::MenuItem(
                          reinterpret_cast<const char *>(ICON_FA_TAGS "\tAdd to Group"));
                        ImGui::PopStyleColor();

                        if (ImGui::IsItemHovered())
                        {
                            ImGui::BeginTooltip();
                            ImGui::TextUnformatted("Select nodes to assign them to a group");
                            ImGui::EndTooltip();
                        }
                    }

                    ImGui::EndMenuBar();
                }

                m_popupMenuFunction();

                ed::SetCurrentEditor(m_editorContext);

                ed::PushStyleColor(ax::NodeEditor::StyleColor_Bg,
                                   ImGui::GetStyleColorVec4(ImGuiCol_FrameBg));

                ed::Begin("Model Editor");

                // Clear selection now that the editor context is active
                if (m_pendingClearSelection)
                {
                    ed::ClearSelection();
                    m_pendingClearSelection = false;
                }

                // Handle any deferred paste request once editor is active
                if (m_pendingPasteRequest)
                {
                    m_pendingPasteRequest = false;
                    pasteClipboardAtMouse();
                }

                m_nodeViewVisitor.setAssembly(m_assembly);
                m_nodeViewVisitor.setModelEditor(this);
                m_nodeViewVisitor.setExportState(m_exportState);
                if (m_currentModel)
                {
                    m_nodeWidthsInitialized = m_nodeViewVisitor.columnWidthsAreInitialized();
                    m_currentModel->visitNodes(m_nodeViewVisitor);

                    // Update node groups after nodes are rendered and positioned
                    m_nodeViewVisitor.updateNodeGroups();

                    // Perform pending initial auto-layout after nodes are first rendered,
                    // so widths/metrics are available to the layout engine.
                    if (m_pendingAutoLayout && m_nodeWidthsInitialized)
                    {
                        m_pendingAutoLayout = false;
                        autoLayout();
                    }
                }
                onCreateNode();
                onDeleteNode();

                // Keyboard copy/paste when editor is focused
                // Block copy/paste during export to prevent model modifications
                bool const exportLocked = m_exportState && m_exportState->isExportInProgress();
                if (!exportLocked && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows))
                {
                    ImGuiIO & io = ImGui::GetIO();
                    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false))
                    {
                        copySelectionToClipboard();
                    }
                    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false))
                    {
                        m_pendingPasteRequest = true;
                    }
                }

                // Handle group movement - detect when nodes are moved and move their group members
                m_nodeViewVisitor.handleGroupMovement();

                // Handle group dragging via header/border areas - must be called before rendering
                m_nodeViewVisitor.handleGroupDragging();

                // Render node group last, to prioritize node interaction
                m_nodeViewVisitor.renderNodeGroups();

                // Allow quick navigation with mouse back/forward buttons when editor is hovered
                if (isHovered())
                {
                    // Prefer key-based detection for mouse X buttons using ImGuiKey_* constants
                    if (ImGui::IsKeyPressed(ImGuiKey_MouseX1, false))
                    {
                        goBack();
                    }
                    if (ImGui::IsKeyPressed(ImGuiKey_MouseX2, false))
                    {
                        goForward();
                    }
                }

                // Check for group double-clicks and handle them AFTER rendering (so bounds are
                // updated)
                std::string doubleClickedGroup = m_nodeViewVisitor.checkForGroupClick();
                if (!doubleClickedGroup.empty())
                {
                    m_nodeViewVisitor.handleGroupClick(doubleClickedGroup);
                }

                ed::End();
                ed::PopStyleColor();

                // Export overlay is now rendered at MainWindow level to block entire UI

                if (m_nodeViewVisitor.haveParameterChanged())
                {
                    m_dirty = true;
                    parameterChanged = true;
                    m_currentModel->setLogger(m_doc->getSharedLogger());
                    m_currentModel->updateTypes();
                    if (!m_stateApplyingUndo)
                    {
                        auto tmpAssembly = *m_assembly;
                        m_history.storeState(tmpAssembly, "Parameter changed");
                    }
                }

                m_modelWasModified |= m_nodeViewVisitor.hasModelChanged();

                if (m_nodePositionsNeedUpdate)
                {
                    applyNodePositions();
                }
                else
                {
                    readBackNodePositions();
                }
            }

            ImGui::End();
            ImGui::PopStyleVar();
        }
        catch (std::exception const & e)
        {
            if (m_doc && m_doc->getSharedLogger())
            {
                m_doc->getSharedLogger()->addEvent(
                    {fmt::format("Model editor error: {}", e.what()), events::Severity::Error});
            }
        }

        // Extract Function dialog
        if (m_showExtractDialog)
        {
            ImVec2 const center(ImGui::GetIO().DisplaySize.x * 0.5f,
                                ImGui::GetIO().DisplaySize.y * 0.5f);
            ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
            ImGui::OpenPopup("Extract Function");
            if (ImGui::BeginPopupModal(
                  "Extract Function", &m_showExtractDialog, ImGuiWindowFlags_AlwaysAutoResize))
            {
                ImGui::Text("Create a new function from the selected nodes.");
                ImGui::Separator();
                ImGui::InputText("Function name", &m_extractFunctionName);

                // Gather selection ids and compute name proposals on first open
                if (!m_extractDialogInitialized)
                {
                    m_extractDialogInitialized = true;
                    m_extractInputNames.clear();
                    m_extractOutputNames.clear();

                    auto selectionIds = selectedNodes(m_editorContext);
                    std::set<nodes::NodeId> selection;
                    for (auto const & nid : selectionIds)
                        selection.insert(static_cast<nodes::NodeId>(nid.Get()));
                    if (m_currentModel && !selection.empty())
                    {
                        auto props =
                          nodes::FunctionExtractor::proposeNames(*m_currentModel, selection);
                        for (auto const & e : props.inputs)
                            m_extractInputNames.push_back({e.uniqueKey, e.defaultName, e.type});
                        for (auto const & e : props.outputs)
                            m_extractOutputNames.push_back({e.uniqueKey, e.defaultName, e.type});
                    }
                }

                // Compute validation for current names using validation utilities
                std::vector<std::string> inputNames;
                std::vector<std::string> outputNames;
                for (auto const & e : m_extractInputNames)
                    inputNames.push_back(e.name);
                for (auto const & e : m_extractOutputNames)
                    outputNames.push_back(e.name);

                std::vector<bool> inputValid;
                std::vector<bool> outputValid;
                bool const inputsValid = validation::validateUniqueNames(inputNames, inputValid);
                bool const outputsValid = validation::validateUniqueNames(outputNames, outputValid);
                bool const allNamesValid = inputsValid && outputsValid;

                // Editable list of argument names
                if (!m_extractInputNames.empty())
                {
                    ImGui::Separator();
                    ImGui::Text("Inputs (arguments):");
                    ImGui::BeginChild("##extract_inputs", ImVec2(500, 150), true);
                    for (size_t i = 0; i < m_extractInputNames.size(); ++i)
                    {
                        ImGui::PushID(static_cast<int>(i));
                        ImGui::Text("%s", m_extractInputNames[i].key.c_str());
                        ImGui::SameLine();
                        ImGui::PushItemWidth(260.0f * m_uiScale);
                        ImGui::InputText("##argname", &m_extractInputNames[i].name);
                        if (!inputValid[i])
                        {
                            ImGui::SameLine();
                            ImGui::TextColored(ImVec4(1.0f, 0.25f, 0.25f, 1.0f), "invalid");
                        }
                        ImGui::PopItemWidth();
                        ImGui::PopID();
                    }
                    ImGui::EndChild();
                }

                // Editable list of output names
                if (!m_extractOutputNames.empty())
                {
                    ImGui::Separator();
                    ImGui::Text("Outputs:");
                    ImGui::BeginChild("##extract_outputs", ImVec2(500, 150), true);
                    for (size_t i = 0; i < m_extractOutputNames.size(); ++i)
                    {
                        ImGui::PushID(static_cast<int>(10000 + i));
                        ImGui::Text("%s", m_extractOutputNames[i].key.c_str());
                        ImGui::SameLine();
                        ImGui::PushItemWidth(260.0f * m_uiScale);
                        ImGui::InputText("##outname", &m_extractOutputNames[i].name);
                        ImGui::PopItemWidth();
                        ImGui::PopID();
                    }
                    ImGui::EndChild();
                }
                bool valid = !m_extractFunctionName.empty();
                if (!valid)
                {
                    ImGui::TextColored(ImVec4(1, 0.5f, 0, 1), "Please enter a function name.");
                }
                if (valid && ImGui::Button("Extract", ImVec2(120, 0)))
                {
                    // Build override maps from edited names
                    std::unordered_map<std::string, std::string> inputOverrides;
                    std::unordered_map<std::string, std::string> outputOverrides;
                    for (auto const & e : m_extractInputNames)
                        inputOverrides[e.key] = e.name;
                    for (auto const & e : m_extractOutputNames)
                        outputOverrides[e.key] = e.name;

                    // Perform extraction with overrides
                    if (m_doc && m_currentModel)
                    {
                        auto selectionIds = selectedNodes(m_editorContext);
                        std::set<nodes::NodeId> selection;
                        for (auto const & nid : selectionIds)
                            selection.insert(static_cast<nodes::NodeId>(nid.Get()));
                        nodes::Model & newModel = m_doc->createNewFunction();
                        newModel.setDisplayName(m_extractFunctionName);
                        createUndoRestorePoint("Extract Function");
                        nodes::FunctionExtractor::Result result;
                        bool ok = nodes::FunctionExtractor::extractInto(*m_currentModel,
                                                                        newModel,
                                                                        selection,
                                                                        inputOverrides,
                                                                        outputOverrides,
                                                                        result);
                        if (!ok)
                        {
                            // rollback
                            m_doc->deleteFunction(newModel.getResourceId());
                        }
                        else
                        {
                            if (result.functionCall)
                            {
                                result.functionCall->setFunctionId(newModel.getResourceId());
                                result.functionCall->updateInputsAndOutputs(newModel);
                                m_currentModel->registerInputs(*result.functionCall);
                                m_currentModel->registerOutputs(*result.functionCall);
                                // Place the node near selection center
                                ImVec2 minP{std::numeric_limits<float>::max(),
                                            std::numeric_limits<float>::max()};
                                ImVec2 maxP{-std::numeric_limits<float>::max(),
                                            -std::numeric_limits<float>::max()};
                                for (auto sid : selection)
                                {
                                    auto opt = m_currentModel->getNode(sid);
                                    if (!opt.has_value())
                                        continue;
                                    auto p = opt.value()->screenPos();
                                    minP.x = std::min(minP.x, p.x);
                                    minP.y = std::min(minP.y, p.y);
                                    maxP.x = std::max(maxP.x, p.x);
                                    maxP.y = std::max(maxP.y, p.y);
                                }
                                ImVec2 center{(minP.x + maxP.x) * 0.5f, (minP.y + maxP.y) * 0.5f};
                                ed::SetNodePosition(result.functionCall->getId(), center);
                                requestNodeFocus(result.functionCall->getId());
                            }

                            if (m_assembly)
                            {
                                m_assembly->updateInputsAndOutputs();
                            }

                            m_currentModel->setLogger(m_doc->getSharedLogger());
                            m_currentModel->updateTypes();
                            markModelAsModified();
                            switchModel();
                            m_nodePositionsNeedUpdate = true;
                        }
                    }
                    m_showExtractDialog = false;
                    m_extractDialogInitialized = false;
                    m_extractInputNames.clear();
                    m_extractOutputNames.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::SameLine();
                if (ImGui::Button("Cancel", ImVec2(120, 0)))
                {
                    m_showExtractDialog = false;
                    m_extractDialogInitialized = false;
                    m_extractInputNames.clear();
                    m_extractOutputNames.clear();
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        m_parameterDirty = parameterChanged;
        return m_parameterDirty;
    }

    void ModelEditor::triggerNodePositionUpdate()
    {
        m_nodePositionsNeedUpdate = true;
    }

    void ModelEditor::showPopupMenu(PopupMenuFunction popupMenuFunction)
    {
        m_popupMenuFunction = std::move(popupMenuFunction);
    }

    void ModelEditor::closePopupMenu()
    {
        m_popupMenuFunction = noOp;
    }

    auto ModelEditor::currentModel() const -> nodes::SharedModel
    {
        return m_currentModel;
    }

    void ModelEditor::setDocument(std::shared_ptr<Document> document)
    {
        if (!document)
        {
            return;
        }
        m_doc = std::move(document);
        setAssembly(m_doc->getAssembly());

        if (m_doc)
        {
            m_libraryBrowser.setLogger(m_doc->getSharedLogger());
        }

        m_outline.setDocument(m_doc);
    }

    void ModelEditor::setExportState(ExportState * state)
    {
        m_exportState = state;
        m_resourceView.setExportState(state);
        m_beamLatticeView.setExportState(state);
    }

    void ModelEditor::setAssembly(nodes::SharedAssembly assembly)
    {
        if (!assembly)
        {
            return;
        }

        m_assembly = std::move(assembly);
        m_currentModel = m_assembly->assemblyModel();

        // if there are other models, we switch to the first one
        if (m_assembly->getFunctions().size() > 1)
        {
            // find first function that is not the assembly model
            for (auto & [id, model] : m_assembly->getFunctions())
            {
                if (model->getResourceId() != m_assembly->assemblyModel()->getResourceId())
                {
                    m_currentModel = model;
                    break;
                }
            }
        }

        m_nodePositionsNeedUpdate = true;
        m_history = History();
        switchModel();
        // Initialize navigation history with the current model
        m_navHistory.reset(m_currentModel ? m_currentModel->getResourceId() : 0u);
    }

    bool ModelEditor::matchesNodeFilter(const std::string & text) const
    {
        if (m_nodeFilterText.empty())
        {
            return true; // No filter active, match everything
        }

        // Case-insensitive comparison
        std::string lowerText = text;
        std::string lowerFilter = m_nodeFilterText;
        std::transform(lowerText.begin(),
                       lowerText.end(),
                       lowerText.begin(),
                       [](unsigned char c) { return std::tolower(c); });
        std::transform(lowerFilter.begin(),
                       lowerFilter.end(),
                       lowerFilter.begin(),
                       [](unsigned char c) { return std::tolower(c); });

        return lowerText.find(lowerFilter) != std::string::npos;
    }

    void ModelEditor::functionToolBox(ImVec2 mousePos)
    {
        auto functions = m_assembly->getFunctions();
        for (auto & [id, model] : functions)
        {
            if (!model || model->isManaged() || model == m_currentModel)
            {
                continue;
            }

            // Get the display name
            std::string displayName = model->getDisplayName().value_or("function");

            // Check if it matches the filter
            if (!matchesNodeFilter(displayName))
            {
                continue; // Skip this item if it doesn't match the filter
            }

            if (ImGui::Button(displayName.c_str()))
            {
                createUndoRestorePoint("Create node");
                auto posOnCanvas = ed::ScreenToCanvas(mousePos);
                // create function call node
                auto createdNode = m_currentModel->create<nodes::FunctionCall>();
                createdNode->setFunctionId(id); // expands to a ResourceId node during writing
                createdNode->updateInputsAndOutputs(*model);
                m_currentModel->registerInputs(*createdNode);
                m_currentModel->registerOutputs(*createdNode);
                ed::SetNodePosition(createdNode->getId(), posOnCanvas);

                if (model->getDisplayName().has_value())
                {
                    createdNode->setDisplayName(model->getDisplayName().value());
                }

                // Request focus on the newly created node for keyboard-driven workflow
                requestNodeFocus(createdNode->getId());

                markModelAsModified();
            }
        }
    }

    void ModelEditor::meshResourceToolBox(ImVec2 mousePos)
    {
        auto & resourceManager = m_doc->getResourceManager();

        auto const & resources = resourceManager.getResourceMap();

        for (auto const & [key, res] : resources)
        {
            auto const * mesh = dynamic_cast<MeshResource const *>(res.get());
            if (!mesh)
            {
                continue;
            }

            // Get the display name
            std::string displayName = key.getDisplayName();

            // Check if it matches the filter
            if (!matchesNodeFilter(displayName))
            {
                continue; // Skip this item if it doesn't match the filter
            }

            if (ImGui::Button(displayName.c_str()))
            {
                createUndoRestorePoint("Create node");
                auto posOnCanvas = ed::ScreenToCanvas(mousePos);
                auto createdNode = m_currentModel->create<nodes::Resource>();
                createdNode->setResourceId(key.getResourceId().value());
                ed::SetNodePosition(createdNode->getId(), posOnCanvas);

                auto signedDistanceToMesh = m_currentModel->create<nodes::SignedDistanceToMesh>();
                ImVec2 const posOnCanvasWithOffset = ImVec2(posOnCanvas.x + 400, posOnCanvas.y);
                m_currentModel->addLink(createdNode->getOutputValue().getId(),
                                        signedDistanceToMesh->parameter().at("mesh").getId());

                signedDistanceToMesh->setDisplayName("SD to " + key.getDisplayName());
                ed::SetNodePosition(signedDistanceToMesh->getId(), posOnCanvasWithOffset);

                // Request focus on the SignedDistanceToMesh node as it's more useful to focus on
                requestNodeFocus(signedDistanceToMesh->getId());

                markModelAsModified();
            }
        }
    }

    void ModelEditor::beamLatticeResourceToolBox(ImVec2 mousePos)
    {
        // Similar to meshResourceToolBox: for each BeamLatticeResource create a quick action
        // button that spawns a Resource node plus a SignedDistanceToBeamLattice node already
        // linked to it. This streamlines inserting signed distance queries for beam lattices.
        if (!m_doc)
        {
            return;
        }

        auto & resourceManager = m_doc->getResourceManager();
        auto const & resources = resourceManager.getResourceMap();

        for (auto const & [key, res] : resources)
        {
            auto const * beamLattice =
              dynamic_cast<gladius::BeamLatticeResource const *>(res.get());
            if (!beamLattice)
            {
                continue;
            }

            // Display name for the resource
            std::string displayName = key.getDisplayName();

            if (!matchesNodeFilter(displayName))
            {
                continue; // Skip if it doesn't match filter
            }

            if (ImGui::Button(displayName.c_str()))
            {
                createUndoRestorePoint("Create beam lattice SD node");
                auto posOnCanvas = ed::ScreenToCanvas(mousePos);

                // Create the resource node first (same pattern as meshes)
                auto createdResourceNode = m_currentModel->create<nodes::Resource>();
                createdResourceNode->setResourceId(key.getResourceId().value());
                ed::SetNodePosition(createdResourceNode->getId(), posOnCanvas);

                // Create the signed distance to beam lattice node and connect
                auto signedDistanceNode =
                  m_currentModel->create<nodes::SignedDistanceToBeamLattice>();
                ImVec2 const offsetPos = ImVec2(posOnCanvas.x + 400, posOnCanvas.y);
                m_currentModel->addLink(createdResourceNode->getOutputValue().getId(),
                                        signedDistanceNode->parameter().at("beamLattice").getId());

                signedDistanceNode->setDisplayName("SD to " + key.getDisplayName());
                ed::SetNodePosition(signedDistanceNode->getId(), offsetPos);

                // Focus the distance node as primary interaction target
                requestNodeFocus(signedDistanceNode->getId());

                markModelAsModified();
            }
        }
    }

    void ModelEditor::undo()
    {
        if (m_history.canUnDo())
        {
            std::optional<ResourceId> modelId;
            if (currentModel())
            {
                modelId = currentModel()->getResourceId();
            }
            m_stateApplyingUndo = true;
            m_history.undo(m_assembly.get());
            if (modelId.has_value())
            {
                m_currentModel = m_assembly->findModel(modelId.value());
            }
            switchModel();
            invalidateEverything();
        }
    }

    void ModelEditor::redo()
    {
        if (m_history.canReDo())
        {
            std::optional<ResourceId> modelId;
            if (currentModel())
            {
                modelId = currentModel()->getResourceId();
            }
            m_stateApplyingUndo = true;
            m_history.redo(m_assembly.get());
            if (modelId.has_value())
            {
                m_currentModel = m_assembly->findModel(modelId.value());
            }
            switchModel();
            invalidateEverything();
        }
    }

    void ModelEditor::pushNodeColor(nodes::NodeBase & node)
    {
        auto const colorIter = m_nodeTypeToColor.find(typeid(node));
        if (colorIter != std::end(m_nodeTypeToColor))
        {
            ImVec4 const color = colorIter->second;
            ImVec4 const colorDark =
              ImVec4(color.x * 0.6f, color.y * 0.6f, color.z * 0.6f, color.w);
            ImVec4 const colorHovered =
              ImVec4(color.x * 0.8f, color.y * 0.8f, color.z * 0.8f, color.w);
            ImGui::PushStyleColor(ImGuiCol_Button, colorDark);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, color);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, colorHovered);

            ImGui::PushStyleColor(ImGuiCol_Header, colorDark);
            ImGui::PushStyleColor(ImGuiCol_HeaderActive, color);
            ImGui::PushStyleColor(ImGuiCol_HeaderHovered, colorHovered);
        }
    }

    void ModelEditor::popNodeColor(nodes::NodeBase & node)
    {
        auto const colorIter = m_nodeTypeToColor.find(typeid(node));
        if (colorIter != std::end(m_nodeTypeToColor))
        {
            ImGui::PopStyleColor(6);
        }
    }

    auto ModelEditor::modelWasModified() const -> bool
    {
        return m_modelWasModified;
    }

    bool ModelEditor::isCompileRequested() const
    {
        if (m_isManualCompileRequested)
        {
            return true;
        }

        if (!m_autoCompile)
        {
            return false;
        }
        return m_modelWasModified;
    }

    void ModelEditor::markModelAsModified()
    {
        // if (!m_autoCompile)
        // {
        //     return;
        // }

        m_modelWasModified = true;
        invalidatePrimitiveData();
    }

    void ModelEditor::markModelAsUpToDate()
    {
        m_modelWasModified = false;
        m_isManualCompileRequested = false;
    }

    void ModelEditor::readBackNodePositions()
    {
        if (!currentModel())
        {
            return;
        }
        for (auto & node : *currentModel())
        {
            if (!node.second)
            {
                continue;
            }
            auto const screenPos = ed::GetNodePosition(node.second->getId());
            node.second->screenPos().x = screenPos.x;
            node.second->screenPos().y = screenPos.y;
        }
        m_nodePositionsNeedUpdate = false;
    }

    void ModelEditor::autoLayout()
    {
        if (currentModel() == nullptr)
        {
            return;
        }

        // if (!m_nodeWidthsInitialized)
        // {
        //     return;
        // }

        createUndoRestorePoint("Autolayout");

        // Use the dedicated layout engine for all layout operations
        gladius::ui::NodeLayoutEngine layoutEngine;
        gladius::ui::NodeLayoutEngine::LayoutConfig config;
        config.nodeDistance = m_nodeDistance;
        // Use a proportional layer spacing; strategies can scale further.
        config.layerSpacing = std::max(m_nodeDistance * 2.0f, m_nodeDistance);
        config.groupPadding = m_nodeDistance * 0.5f;

        layoutEngine.setNodeSizeProvider([](nodes::NodeId nodeId) {
            auto * editorContext = ed::GetCurrentEditor();
            if (editorContext == nullptr)
            {
                return kDefaultNodeSize;
            }

            auto size = ed::GetNodeSize(nodeId);
            if (size.x <= 0.0f || size.y <= 0.0f)
            {
                return kDefaultNodeSize;
            }

            return size;
        });

        layoutEngine.setNodePositionWriter([](nodes::NodeId nodeId, ImVec2 const & position) {
            auto * editorContext = ed::GetCurrentEditor();
            if (editorContext == nullptr)
            {
                return;
            }

            ed::SetNodePosition(nodeId, position);
        });

        std::vector<NodeLayoutEngine::LayoutStrategy> strategies;

        if (m_selectedLayoutStrategy == LayoutStrategyChoice::Auto)
        {
            auto const descriptors = layoutStrategyDescriptors();
            strategies.reserve(descriptors.size());
            for (auto const & descriptor : descriptors)
            {
                strategies.emplace_back(makeLayoutStrategy(descriptor.choice));
            }
        }
        else
        {
            strategies.emplace_back(makeLayoutStrategy(m_selectedLayoutStrategy));
        }

        if (strategies.empty())
        {
            return;
        }

        layoutEngine.setStrategies(std::move(strategies));

        layoutEngine.performAutoLayout(*currentModel(), config);

        m_nodePositionsNeedUpdate = true;
    }

    void ModelEditor::applyNodePositions()
    {
        if (currentModel() == nullptr)
        {
            return;
        }
        if (!m_nodePositionsNeedUpdate)
        {
            return;
        }

        m_nodePositionsNeedUpdate = false;

        for (auto & node : *currentModel())
        {
            auto const targetPos = node.second->screenPos();
            ed::SetNodePosition(node.first, {targetPos.x, targetPos.y});
        }
        ed::NavigateToContent();
    }

    void ModelEditor::placeTransformation(nodes::NodeBase & createdNode,
                                          std::vector<ed::NodeId> & selection) const
    {
        if (currentModel() == nullptr)
        {
            return;
        }
        auto selectedNode =
          currentModel()->getNode(static_cast<nodes::NodeId>(selection.back().Get()));
        if (selectedNode)
        {
            auto const screenPos = selectedNode.value()->screenPos();
            createdNode.screenPos().x = screenPos.x - kNodePlacementOffsetX;
            createdNode.screenPos().y = screenPos.y;
        }
        auto const csOutPut = createdNode.getOutputs()[nodes::FieldNames::Pos];
        auto & csInPut = createdNode.parameter()[nodes::FieldNames::Pos];
        for (auto const nodeId : selection)
        {
            (void) nodeId; // only for suppressing warnings about unused nodeId
            auto selNode =
              currentModel()->getNode(static_cast<nodes::NodeId>(selection.back().Get()));
            if (selNode)
            {
                auto csIter = selNode.value()->parameter().find(nodes::FieldNames::Pos);
                if (csIter != std::end(selNode.value()->parameter()))
                {
                    if (csIter->second.getSource().has_value())
                    {
                        currentModel()->addLink(csIter->second.getSource().value().portId,
                                                csInPut.getId());
                    }
                    currentModel()->addLink(csOutPut.getId(), csIter->second.getId());
                }
            }
        }
    }

    void ModelEditor::placeBoolOp(nodes::NodeBase & createdNode,
                                  std::vector<ed::NodeId> & selection) const
    {
        if (currentModel() == nullptr)
        {
            return;
        }
        if (selection.size() != 2)
        {
            defaultPlacement(createdNode, selection);
            return;
        }

        auto selectedNode =
          currentModel()->getNode(static_cast<nodes::NodeId>(selection.back().Get()));
        if (selectedNode)
        {
            auto const screenPos = selectedNode.value()->screenPos();
            createdNode.screenPos().x = screenPos.x + kNodePlacementOffsetX;
            createdNode.screenPos().y = screenPos.y;
        }
        auto const shapeOutPut = createdNode.getOutputs().at(FieldNames::Shape);
        std::array<nodes::ParameterId, 2> shapeInputs{
          createdNode.parameter()[FieldNames::A].getId(),
          createdNode.parameter()[FieldNames::B].getId()};

        for (auto i = 0; i < 2; i++)
        {
            auto selNode = currentModel()->getNode(static_cast<nodes::NodeId>(selection[i].Get()));
            if (selNode)
            {
                auto shapeIter = selNode.value()->getOutputs().find(FieldNames::Shape);
                if (shapeIter != std::end(selNode.value()->getOutputs()))
                {
                    currentModel()->addLink(shapeIter->second.getId(), shapeInputs[i]);
                }
            }
        }
    }

    void ModelEditor::defaultPlacement(nodes::NodeBase & createdNode,
                                       std::vector<ed::NodeId> & selection) const
    {
        if (currentModel() == nullptr)
        {
            return;
        }
        auto selectedNode =
          currentModel()->getNode(static_cast<nodes::NodeId>(selection.back().Get()));
        if (selectedNode)
        {
            auto const screenPos = selectedNode.value()->screenPos();
            createdNode.screenPos().x = screenPos.x + kNodePlacementOffsetX;
            createdNode.screenPos().y = screenPos.y;
        }
    }

    void ModelEditor::placeNode(nodes::NodeBase & createdNode)
    {
        auto selection = selectedNodes(m_editorContext);
        auto const category = createdNode.getCategory();
        if (!selection.empty())
        {
            switch (category)
            {
            case nodes::Category::Transformation:
                placeTransformation(createdNode, selection);
                break;
            case nodes::Category::BoolOperation:
                placeBoolOp(createdNode, selection);
                break;
            case nodes::Category::Internal:
            case nodes::Category::Primitive:
            case nodes::Category::Alteration:

            case nodes::Category::Lattice:
            case nodes::Category::Misc:
            default:
                defaultPlacement(createdNode, selection);
            }
        }
        m_nodePositionsNeedUpdate = true;
    }

    void ModelEditor::setVisibility(bool visible)
    {
        m_visible = visible;
    }

    auto ModelEditor::isVisible() const -> bool
    {
        return m_visible;
    }

    void ModelEditor::createUndoRestorePoint(std::string const & description)
    {
        if (m_stateApplyingUndo)
        {
            return;
        }
        m_history.storeState(*m_assembly, description);
    }

    void ModelEditor::resetUndo()
    {
        m_history = History();
    }

    auto ModelEditor::primitveDataNeedsUpdate() const -> bool
    {
        return m_primitiveDataDirty;
    }

    void ModelEditor::invalidatePrimitiveData()
    {
        m_primitiveDataDirty = true;
    }

    void ModelEditor::markPrimitiveDataAsUpToDate()
    {
        m_primitiveDataDirty = false;
    }

    auto selectedNodes(ed::EditorContext * const editorContext) -> std::vector<ed::NodeId>
    {
        SetCurrentEditor(editorContext);

        auto const numSelectedItems = ed::GetSelectedObjectCount();
        std::vector<ed::NodeId> selectedNodeIds(numSelectedItems);
        GetSelectedNodes(selectedNodeIds.data(), static_cast<int>(numSelectedItems));
        return selectedNodeIds;
    }

    void ModelEditor::showDeleteUnusedResourcesDialog()
    {
        if (!m_showDeleteUnusedResourcesConfirmation)
        {
            return;
        }

        ImVec2 const center(ImGui::GetIO().DisplaySize.x * 0.5f,
                            ImGui::GetIO().DisplaySize.y * 0.5f);
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        if (!ImGui::IsPopupOpen("Delete Unused Resources"))
        {
            ImGui::OpenPopup("Delete Unused Resources");
        }

        if (ImGui::BeginPopupModal("Delete Unused Resources",
                                   &m_showDeleteUnusedResourcesConfirmation,
                                   ImGuiWindowFlags_AlwaysAutoResize))
        {
            if (m_unusedResources.empty())
            {
                ImGui::TextUnformatted("No unused resources found in the model.");
            }
            else
            {
                ImGui::Text("The following %zu unused resources will be deleted:",
                            m_unusedResources.size());
                ImGui::Separator();

                ImGui::BeginChild("ResourceList", ImVec2(400, 300), true);

                for (auto const & resource : m_unusedResources)
                {
                    try
                    {
                        Lib3MF_uint32 modelResourceId = resource->GetModelResourceID();
                        ResourceKey key{modelResourceId};
                        std::string resourceName = key.getDisplayName();

                        // Try to determine the resource type
                        std::string resourceType = "Unknown";
                        try
                        {
                            if (std::dynamic_pointer_cast<Lib3MF::CFunction>(resource))
                                resourceType = "Function";
                            else if (std::dynamic_pointer_cast<Lib3MF::CMeshObject>(resource))
                                resourceType = "Mesh";
                            else if (std::dynamic_pointer_cast<Lib3MF::CComponentsObject>(resource))
                                resourceType = "Components";
                            else if (std::dynamic_pointer_cast<Lib3MF::CLevelSet>(resource))
                                resourceType = "Level Set";
                        }
                        catch (const std::exception &)
                        {
                            // Fallback to unknown type
                        }

                        ImGui::Text("%s #%u (%s)",
                                    resourceName.c_str(),
                                    modelResourceId,
                                    resourceType.c_str());
                    }
                    catch (const std::exception & e)
                    {
                        ImGui::Text("Error getting resource info: %s", e.what());
                    }
                }

                ImGui::EndChild();
                ImGui::Separator();

                ImGui::Text("Are you sure you want to delete these resources?");
                ImGui::Text("This action cannot be undone.");
                ImGui::Separator();

                if (ImGui::Button("Delete", ImVec2(120, 0)))
                {
                    m_doc->removeUnusedResources();
                    markModelAsModified();
                    m_showDeleteUnusedResourcesConfirmation = false;
                    ImGui::CloseCurrentPopup();
                }

                ImGui::SameLine();

                if (ImGui::Button("Cancel", ImVec2(120, 0)))
                {
                    m_unusedResources.clear();
                    m_showDeleteUnusedResourcesConfirmation = false;
                    ImGui::CloseCurrentPopup();
                }
            }

            ImGui::EndPopup();
        }
    }
    void ModelEditor::setLibraryRootDirectory(const std::filesystem::path & directory)
    {
        m_libraryBrowser.setRootDirectory(directory);
    }

    void ModelEditor::toggleLibraryVisibility()
    {
        m_libraryBrowser.setVisibility(!m_libraryBrowser.isVisible());
    }

    void ModelEditor::setLibraryVisibility(bool visible)
    {
        m_libraryBrowser.setVisibility(visible);
    }

    bool ModelEditor::isLibraryVisible() const
    {
        return m_libraryBrowser.isVisible();
    }

    void ModelEditor::refreshLibraryDirectories()
    {
        m_libraryBrowser.refreshDirectories();
    }

    void ModelEditor::requestManualCompile()
    {
        m_isManualCompileRequested = true;
    }

    void ModelEditor::autoLayoutNodes(float distance)
    {
        autoLayout();
    }

    void ModelEditor::showCreateNodePopup()
    {
        // Get current mouse position and show the create node popup
        ImVec2 currentMousePos = ImGui::GetMousePos();
        showPopupMenu([&, currentMousePos]() { createNodePopup(-1, currentMousePos); });
        m_showCreateNodePopUp = true;
        ImGui::OpenPopup("Create Node");
    }

    void ModelEditor::showExpressionDialog()
    {
        m_expressionDialog.show();
    }

    void ModelEditor::extractSelectedNodesToFunction(const std::string & functionName)
    {
        if (!m_doc || !m_currentModel)
        {
            return;
        }

        auto selectionIds = selectedNodes(m_editorContext);
        if (selectionIds.empty())
        {
            return;
        }

        std::set<nodes::NodeId> selection;
        for (auto const & nid : selectionIds)
        {
            selection.insert(static_cast<nodes::NodeId>(nid.Get()));
        }

        // Create new function via Document to ensure 3MF resource exists
        nodes::Model & newModel = m_doc->createNewFunction();
        newModel.setDisplayName(functionName);

        createUndoRestorePoint("Extract Function");

        nodes::FunctionExtractor::Result result;
        bool ok =
          nodes::FunctionExtractor::extractInto(*m_currentModel, newModel, selection, result);
        if (!ok)
        {
            // Rollback by removing the created empty function
            m_doc->deleteFunction(newModel.getResourceId());
            return;
        }

        // Find the FunctionCall we just created and assign the function id
        nodes::FunctionCall * fc = result.functionCall;
        if (fc)
        {
            fc->setFunctionId(newModel.getResourceId());
            fc->updateInputsAndOutputs(newModel);
            m_currentModel->registerInputs(*fc);
            m_currentModel->registerOutputs(*fc);
            if (auto dn = newModel.getDisplayName(); dn.has_value())
            {
                fc->setDisplayName(dn.value());
            }
            // Place the node near selection center
            ImVec2 minP{std::numeric_limits<float>::max(), std::numeric_limits<float>::max()};
            ImVec2 maxP{-std::numeric_limits<float>::max(), -std::numeric_limits<float>::max()};
            for (auto sid : selection)
            {
                auto opt = m_currentModel->getNode(sid);
                if (!opt.has_value())
                    continue;
                auto p = opt.value()->screenPos();
                minP.x = std::min(minP.x, p.x);
                minP.y = std::min(minP.y, p.y);
                maxP.x = std::max(maxP.x, p.x);
                maxP.y = std::max(maxP.y, p.y);
            }
            ImVec2 center{(minP.x + maxP.x) * 0.5f, (minP.y + maxP.y) * 0.5f};
            ed::SetNodePosition(fc->getId(), ImVec2(center.x, center.y));
            requestNodeFocus(fc->getId());
        }

        // After wiring, update assembly IOs so downstream validation doesn't crash
        if (m_assembly)
        {
            m_assembly->updateInputsAndOutputs();
        }

        // Track state and UI
        m_currentModel->setLogger(m_doc->getSharedLogger());
        m_currentModel->updateTypes();
        markModelAsModified();
        switchModel();
        m_nodePositionsNeedUpdate = true;
    }

    void
    ModelEditor::onCreateFunctionFromExpression(std::string const & functionName,
                                                std::string const & expression,
                                                std::vector<FunctionArgument> const & arguments,
                                                FunctionOutput const & output)
    {
        if (!m_doc || !m_assembly || functionName.empty() || expression.empty())
        {
            return;
        }

        try
        {
            // Create a new function model
            nodes::Model & newModel = m_doc->createNewFunction();
            newModel.setDisplayName(functionName);

            // Create a parser instance
            ExpressionParser parser;

            // Convert expression to node graph
            nodes::NodeId resultNodeId = ExpressionToGraphConverter::convertExpressionToGraph(
              expression, newModel, parser, arguments, output);

            if (resultNodeId != 0)
            {
                // Successfully created the graph - switch to the new function
                m_currentModel = m_assembly->findModel(newModel.getResourceId());
                switchModel();
                markModelAsModified();

                // Close the dialog
                m_expressionDialog.hide();
            }
            else
            {
                // Failed to convert expression - remove the created function
                m_doc->deleteFunction(newModel.getResourceId());

                if (auto logger = m_doc->getSharedLogger())
                {
                    logger->addEvent(
                        {fmt::format("Failed to convert expression to graph: {}", expression),
                         events::Severity::Error});
                }
            }
        }
        catch (std::exception const & ex)
        {
            if (m_doc && m_doc->getSharedLogger())
            {
                m_doc->getSharedLogger()->addEvent(
                    {fmt::format("Error creating function from expression: {}", ex.what()),
                     events::Severity::Error});
            }
        }
    }

    bool ModelEditor::switchToFunction(nodes::ResourceId functionId)
    {
        if (!m_assembly)
        {
            return false;
        }

        auto functionModel = m_assembly->findModel(functionId);
        if (!functionModel)
        {
            return false;
        }

        m_currentModel = functionModel;
        switchModel();
        return true;
    }

    bool ModelEditor::navigateToFunction(nodes::ResourceId functionId)
    {
        if (!m_assembly)
        {
            return false;
        }

        auto target = m_assembly->findModel(functionId);
        if (!target)
        {
            return false;
        }

        // Record navigation in history (returns false if no-op)
        nodes::ResourceId const currentId = m_currentModel ? m_currentModel->getResourceId() : 0u;
        if (!m_navHistory.recordNavigation(currentId, functionId))
        {
            return true; // No-op navigation, already at target
        }

        return switchToFunction(functionId);
    }

    bool ModelEditor::canGoBack() const
    {
        return m_navHistory.canGoBack();
    }

    bool ModelEditor::canGoForward() const
    {
        return m_navHistory.canGoForward();
    }

    bool ModelEditor::goBack()
    {
        m_navHistory.setInHistoryNavigation(true);
        auto const targetId = m_navHistory.goBack();
        bool const ok = (targetId != 0u) && switchToFunction(targetId);
        m_navHistory.setInHistoryNavigation(false);
        return ok;
    }

    bool ModelEditor::goForward()
    {
        m_navHistory.setInHistoryNavigation(true);
        auto const targetId = m_navHistory.goForward();
        bool const ok = (targetId != 0u) && switchToFunction(targetId);
        m_navHistory.setInHistoryNavigation(false);
        return ok;
    }

    bool ModelEditor::isHovered() const
    {
        // Check if any of the editor windows are hovered
        return ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow) && isVisible();
    }

    void ModelEditor::requestNodeFocus(nodes::NodeId nodeId)
    {
        m_nodeToFocus = nodeId;
        m_shouldFocusNode = true;
    }

    bool ModelEditor::shouldFocusNode(nodes::NodeId nodeId) const
    {
        return m_shouldFocusNode && m_nodeToFocus == nodeId;
    }

    void ModelEditor::clearNodeFocus()
    {
        m_shouldFocusNode = false;
        m_nodeToFocus = 0;
    }

    bool ModelEditor::hasClipboard() const
    {
        return m_clipboard.hasContent();
    }

    void ModelEditor::copySelectionToClipboard()
    {
        if (!m_currentModel)
        {
            return;
        }

        auto selection = selectedNodes(m_editorContext);
        if (selection.empty())
        {
            return;
        }

        std::set<nodes::NodeId> selectedIds;
        for (auto const & n : selection)
        {
            selectedIds.insert(static_cast<nodes::NodeId>(n.Get()));
        }

        m_clipboard.copyNodes(*m_currentModel, selectedIds);
    }

    void ModelEditor::pasteClipboardAtMouse()
    {
        if (!m_currentModel || !m_clipboard.hasContent())
        {
            return;
        }

        // Make sure we only use NodeEditor API when an editor is active
        ImVec2 mouse = ImGui::GetMousePos();
        ImVec2 rawCanvas = ed::ScreenToCanvas(mouse);
        ImVec2 canvas = m_clipboard.getAdjustedPastePosition(rawCanvas);

        createUndoRestorePoint("Paste node(s)");

        auto pastedMap = m_clipboard.pasteNodes(*m_currentModel, canvas);

        // Set node positions in the editor and select them
        ed::ClearSelection();
        for (auto const & [_, node] : pastedMap)
        {
            (void) _;
            ed::SetNodePosition(node->getId(),
                                ImVec2(node->screenPos().x, node->screenPos().y));
            ed::SelectNode(node->getId(), true);
        }
        ed::NavigateToSelection(true);

        markModelAsModified();

        // Track last paste canvas position for offset nudging
        m_clipboard.updatePastePosition(canvas);
    }

} // namespace gladius::ui
