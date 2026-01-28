#include "FunctionalEquality.h"
#include "DerivedNodes.h"
#include "Model.h"
#include <cmath>
#include <functional>
#include <typeindex>
#include <variant>

namespace gladius::nodes
{
    double FunctionalEquality::s_epsilon = 1e-6;

    namespace
    {
        /// @brief Combines hash values (boost::hash_combine pattern)
        inline void hashCombine(size_t & seed, size_t value)
        {
            seed ^= value + 0x9e3779b9 + (seed << 6) + (seed >> 2);
        }

        /// @brief Quantizes a float to reduce sensitivity to small differences
        /// This makes the hash stable for values within epsilon of each other
        inline float quantizeFloat(float value, double epsilon)
        {
            // Round to nearest multiple of epsilon to ensure similar values hash the same
            double const invEps = 1.0 / epsilon;
            return static_cast<float>(std::round(static_cast<double>(value) * invEps) / invEps);
        }

        /// @brief Hashes a float value with epsilon-aware quantization
        inline size_t hashFloat(float value, double epsilon)
        {
            return std::hash<float>{}(quantizeFloat(value, epsilon));
        }

        /// @brief Hashes a string
        inline size_t hashString(std::string const & value)
        {
            return std::hash<std::string>{}(value);
        }

        /// @brief Extracts the constant float value from a parameter variant
        std::optional<float> getFloatValue(VariantParameter const & param)
        {
            auto value = param.getValue();
            if (auto const * floatVal = std::get_if<float>(&value))
            {
                return *floatVal;
            }
            return std::nullopt;
        }

        /// @brief Extracts the ResourceId value from a parameter variant
        std::optional<ResourceId> getResourceIdValue(VariantParameter const & param)
        {
            auto value = param.getValue();
            if (auto const * resId = std::get_if<ResourceId>(&value))
            {
                return *resId;
            }
            return std::nullopt;
        }

        /// @brief Computes hash for a single node based on type and constant values
        size_t hashNode(NodeBase const & node, double epsilon)
        {
            size_t hash = 0;

            // Hash the node type name
            hashCombine(hash, hashString(node.name()));

            // Hash constant values (for nodes that have them)
            for (auto const & [paramName, param] : node.constParameter())
            {
                // Only hash parameters that don't require input (i.e., constant values)
                if (!param.isInputSourceRequired())
                {
                    if (auto floatVal = getFloatValue(param))
                    {
                        hashCombine(hash, hashFloat(*floatVal, epsilon));
                    }
                    else if (auto resId = getResourceIdValue(param))
                    {
                        hashCombine(hash, std::hash<ResourceId>{}(*resId));
                    }
                }
            }

            return hash;
        }

        /// @brief Compares floats with relative epsilon tolerance
        bool floatsEqual(float a, float b, double epsilon)
        {
            if (a == b)
            {
                return true; // Handles infinities and exact matches
            }

            float const diff = std::abs(a - b);
            float const maxVal = std::max(std::abs(a), std::abs(b));

            // Use relative epsilon for large values, absolute for small
            if (maxVal < 1.0f)
            {
                return diff < static_cast<float>(epsilon);
            }
            return diff < maxVal * static_cast<float>(epsilon);
        }

