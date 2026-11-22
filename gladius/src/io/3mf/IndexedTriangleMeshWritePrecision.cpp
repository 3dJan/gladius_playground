#include "io/3mf/IndexedTriangleMeshWritePrecision.h"

#include <cmath>
#include <cstddef>

namespace
{
    /// @brief lib3mf converges after two rounds; the extra passes only guard extreme magnitudes.
    constexpr std::size_t kMaximumSnapPasses = 4U;

    /// @brief Replays lib3mf's scaled conversion: the value is scaled in float32 and unscaled in
    /// double precision. Unscaled with a float reciprocal the result is wrong by an ULP already.
    [[nodiscard]] float applyWriteQuantisation(float value, float decimalScale)
    {
        float const scaled = value * decimalScale;
        if (!std::isfinite(scaled))
        {
            return value;
        }
        return static_cast<float>(static_cast<double>(scaled) / static_cast<double>(decimalScale));
    }
}

namespace gladius::io
{
    float snapCoordinateTo3mfWritePrecision(float value, std::size_t decimalPrecision)
    {
        if (!std::isfinite(value) || decimalPrecision > 15U)
        {
            return value;
        }

        float decimalScale = 1.0f;
        for (std::size_t digit = 0U; digit < decimalPrecision; ++digit)
        {
            decimalScale *= 10.0f;
        }

        float snapped = value;
        for (std::size_t pass = 0U; pass < kMaximumSnapPasses; ++pass)
        {
            float const next = applyWriteQuantisation(snapped, decimalScale);
            if (next == snapped)
            {
                break;
            }
            snapped = next;
        }
        return snapped;
    }

    std::size_t snapMeshTo3mfWritePrecision(IndexedTriangleMesh& mesh, std::size_t decimalPrecision)
    {
        std::size_t changedComponentCount = 0U;
        for (auto& position : mesh.positions)
        {
            for (Eigen::Index component = 0; component < 3; ++component)
            {
                float const& value = position[component];
                float const snapped = snapCoordinateTo3mfWritePrecision(value, decimalPrecision);
                if (snapped != value)
                {
                    position[component] = snapped;
                    ++changedComponentCount;
                }
            }
        }
        return changedComponentCount;
    }
}
