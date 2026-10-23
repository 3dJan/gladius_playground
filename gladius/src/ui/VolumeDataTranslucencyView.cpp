#include "VolumeDataTranslucencyView.h"

#include "Widgets.h"
#include "io/3mf/ResourceIdUtil.h"
#include "io/3mf/VolumeDataTranslucency.h"
#include "nodes/ModelUtils.h"
#include <algorithm>
#include <fmt/format.h>
#include <functional>
#include <lib3mf_implicit.hpp>
#include <vector>

namespace gladius::ui
{
    namespace
    {
        void logUiEvent(SharedDocument const & document, std::string const & message,
                        events::Severity severity)
        {
            if (document && document->getSharedLogger())
            {
                document->getSharedLogger()->addEvent({message, severity});
            }
        }

        /// @brief Resolve the gladius function model referenced by a 3MF function resource.
        nodes::SharedModel findFunctionModel(SharedDocument const & document,
                                             Lib3MF::PModel const & model3mf,
                                             Lib3MF::PFunction const & function)
        {
            if (!document || !model3mf || !function)
            {
                return nullptr;
            }
            auto const resourceId =
              io::uniqueResourceIdToResourceId(model3mf, function->GetResourceID());
            return document->getAssembly()->findModel(resourceId);
        }
    } // namespace

    bool VolumeDataTranslucencyView::applyMutation(SharedDocument document,
                                                   std::function<void()> const & mutation)
    {
        try
        {
            // Keep the 3MF model and the graph in sync: write the current graph out,
            // change the volume data, then rebuild the graph from the 3MF model.
            document->update3mfModel();
            mutation();
            document->markFileAsChanged();
            document->updateDocumentFrom3mfModel();
            return true;
        }
        catch (const std::exception & e)
        {
            logUiEvent(document,
                       fmt::format("Failed to update the translucency property: {}", e.what()),
                       events::Severity::Error);
            return false;
        }
    }

    std::optional<VolumeDataTranslucencyView::TranslucencyCandidate>
      VolumeDataTranslucencyView::findTranslucencyCandidate(SharedDocument const & document,
                                                            Lib3MF::PModel const & model3mf)
    {
        if (!document || !model3mf || !document->getAssembly())
        {
            return std::nullopt;
        }

        auto const assemblyModelId = document->getAssembly()->getAssemblyModelId();
        for (const auto & [id, modelNode] : document->getAssembly()->getFunctions())
        {
            // The assembly itself is never a valid source - it is not serialized as a function.
            if (!modelNode || id == assemblyModelId ||
                !nodes::isQualifiedForScalarPropertyChannel(*modelNode))
            {
                continue;
            }

            auto const channelNames = nodes::collectScalarOutputChannelNames(*modelNode);
            if (channelNames.empty())
            {
                continue;
            }

            try
            {
                auto resource = model3mf->GetResourceByID(
                  io::resourceIdToUniqueResourceId(model3mf, modelNode->getResourceId()));
                auto function = std::dynamic_pointer_cast<Lib3MF::CFunction>(resource);
                if (function)
                {
                    return TranslucencyCandidate{function, channelNames.front()};
                }
            }
            catch (...)
            {
                // Function not present in the 3MF model yet - try the next one.
            }
        }
        return std::nullopt;
    }

    bool VolumeDataTranslucencyView::areTranslucencyFunctionsAvailable(
      SharedDocument const & document, Lib3MF::PModel const & model3mf)
    {
        return findTranslucencyCandidate(document, model3mf).has_value();
    }

    bool VolumeDataTranslucencyView::renderTranslucencySection(SharedDocument document,
                                                                Lib3MF::PModel model3mf,
                                                                Lib3MF::PVolumeData volumeData)
    {
        if (!document || !model3mf || !volumeData)
        {
            return false;
        }

        auto const propertyIndex = io::findTranslucencyPropertyIndex(volumeData);

        ImGui::TableNextColumn();
        ImGui::TextUnformatted("Translucency Function");
        ImGui::TableNextColumn();

        if (propertyIndex != io::NO_TRANSLUCENCY_PROPERTY)
        {
            auto propertyData = volumeData->GetProperty(static_cast<Lib3MF_uint32>(propertyIndex));
            if (!propertyData)
            {
                return false;
            }

            bool propertiesChanged =
              renderTranslucencyFunctionDropdown(document, model3mf, volumeData, propertyData);

            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Translucency Channel");
            ImGui::TableNextColumn();

            propertiesChanged |= renderTranslucencyChannelDropdown(document, model3mf, propertyData);
            return propertiesChanged;
        }

        bool const buttonClicked = ImGui::Button("Add Translucency Function");
        if (ImGui::IsItemHovered() && !areTranslucencyFunctionsAvailable(document, model3mf))
        {
            ImGui::SetTooltip("No qualified translucency functions available");
        }
        if (!buttonClicked)
        {
            return false;
        }

        auto const candidate = findTranslucencyCandidate(document, model3mf);
        if (!candidate)
        {
            logUiEvent(document,
                       "Failed to add translucency function: No suitable function found",
                       events::Severity::Warning);
            return false;
        }

        return applyMutation(document, [&]() {
            io::addTranslucencyProperty(volumeData, candidate->function, candidate->channelName);
        });
    }

