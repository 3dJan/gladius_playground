#pragma once

#include <cstddef>
#include <string>
#include <lib3mf_implicit.hpp>

namespace gladius::io
{
    /// @brief Sentinel returned when a volume data has no translucency property.
    inline auto constexpr NO_TRANSLUCENCY_PROPERTY = std::size_t(-1);

    /**
     * @brief Helpers for the translucency volumetric property.
     *
     * lib3mf cannot serialize the dedicated @c \<v:translucency\> element of the 3MF
     * volumetric extension yet, but it round-trips the generic @c \<v:property\>
     * element. Translucency is therefore stored as a property named
     * @c FieldNames::Translucency referencing an implicit function plus the scalar
     * output channel that carries the ISO/TS 19307 alpha.
     *
     * @{
     */

    /**
     * @brief Find the index of the translucency property of a volume data.
     *
     * @param volumeData The volume data to inspect
     * @return the property index, or NO_TRANSLUCENCY_PROPERTY when there is none
     */
    [[nodiscard]] std::size_t
      findTranslucencyPropertyIndex(Lib3MF::PVolumeData const & volumeData);

    /**
     * @brief Find the translucency property of a volume data.
     *
     * @param volumeData The volume data to inspect
     * @return the property named "translucency", or nullptr when there is none
     */
    [[nodiscard]] Lib3MF::PVolumeDataProperty
      findTranslucencyProperty(Lib3MF::PVolumeData const & volumeData);

    /**
     * @brief Add a translucency property referencing the given function.
     *
     * @param volumeData The volume data to modify
     * @param function The 3MF function resource providing the scalar channel
     * @param channelName Name of the scalar output channel to use
     * @return the new property, or nullptr if the volume data already has one
     */
    [[nodiscard]] Lib3MF::PVolumeDataProperty
      addTranslucencyProperty(Lib3MF::PVolumeData const & volumeData,
                              Lib3MF::PFunction const & function,
                              std::string const & channelName);

    /**
     * @brief Remove the translucency property of a volume data.
     *
     * @param volumeData The volume data to modify
     * @return true if a property was removed
     */
    bool removeTranslucencyProperty(Lib3MF::PVolumeData const & volumeData);

    /** @} */
} // namespace gladius::io
