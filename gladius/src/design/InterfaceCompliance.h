#pragma once

#include "DesignTypes.h"

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace gladius::design
{
    /**
     * @brief Result of comparing one pair of mating interface specifications.
     */
    struct InterfaceCompliance
    {
        bool compatible = false;
        /// @brief Keys that disagree, with both values, for the redesign request the orchestrator writes
        std::vector<std::string> mismatches;
        /// @brief Keys only one side declared; compatibility never depends on them
        std::vector<std::string> unconstrainedKeys;

        [[nodiscard]] std::string describe() const;
    };

    /**
     * @brief Tolerance used when two numeric interface values are considered the same fit.
     * @details Implicit-CAD interfaces are declared by a language model, so the same bore written
     *          as 4.8 and 4.8000001 is the same intent. Anything looser stops being an assembly fit.
     */
    inline constexpr double kInterfaceNumericTolerance = 1e-3;

    /**
     * @brief Compares two interface specifications key by key.
     * @details Numeric values match within `tolerance`. An `envelope` key additionally tolerates a
     *          smaller envelope inside a larger one, because a component that fits strictly within
     *          the space its parent reserved satisfies the contract. All other value types must be
     *          equal; there is no sensible way to relax a frame string or an axis enum.
     */
    [[nodiscard]] InterfaceCompliance compareInterfaceSpecifications(nlohmann::json const & a,
                                                                    nlohmann::json const & b,
                                                                    double tolerance = kInterfaceNumericTolerance);

    /**
     * @brief Checks every interface `candidate` shares a name and kind with.
     * @details Interfaces declared by only one side are not a mismatch: they constrain nothing that
     *          the other side must honour.
     */
    [[nodiscard]] InterfaceCompliance checkInterfacePair(Interface const & a, Interface const & b);

    /**
     * @brief Checks `candidate` against every selected variant of `otherNodeIds`.
     * @return Empty vector when all mating interfaces are compatible.
     */
    [[nodiscard]] std::vector<InterfaceCompliance> checkInterfacesAgainstConfiguration(
      std::vector<Interface> const & candidate,
      std::vector<Interface> const & reference);

    /**
     * @brief Applies each variant's interface keys to its parent's reserved envelope.
     * @details A child declared inside a parent must not stick out of the volume the parent reserved
     *          for it; that is the containment half of implicit-CAD compatibility.
     * @param envelope Parent specification, expected to carry `envelope` as `[minx,miny,minz,maxx,maxy,maxz]`
     * @param child Child specification carrying its own `envelope`, if any
     */
    [[nodiscard]] InterfaceCompliance checkEnvelopeContainment(nlohmann::json const & envelope,
                                                              nlohmann::json const & child);

    /**
     * @brief Applies every parent interface's reserved envelope to the matching child interfaces.
     * @details Interfaces are paired by name and kind; unpaired ones constrain nothing. Used both by
     *          the `interface` gate and by `DesignGraph::substituteVariant`, so a variant swap can
     *          never silently break the space its parent reserved.
     */
    [[nodiscard]] InterfaceCompliance checkEnvelopeContainmentBetween(std::vector<Interface> const & parent,
                                                                     std::vector<Interface> const & child);

    /**
     * @brief Deterministic evidence for one checklist entry of kind `interface`.
     * @details Writes measured keys and decisions to `evidence` and returns true when the entry may
     *          be checked by `TodoStore::checkByGate`.
     */
    [[nodiscard]] bool evaluateInterfaceChecklistEntry(std::vector<Interface> const & candidate,
                                                      std::vector<Interface> const & reference,
                                                      nlohmann::json & evidence);
} // namespace gladius::design