    bool VolumeDataTranslucencyView::renderTranslucencyFunctionDropdown(
      SharedDocument document, Lib3MF::PModel model3mf, Lib3MF::PVolumeData volumeData,
      Lib3MF::PVolumeDataProperty propertyData)
    {
        bool propertiesChanged = false;

        ImGui::PushID("TranslucencyFunctionDropdown");

        std::string functionDisplayName = "Please select";
        Lib3MF_uint32 currentFunctionId = 0;
        try
        {
            currentFunctionId = propertyData->GetFunctionResourceID();
            if (currentFunctionId != 0)
            {
                auto resource = model3mf->GetResourceByID(currentFunctionId);
                auto currentFunction = std::dynamic_pointer_cast<Lib3MF::CFunction>(resource);
                functionDisplayName = currentFunction
                                        ? currentFunction->GetDisplayName()
                                        : fmt::format("Invalid Function ID: {}", currentFunctionId);
            }
        }
        catch (...)
        {
            functionDisplayName = "Error reading function";
        }

        if (ImGui::BeginCombo("##TranslucencyFunctionCombo", functionDisplayName.c_str()))
        {
            auto const assemblyModelId = document->getAssembly()->getAssemblyModelId();
            for (const auto & [id, modelNode] : document->getAssembly()->getFunctions())
            {
                if (!modelNode || id == assemblyModelId ||
                    !nodes::isQualifiedForScalarPropertyChannel(*modelNode))
                {
                    continue;
                }

                Lib3MF::PFunction functionResource;
                try
                {
                    auto resource = model3mf->GetResourceByID(
                      io::resourceIdToUniqueResourceId(model3mf, modelNode->getResourceId()));
                    functionResource = std::dynamic_pointer_cast<Lib3MF::CFunction>(resource);
                }
                catch (...)
                {
                    continue;
                }
                if (!functionResource)
                {
                    continue;
                }

                bool const isSelected = (currentFunctionId != 0) &&
                                        (functionResource->GetUniqueResourceID() == currentFunctionId);
                auto const itemName = fmt::format("{} (Function #{})",
                                                  functionResource->GetDisplayName(),
                                                  functionResource->GetResourceID());

                if (ImGui::Selectable(itemName.c_str(), isSelected))
                {
                    auto const functionId = functionResource->GetUniqueResourceID();
                    auto const channelNames = nodes::collectScalarOutputChannelNames(*modelNode);
                    auto const fallbackChannel = channelNames.empty()
                                                   ? std::string{nodes::FieldNames::Translucency}
                                                   : channelNames.front();
                    propertiesChanged |= applyMutation(document, [&]() {
                        propertyData->SetFunctionResourceID(functionId);
                        propertyData->SetChannelName(fallbackChannel);
                    });
                }
            }

            if (ImGui::Selectable("[Remove Translucency Function]", false))
            {
                propertiesChanged |= applyMutation(
                  document, [&]() { io::removeTranslucencyProperty(volumeData); });
            }

            ImGui::EndCombo();
        }
        ImGui::PopID();

        return propertiesChanged;
    }

    bool VolumeDataTranslucencyView::renderTranslucencyChannelDropdown(
      SharedDocument document, Lib3MF::PModel model3mf, Lib3MF::PVolumeDataProperty propertyData)
    {
        auto functionId = propertyData->GetFunctionResourceID();
        if (functionId == 0)
        {
            return false;
        }

        Lib3MF::PFunction function;
        try
        {
            auto resource = model3mf->GetResourceByID(functionId);
            function = std::dynamic_pointer_cast<Lib3MF::CFunction>(resource);
        }
        catch (...)
        {
            return false;
        }

        auto functionModel = findFunctionModel(document, model3mf, function);
        if (!functionModel)
        {
            return false;
        }

        auto const channelNames = nodes::collectScalarOutputChannelNames(*functionModel);
        if (channelNames.empty())
        {
            return false;
        }

        bool propertiesChanged = false;
        auto const currentChannel = propertyData->GetChannelName();

        ImGui::PushID("TranslucencyChannelDropdown");
        if (ImGui::BeginCombo("##TranslucencyChannelCombo", currentChannel.c_str()))
        {
            for (auto const & channelName : channelNames)
            {
                bool const isSelected = (channelName == currentChannel);
                if (ImGui::Selectable(channelName.c_str(), isSelected))
                {
                    propertiesChanged |= applyMutation(document,
                                                       [&]() { propertyData->SetChannelName(channelName); });
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
} // namespace gladius::ui
