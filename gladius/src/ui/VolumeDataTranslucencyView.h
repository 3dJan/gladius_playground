#pragma once

#include "Document.h"
#include "imgui.h"
#include <functional>
#include <lib3mf_implicit.hpp>
#include <optional>
#include <string>
#include <utility>

namespace gladius::ui
{
    /**
     * @brief UI for selecting the translucency channel of a VolumeData resource.
     *
     * Translucency is stored as a generic 3MF volumetric property named
     * "translucency" that references an implicit function and one of its scalar
     * output channels, mirroring how the color function and channel are selected.
     */
    class VolumeDataTranslucencyView
    {
      public:
        /**
         * @brief Renders the translucency rows of the volume data property table.
         *
         * Renders either the function/channel dropdowns of an existing translucency
         * property, or a button that creates one.
         *
         * @param document The document whose assembly provides the functions
         * @param model3mf The 3MF model holding the volume data and function resources
         * @param volumeData The volume data to modify
         * @return true if the translucency property was modified
         */
        static bool renderTranslucencySection(SharedDocument document,
                                              Lib3MF::PModel model3mf,
                                              Lib3MF::PVolumeData volumeData);

        /**
         * @brief Checks whether any function can provide a translucency channel.
         *
         * A function qualifies if it exposes a float3 "pos" input and at least one
         * scalar output, and it exists as a function resource in the 3MF model.
         */
        static bool areTranslucencyFunctionsAvailable(SharedDocument const & document,
                                                      Lib3MF::PModel const & model3mf);

      private:
        /**
         * @brief A function that can feed the translucency channel plus a suggested channel.
         */
        struct TranslucencyCandidate
        {
            Lib3MF::PFunction function;
            std::string channelName;
        };

        /**
         * @brief Find the first function usable as a translucency source.
         *
         * @param document The document whose assembly provides the functions
         * @param model3mf The 3MF model holding the function resources
         * @return the candidate function and a channel to select, if any exists
         */
        [[nodiscard]] static std::optional<TranslucencyCandidate>
          findTranslucencyCandidate(SharedDocument const & document,
                                    Lib3MF::PModel const & model3mf);

        /**
         * @brief Renders the dropdown selecting the function behind the translucency property.
         *
         * @return true if the referenced function was changed or the property removed
         */
        static bool renderTranslucencyFunctionDropdown(SharedDocument document,
                                                       Lib3MF::PModel model3mf,
                                                       Lib3MF::PVolumeData volumeData,
                                                       Lib3MF::PVolumeDataProperty propertyData);

        /**
         * @brief Renders the dropdown selecting the scalar output channel.
         *
         * @return true if the selected channel was changed
         */
        static bool renderTranslucencyChannelDropdown(SharedDocument document,
                                                      Lib3MF::PModel model3mf,
                                                      Lib3MF::PVolumeDataProperty propertyData);

        /**
         * @brief Applies a 3MF mutation to the document and rebuilds the graph from it.
         *
         * @return true when the mutation succeeded
         */
        static bool applyMutation(SharedDocument document,
                                  std::function<void()> const & mutation);
    };
} // namespace gladius::ui
