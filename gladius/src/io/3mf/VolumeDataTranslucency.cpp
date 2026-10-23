#include "VolumeDataTranslucency.h"

#include "nodes/nodesfwd.h"

namespace gladius::io
{
    std::size_t findTranslucencyPropertyIndex(Lib3MF::PVolumeData const & volumeData)
    {
        if (!volumeData)
        {
            return NO_TRANSLUCENCY_PROPERTY;
        }

        auto const propertyCount = volumeData->GetPropertyCount();
        for (auto propertyIndex = 0u; propertyIndex < propertyCount; ++propertyIndex)
        {
            auto property = volumeData->GetProperty(propertyIndex);
            if (property && property->GetName() == nodes::FieldNames::Translucency)
            {
                return propertyIndex;
            }
        }
        return NO_TRANSLUCENCY_PROPERTY;
    }

    Lib3MF::PVolumeDataProperty findTranslucencyProperty(Lib3MF::PVolumeData const & volumeData)
    {
        auto const propertyIndex = findTranslucencyPropertyIndex(volumeData);
        if (propertyIndex == NO_TRANSLUCENCY_PROPERTY)
        {
            return nullptr;
        }
        return volumeData->GetProperty(static_cast<Lib3MF_uint32>(propertyIndex));
    }

    Lib3MF::PVolumeDataProperty addTranslucencyProperty(Lib3MF::PVolumeData const & volumeData,
                                                        Lib3MF::PFunction const & function,
                                                        std::string const & channelName)
    {
        if (!volumeData || !function ||
            findTranslucencyPropertyIndex(volumeData) != NO_TRANSLUCENCY_PROPERTY)
        {
            return nullptr;
        }

        auto property = volumeData->AddPropertyFromFunction(nodes::FieldNames::Translucency,
                                                            function.get());
        if (!property)
        {
            return nullptr;
        }
        property->SetChannelName(channelName);
        property->SetIsRequired(false);
        return property;
    }

    bool removeTranslucencyProperty(Lib3MF::PVolumeData const & volumeData)
    {
        auto const propertyIndex = findTranslucencyPropertyIndex(volumeData);
        if (propertyIndex == NO_TRANSLUCENCY_PROPERTY)
        {
            return false;
        }
        volumeData->RemoveProperty(static_cast<Lib3MF_uint32>(propertyIndex));
        return true;
    }
} // namespace gladius::io
