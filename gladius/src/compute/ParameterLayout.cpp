#include "ParameterLayout.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace gladius::compute
{
    ParameterLayout::ParameterLayout(std::uint64_t const sceneGeneration)
        : m_sceneGeneration(sceneGeneration)
    {
        if (sceneGeneration == 0u)
        {
            throw std::invalid_argument("Parameter layout requires a structural generation");
        }
    }

    void ParameterLayout::add(nodes::ParameterOrigin const origin,
                              std::size_t const offset,
                              std::type_index const type)
    {
        if (type != nodes::ParameterTypeIndex::Float && type != nodes::ParameterTypeIndex::Float3 &&
            type != nodes::ParameterTypeIndex::Matrix4)
        {
            throw std::invalid_argument("Parameter layout supports only float, float3, and Matrix4x4");
        }
        auto & destinations = m_destinations[origin];
        if (!destinations.empty() && destinations.front().type != type)
        {
            throw std::invalid_argument("Parameter origin has incompatible destination types");
        }
        destinations.push_back({offset, type});
    }

    std::span<ParameterLayout::Destination const> ParameterLayout::getDestinations(
      nodes::ParameterOrigin const origin) const
    {
        auto const iterator = m_destinations.find(origin);
        return iterator == m_destinations.end() ? std::span<Destination const>{} : iterator->second;
    }

    bool ParameterLayout::apply(nodes::ParameterOrigin const origin,
                                nodes::VariantType const & value,
                                std::uint64_t const sceneGeneration,
                                std::vector<float> & values) const
    {
        if (sceneGeneration != m_sceneGeneration)
        {
            return false;
        }
        auto const destinations = getDestinations(origin);
        if (destinations.empty())
        {
            return false;
        }

        std::array<float, 16> packed{};
        std::size_t count{};
        auto const type = std::type_index(nodes::getVariantType(value));
        if (auto const * scalar = std::get_if<float>(&value))
        {
            packed[0] = *scalar;
            count = 1u;
        }
        else if (auto const * vector = std::get_if<nodes::float3>(&value))
        {
            packed[0] = vector->x;
            packed[1] = vector->y;
            packed[2] = vector->z;
            count = 3u;
        }
        else if (auto const * matrix = std::get_if<nodes::Matrix4x4>(&value))
        {
            count = 16u;
            for (std::size_t row = 0u; row < 4u; ++row)
            {
                for (std::size_t column = 0u; column < 4u; ++column)
                {
                    packed[row * 4u + column] = (*matrix)[row][column];
                }
            }
        }
        else
        {
            return false;
        }

        if (!std::all_of(packed.begin(), packed.begin() + count,
                         [](float const component) { return std::isfinite(component); }))
        {
            return false;
        }
        for (auto const & destination : destinations)
        {
            if (destination.type != type || destination.offset > values.size() ||
                count > values.size() - destination.offset)
            {
                return false;
            }
        }
        for (auto const & destination : destinations)
        {
            std::copy_n(packed.begin(), count, values.begin() + destination.offset);
        }
        return true;
    }
}