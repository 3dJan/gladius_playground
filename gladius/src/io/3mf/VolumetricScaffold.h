#pragma once

#include "EventLogger.h"
#include "types.h"

#include <lib3mf_implicit.hpp>

#include <array>
#include <string>

namespace gladius::io
{
    /**
     * @brief Identifiers of the resources a volumetric output chain is made of.
     */
    struct VolumetricScaffoldResult
    {
        bool success{};
        ResourceId levelSetId{};
        ResourceId domainMeshId{};
        std::string channel;
        std::string error;
    };

    /// @brief How a requested output chain relates to what the scene already holds.
    enum class VolumetricOutputChainAction
    {
        Created,      ///< The scene had no output chain, new resources were added.
        Rebound,      ///< An existing level set and domain were pointed at the function.
        AlreadyWired  ///< A level set already evaluates that function and is placed in the scene.
    };

    /// @brief Result of reconciling the scene with a function that should be visible.
    struct VolumetricOutputChainReconciliation : VolumetricScaffoldResult
    {
        VolumetricOutputChainAction action{};
    };

    /**
     * @brief Resolves a function resource by its Gladius ResourceId (lib3mf ModelResourceID).
     * @return The function resource, or nullptr when the id does not reference a function.
     */
    [[nodiscard]] Lib3MF::PFunction findFunctionResource(Lib3MF::PModel const & model,
                                                        ResourceId functionId);

    /**
     * @brief Resolves a function resource by its display name.
     * @details Gladius function ids only line up with lib3mf ids for functions that came from the
     *          file. Ones a snippet added are written to the 3MF model under the next free id, so a
     *          caller that knows the Gladius id still has to find the lib3mf resource by name.
     */
    [[nodiscard]] Lib3MF::PFunction findFunctionResourceByName(Lib3MF::PModel const & model,
                                                              std::string const & displayName);

    /**
     * @brief Adds the resources that make a function renderable: domain mesh, level set, build item.
     * @details A function on its own is never visible. Only a level set that evaluates the function
     *          over a bounding-box domain mesh, plus a build item that puts that level set into the
     *          scene, turns the program into something the renderer and the slicer can consume. Both
     *          the MCP resource tool and the agent's repair path need exactly this, so it lives here
     *          instead of being duplicated.
     */
    /**
     * @brief Makes a function visible without ever duplicating the output chain.
     * @details A seeded document already carries a level set and a build item, and a document the
     *          agent produced usually does once it repaired an earlier attempt. Adding a second chain
     *          renders the geometry twice, so an existing chain is reused or rebound instead.
     */
    [[nodiscard]] VolumetricOutputChainReconciliation reconcileVolumetricOutputChain(
      Lib3MF::PModel const & model,
      Lib3MF::PFunction const & function,
      std::array<float, 3> const & minPoint,
      std::array<float, 3> const & maxPoint,
      events::SharedLogger const & logger = nullptr);

    [[nodiscard]] VolumetricScaffoldResult createVolumetricOutputChain(
      Lib3MF::PModel const & model,
      Lib3MF::PFunction const & function,
      std::array<float, 3> const & minPoint,
      std::array<float, 3> const & maxPoint,
      events::SharedLogger const & logger = nullptr);
}
