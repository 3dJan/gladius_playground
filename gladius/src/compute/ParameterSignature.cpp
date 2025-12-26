#include "ParameterSignature.h"

#include <fmt/format.h>
#include <functional>
#include <sstream>

#include "../nodes/Assembly.h"
#include "../nodes/Parameter.h"

namespace gladius
{
    bool ParameterSignature::matches(ParameterSignature const & other) const
    {
        // Fast path: compare hash first
        if (signatureHash != other.signatureHash)
        {
            return false;
        }

        // Hash matched - do full comparison for safety against collisions
        if (totalFloatCount != other.totalFloatCount)
        {
            return false;
        }

        // Compare parameter sizes vector
        if (parameterSizes.size() != other.parameterSizes.size())
        {
            return false;
        }

        for (size_t i = 0; i < parameterSizes.size(); ++i)
        {
            if (parameterSizes[i] != other.parameterSizes[i])
            {
                return false;
            }
        }

        return true;
    }

    ParameterSignature ParameterSignature::compute(nodes::Assembly const & assembly)
    {
        ParameterSignature sig{};
        std::stringstream signatureStream;

        // Walk through all functions and parameters in the same order as
        // updateParameterBlocking
        for (auto const & model : assembly.getFunctions())
        {
            if (!model.second)
            {
                continue;
            }

            for (auto const [id, param] : model.second->getParameterRegistry())
            {
                if (param == nullptr || param->getId() != id)
                {
                    continue;
                }

                auto const * varParam = dynamic_cast<nodes::VariantParameter const *>(param);
                if (varParam == nullptr)
                {
                    continue;
                }

                // Only count parameters that don't have a source (i.e., are not computed
                // from other nodes)
                if (!varParam->getConstSource().has_value())
                {
                    auto const & variant = varParam->Value();

                    // Determine parameter size and type
                    if (std::holds_alternative<float>(variant))
                    {
                        sig.parameterSizes.push_back(1);
                        sig.totalFloatCount += 1;
                        signatureStream << "f1;";
                    }
                    else if (std::holds_alternative<int>(variant))
                    {
                        sig.parameterSizes.push_back(1);
                        sig.totalFloatCount += 1;
                        signatureStream << "i1;";
                    }
                    else if (std::holds_alternative<nodes::float3>(variant))
                    {
                        sig.parameterSizes.push_back(3);
                        sig.totalFloatCount += 3;
                        signatureStream << "f3;";
                    }
                    else if (std::holds_alternative<nodes::Matrix4x4>(variant))
                    {
                        sig.parameterSizes.push_back(16);
                        sig.totalFloatCount += 16;
                        signatureStream << "m44;";
                    }
                    // If new types are added, they should be handled here
                }
            }
        }

        // Compute hash from the signature string
        std::string const signatureStr = signatureStream.str();
        std::hash<std::string> hasher;
        sig.signatureHash = hasher(signatureStr);

        return sig;
    }

    ParameterSignature ParameterSignature::empty()
    {
        return ParameterSignature{};
    }

    bool ParameterSignature::isValid() const
    {
        return totalFloatCount > 0 || !parameterSizes.empty();
    }

    std::string ParameterSignature::toString() const
    {
        if (!isValid())
        {
            return "[Empty ParameterSignature]";
        }

        std::stringstream ss;
        ss << fmt::format("ParameterSignature[totalFloats={}, params={}, hash=0x{:016x}]",
                          totalFloatCount,
                          parameterSizes.size(),
                          signatureHash);
        return ss.str();
    }

    uint64_t ParameterSignature::computeHash(size_t totalFloats,
                                            std::vector<size_t> const & sizes)
    {
        // Combine totalFloats with each parameter size using FNV-1a hash
        uint64_t hash = 14695981039346656037ULL; // FNV offset basis
        constexpr uint64_t fnvPrime = 1099511628211ULL;

        // Hash totalFloats
        hash ^= totalFloats;
        hash *= fnvPrime;

        // Hash each parameter size
        for (size_t const size : sizes)
        {
            hash ^= size;
            hash *= fnvPrime;
        }

        return hash;
    }
}
