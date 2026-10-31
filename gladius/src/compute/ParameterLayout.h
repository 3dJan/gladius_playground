#pragma once

#include "nodes/Parameter.h"

#include <cstdint>
#include <map>
#include <span>
#include <vector>

namespace gladius::compute
{
    /// Stable source-to-storage fan-out for one structurally compatible evaluator.
    class ParameterLayout
    {
      public:
        struct Destination
        {
            std::size_t offset{};
            std::type_index type{nodes::ParameterTypeIndex::Float};
        };

        explicit ParameterLayout(std::uint64_t sceneGeneration);
        void add(nodes::ParameterOrigin origin, std::size_t offset, std::type_index type);
        [[nodiscard]] std::span<Destination const> getDestinations(nodes::ParameterOrigin origin) const;

        /// Validate every destination before writing; incompatible updates leave storage intact.
        [[nodiscard]] bool apply(nodes::ParameterOrigin origin,
                                 nodes::VariantType const & value,
                                 std::uint64_t sceneGeneration,
                                 std::vector<float> & values) const;

      private:
        std::uint64_t m_sceneGeneration{};
        std::map<nodes::ParameterOrigin, std::vector<Destination>> m_destinations;
    };
}