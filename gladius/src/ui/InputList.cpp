#include "InputList.h"
#include "LinkColors.h"
#include "Style.h"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace gladius::ui
{
    namespace
    {
        struct PortOption
        {
            nodes::PortId id{};
            std::string label;
            nodes::Category category{nodes::Category::Internal};
        };

        struct ParameterOption
        {
            nodes::ParameterId id{};
            std::string label;
            nodes::Category category{nodes::Category::Internal};
        };

        bool touchButton(char const * label)
        {
            float const padding = std::max(8.f, ImGui::GetFontSize() * 0.45f);
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(padding, padding));
            bool const clicked = ImGui::Button(label);
            ImGui::PopStyleVar();
            return clicked;
        }

        bool pushNodeButtonStyle(nodes::Category category)
        {
            auto const styleIter = NodeColors.find(category);
            if (styleIter == std::end(NodeColors))
            {
                return false;
            }

            auto const & style = styleIter->second;
            ImGui::PushStyleColor(ImGuiCol_Button, static_cast<ImU32>(style.color));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, static_cast<ImU32>(style.activeColor));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, static_cast<ImU32>(style.hoveredColor));
            return true;
        }
    } // namespace

    OptionalInputMenuSelection inputMenu(nodes::Model & model, nodes::ParameterId targetId)
    {
        if (model.getParameter(targetId) == nullptr)
        {
            return {};
        }

        if (ImGui::BeginPopup("Ports"))
        {
            // Offer only constants that satisfy the target node's prospective type rule.
            if (model.canLinkTypeToInput(targetId, nodes::ParameterTypeIndex::Float))
            {
                ImGui::PushStyleColor(ImGuiCol_Button, LinkColors::DarkColorFloat);
                if (touchButton("New Scalar Node"))
                {
                    ImGui::CloseCurrentPopup();
                    ImGui::PopStyleColor();
                    ImGui::EndPopup();
                    return InputMenuSelection{0, nodes::ParameterTypeIndex::Float};
                }
                ImGui::PopStyleColor();
            }

            if (model.canLinkTypeToInput(targetId, nodes::ParameterTypeIndex::Float3))
            {
                ImGui::PushStyleColor(ImGuiCol_Button, LinkColors::DarkColorFloat3);
                if (touchButton("New Vector Node"))
                {
                    ImGui::CloseCurrentPopup();
                    ImGui::PopStyleColor();
                    ImGui::EndPopup();
                    return InputMenuSelection{0, nodes::ParameterTypeIndex::Float3};
                }
                ImGui::PopStyleColor();
            }

            if (model.canLinkTypeToInput(targetId, nodes::ParameterTypeIndex::Matrix4))
            {
                ImGui::PushStyleColor(ImGuiCol_Button, LinkColors::DarkColorMatrix);
                if (touchButton("New Matrix Node"))
                {
                    ImGui::CloseCurrentPopup();
                    ImGui::PopStyleColor();
                    ImGui::EndPopup();
                    return InputMenuSelection{0, nodes::ParameterTypeIndex::Matrix4};
                }
                ImGui::PopStyleColor();
            }

            auto const candidates = model.collectCompatibleLinkCandidates(targetId, false);
            std::vector<PortOption> options;
            options.reserve(candidates.size());
            for (auto const portId : candidates)
            {
                auto const * port = model.getPort(portId);
                if (port == nullptr)
                {
                    continue;
                }

                auto const sourceNode = model.getNode(port->getParentId());
                if (!sourceNode.has_value() || sourceNode.value() == nullptr)
                {
                    continue;
                }

                auto label = std::string{sourceNode.value()->getDisplayName()};
                label += "::";
                label += port->getShortName();
                options.push_back(PortOption{static_cast<nodes::PortId>(portId),
                                             std::move(label),
                                             sourceNode.value()->getCategory()});
            }
            std::sort(options.begin(),
                      options.end(),
                      [](PortOption const & lhs, PortOption const & rhs)
                      { return lhs.label < rhs.label; });

            for (auto const & option : options)
            {
                bool const popStyle = pushNodeButtonStyle(option.category);
                bool const clicked = touchButton(option.label.c_str());
                if (popStyle)
                {
                    ImGui::PopStyleColor(3);
                }

                if (clicked)
                {
                    ImGui::CloseCurrentPopup();
                    ImGui::EndPopup();
                    return InputMenuSelection{option.id, std::nullopt};
                }
            }

            if (options.empty())
            {
                ImGui::TextUnformatted("No compatible existing sources");
            }
            ImGui::EndPopup();
        }

        return {};
    }

    OptionalParameterId outputMenu(nodes::Model & model,
                                   nodes::PortId sourcePortId,
                                   std::function<bool()> renderCreateNodeMenu)
    {
        if (model.getPort(sourcePortId) == nullptr || !ImGui::BeginPopup("Destinations"))
        {
            return {};
        }

        if (renderCreateNodeMenu && ImGui::BeginMenu("Create Node"))
        {
            bool const createNodeHandled = renderCreateNodeMenu();
            ImGui::EndMenu();
            if (createNodeHandled)
            {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return {};
            }
        }

        ImGui::Separator();

        auto const candidates = model.collectCompatibleLinkCandidates(sourcePortId, true);
        std::vector<ParameterOption> options;
        options.reserve(candidates.size());
        for (auto const parameterId : candidates)
        {
            auto const * parameter = model.getParameter(parameterId);
            if (parameter == nullptr)
            {
                continue;
            }

            auto const targetNode = model.getNode(parameter->getParentId());
            if (!targetNode.has_value() || targetNode.value() == nullptr)
            {
                continue;
            }

            for (auto const & [name, candidate] : targetNode.value()->constParameter())
            {
                if (candidate.getId() == parameterId)
                {
                    auto label = std::string{targetNode.value()->getDisplayName()};
                    label += "::";
                    label += name;
                    options.push_back(ParameterOption{static_cast<nodes::ParameterId>(parameterId),
                                                       std::move(label),
                                                       targetNode.value()->getCategory()});
                    break;
                }
            }
        }
        std::sort(options.begin(),
                  options.end(),
                  [](ParameterOption const & lhs, ParameterOption const & rhs)
                  { return lhs.label < rhs.label; });

        for (auto const & option : options)
        {
            bool const popStyle = pushNodeButtonStyle(option.category);
            bool const clicked = touchButton(option.label.c_str());
            if (popStyle)
            {
                ImGui::PopStyleColor(3);
            }

            if (clicked)
            {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return option.id;
            }
        }

        if (options.empty())
        {
            ImGui::TextUnformatted("No compatible inputs");
        }
        ImGui::EndPopup();
        return {};
    }
} // namespace gladius::ui
