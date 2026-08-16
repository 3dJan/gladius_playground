#pragma once

#include <Eigen/Core>
#include <Eigen/src/Core/Matrix.h>

#include "ComputeTypes.h"
#include "nodes/Model.h"

#include <vector>

namespace gladius_tests
{
    using namespace gladius;
    using float3 = Eigen::Vector3f;
    using float2 = Eigen::Vector2f;
    using ShapeFunction = std::function<float(float3)>;
    namespace helper
    {
        template <typename T>
        auto countNumberOfNodesOfType(nodes::Model & model)
        {
            int count = 0;
            auto visitor = gladius::nodes::OnTypeVisitor<T>([&](T &) { ++count; });
            model.visitNodes(visitor);
            return count;
        }

        auto sphere(float3 pos, float radius) -> float;
        auto testModel(float3 pos) -> float;
        auto testModel2(float3 pos) -> float;

        inline auto hashValue(cl_float4 const & value) -> size_t
        {
            size_t hash = 0;
            std::hash<float> hasher;
            for (auto component : value.s)
            {
                hash ^= hasher(component) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            }
            return hash;
        }

        template <typename T>
        auto hashValue(T const & value) -> size_t
        {
            return std::hash<T>{}(value);
        }

        template <typename Iterator>
        auto computeHash(Iterator cbegin, Iterator cend)
        {
            size_t hash = 0;
            for (auto it = cbegin; it != cend; ++it)
            {
                hash ^= hashValue(*it) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
            }
            return hash;
        }
    }
}