        /// @brief Compares two parameters for equality
        bool parametersEqual(VariantParameter const & lhs,
                             VariantParameter const & rhs,
                             double epsilon)
        {
            // Check if both require input (connected)
            if (lhs.isInputSourceRequired() != rhs.isInputSourceRequired())
            {
                return false;
            }

            // If both require input, they're connected - connectivity checked elsewhere
            if (lhs.isInputSourceRequired())
            {
                return true;
            }

            // Compare constant values
            auto lhsVal = lhs.getValue();
            auto rhsVal = rhs.getValue();

            // Must be same type
            if (lhsVal.index() != rhsVal.index())
            {
                return false;
            }

            // Compare float values with epsilon
            if (auto const * lhsFloat = std::get_if<float>(&lhsVal))
            {
                auto const * rhsFloat = std::get_if<float>(&rhsVal);
                return rhsFloat && floatsEqual(*lhsFloat, *rhsFloat, epsilon);
            }

            // Compare ResourceId values
            if (auto const * lhsResId = std::get_if<ResourceId>(&lhsVal))
            {
                auto const * rhsResId = std::get_if<ResourceId>(&rhsVal);
                return rhsResId && (*lhsResId == *rhsResId);
            }

            // Compare int values
            if (auto const * lhsInt = std::get_if<int>(&lhsVal))
            {
                auto const * rhsInt = std::get_if<int>(&rhsVal);
                return rhsInt && (*lhsInt == *rhsInt);
            }

            // Compare string values
            if (auto const * lhsStr = std::get_if<std::string>(&lhsVal))
            {
                auto const * rhsStr = std::get_if<std::string>(&rhsVal);
                return rhsStr && (*lhsStr == *rhsStr);
            }

            // Compare float3 values with epsilon
            if (auto const * lhsF3 = std::get_if<float3>(&lhsVal))
            {
                auto const * rhsF3 = std::get_if<float3>(&rhsVal);
                if (!rhsF3)
                {
                    return false;
                }
                return floatsEqual(lhsF3->x, rhsF3->x, epsilon) &&
                       floatsEqual(lhsF3->y, rhsF3->y, epsilon) &&
                       floatsEqual(lhsF3->z, rhsF3->z, epsilon);
            }

            // Compare Matrix4x4 values with epsilon
            if (auto const * lhsMat = std::get_if<Matrix4x4>(&lhsVal))
            {
                auto const * rhsMat = std::get_if<Matrix4x4>(&rhsVal);
                if (!rhsMat)
                {
                    return false;
                }
                for (size_t i = 0; i < 4; ++i)
                {
                    for (size_t j = 0; j < 4; ++j)
                    {
                        if (!floatsEqual((*lhsMat)[i][j], (*rhsMat)[i][j], epsilon))
                        {
                            return false;
                        }
                    }
                }
                return true;
            }

            // Compare ResourceKey values
            if (auto const * lhsKey = std::get_if<ResourceKey>(&lhsVal))
            {
                auto const * rhsKey = std::get_if<ResourceKey>(&rhsVal);
                return rhsKey && (*lhsKey == *rhsKey);
            }

            // Fallback: same index means same type, and we've handled all known types
            return true;
        }

        /// @brief Compares two nodes for structural equality (T018)
        bool nodesEqual(NodeBase const & lhs, NodeBase const & rhs, double epsilon)
        {
            // Compare node types
            if (lhs.name() != rhs.name())
            {
                return false;
            }

            // Compare parameter counts
            auto const & lhsParams = lhs.constParameter();
            auto const & rhsParams = rhs.constParameter();

            if (lhsParams.size() != rhsParams.size())
            {
                return false;
            }

            // Compare each parameter (T019 - constants with epsilon)
            for (auto const & [paramName, lhsParam] : lhsParams)
            {
                auto rhsIt = rhsParams.find(paramName);
                if (rhsIt == rhsParams.end())
                {
                    return false;
                }

                if (!parametersEqual(lhsParam, rhsIt->second, epsilon))
                {
                    return false;
                }
            }

            return true;
        }
    } // anonymous namespace

    size_t FunctionalEquality::computeHash(Model const & model)
    {
        size_t hash = 0;

        // Hash node count
        hashCombine(hash, model.getSize());

        // Hash each node in topological order
        auto const & order = model.getOutputOrder();
        for (auto nodeId : order)
        {
            auto nodeOpt = model.getNode(nodeId);
            if (nodeOpt.has_value())
            {
                hashCombine(hash, hashNode(**nodeOpt, s_epsilon));
            }
        }

        return hash;
    }

    bool FunctionalEquality::areEqual(Model const & lhs, Model const & rhs)
    {
        // Quick reject: different sizes
        if (lhs.getSize() != rhs.getSize())
        {
            return false;
        }

        // Quick reject: different hashes (hash collision possible, so not conclusive)
        if (computeHash(lhs) != computeHash(rhs))
        {
            return false;
        }

        // Get topological orders
        auto const & lhsOrder = lhs.getOutputOrder();
        auto const & rhsOrder = rhs.getOutputOrder();

        // Must have same number of reachable nodes
        if (lhsOrder.size() != rhsOrder.size())
        {
            return false;
        }

        // Compare nodes in topological order (T020 - topology comparison)
        for (size_t i = 0; i < lhsOrder.size(); ++i)
        {
            auto lhsNodeOpt = lhs.getNode(lhsOrder[i]);
            auto rhsNodeOpt = rhs.getNode(rhsOrder[i]);

            // If both nodes are missing (phantom node IDs in order), that's OK
            if (!lhsNodeOpt.has_value() && !rhsNodeOpt.has_value())
            {
                continue;
            }

            // If only one is missing, they're different
            if (!lhsNodeOpt.has_value() || !rhsNodeOpt.has_value())
            {
                return false;
            }

            if (!nodesEqual(**lhsNodeOpt, **rhsNodeOpt, s_epsilon))
            {
                return false;
            }
        }

        return true;
    }

    void FunctionalEquality::setEpsilon(double epsilon)
    {
        s_epsilon = epsilon;
    }

    double FunctionalEquality::getEpsilon()
    {
        return s_epsilon;
    }
} // namespace gladius::nodes
