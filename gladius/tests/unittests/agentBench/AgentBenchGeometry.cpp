#include "AgentBenchGeometry.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <vector>

namespace gladius::agent::bench
{
    namespace
    {
        using Vec3 = std::array<double, 3>;

        constexpr std::size_t kStlHeaderBytes = 80u;
        constexpr std::size_t kStlCountBytes = 4u;
        constexpr std::size_t kStlTriangleBytes = 50u;

        [[nodiscard]] Vec3 readVec3(std::vector<char> const & buf, std::size_t offset)
        {
            Vec3 out{};
            for (std::size_t i = 0; i < 3u; ++i)
            {
                float value = 0.0f;
                std::memcpy(&value, buf.data() + static_cast<std::ptrdiff_t>(offset + i * 4u), 4u);
                out[i] = static_cast<double>(value);
            }
            return out;
        }

        [[nodiscard]] std::uint32_t readUInt32(std::vector<char> const & buf, std::size_t offset)
        {
            std::uint32_t value = 0u;
            std::memcpy(&value, buf.data() + static_cast<std::ptrdiff_t>(offset), 4u);
            return value;
        }

        [[nodiscard]] Vec3 crossProduct(Vec3 const & a, Vec3 const & b)
        {
            return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
        }

        [[nodiscard]] double dotProduct(Vec3 const & a, Vec3 const & b)
        {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        void expandBox(std::array<double, 3> & lo, std::array<double, 3> & hi, Vec3 const & v)
        {
            for (std::size_t axis = 0; axis < 3u; ++axis)
            {
                lo[axis] = std::min(lo[axis], v[axis]);
                hi[axis] = std::max(hi[axis], v[axis]);
            }
        }

        [[nodiscard]] std::string rangeText(double value, Range const & range)
        {
            return std::to_string(value) + " not in [" + std::to_string(range.min) + ", "
                   + std::to_string(range.max) + "]";
        }
    } // namespace

    std::array<double, 3> GeometryFacts::axisSpans() const
    {
        // The mesh is the authority when available; bounds only fill in for a failed export.
        auto const & lo = meshValid ? meshMin : boundsMin;
        auto const & hi = meshValid ? meshMax : boundsMax;
        return {hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]};
    }

    void GeometryFacts::updateDerivedFacts()
    {
        boundingBoxVolumeMm3 = (meshMax[0] - meshMin[0]) * (meshMax[1] - meshMin[1])
                               * (meshMax[2] - meshMin[2]);
        fillFraction = boundingBoxVolumeMm3 > 0.0 ? volumeMm3 / boundingBoxVolumeMm3 : 0.0;
    }

    nlohmann::json GeometryFacts::toJson() const
    {
        return {{"boundsUsable", boundsUsable},
                {"boundsMin", boundsMin},
                {"boundsMax", boundsMax},
                {"meshValid", meshValid},
                {"triangleCount", triangleCount},
                {"volumeMm3", volumeMm3},
                {"meshMin", meshMin},
                {"meshMax", meshMax},
                {"boundingBoxVolumeMm3", boundingBoxVolumeMm3},
                {"fillFraction", fillFraction},
                {"axisSpans", axisSpans()},
                {"error", error}};
    }

    GeometryFacts measureStl(std::string const & filePath)
    {
        GeometryFacts facts;
        std::ifstream file(filePath, std::ios::binary);
        if (!file)
        {
            facts.error = "STL not readable: " + filePath;
            return facts;
        }
        std::vector<char> buf((std::istreambuf_iterator<char>(file)),
                              std::istreambuf_iterator<char>());
        if (buf.size() < kStlHeaderBytes + kStlCountBytes)
        {
            facts.error = "STL too small";
            return facts;
        }

        std::size_t const triangleCount = (buf.size() - kStlHeaderBytes - kStlCountBytes)
                                          / kStlTriangleBytes;
        if (readUInt32(buf, kStlHeaderBytes) != triangleCount || triangleCount == 0u)
        {
            facts.error = "STL triangle count mismatch or empty mesh";
            return facts;
        }

        facts.meshValid = true;
        facts.triangleCount = triangleCount;
        facts.meshMin = {std::numeric_limits<double>::max(),
                         std::numeric_limits<double>::max(),
                         std::numeric_limits<double>::max()};
        facts.meshMax = {std::numeric_limits<double>::lowest(),
                         std::numeric_limits<double>::lowest(),
                         std::numeric_limits<double>::lowest()};

        double signedVolume = 0.0;
        for (std::size_t i = 0; i < triangleCount; ++i)
        {
            std::size_t const base = kStlHeaderBytes + kStlCountBytes + i * kStlTriangleBytes + 12u;
            auto const v0 = readVec3(buf, base);
            auto const v1 = readVec3(buf, base + 12u);
            auto const v2 = readVec3(buf, base + 24u);
            signedVolume += dotProduct(v0, crossProduct(v1, v2)) / 6.0;
            expandBox(facts.meshMin, facts.meshMax, v0);
            expandBox(facts.meshMin, facts.meshMax, v1);
            expandBox(facts.meshMin, facts.meshMax, v2);
        }
        facts.volumeMm3 = std::abs(signedVolume);
        facts.updateDerivedFacts();
        return facts;
    }

    std::vector<GeometryCheck> evaluateGeometry(AgentBenchTask const & task,
                                                GeometryFacts const & facts)
    {
        std::vector<GeometryCheck> checks;
        if (!facts.error.empty() || !facts.meshValid)
        {
            checks.push_back({"mesh_export",
                              false,
                              facts.error.empty() ? "No mesh exported" : facts.error});
            return checks;
        }

        char const * const spanNames[] = {"min", "mid", "max"};
        // The agent may orient the part any way it likes, so both the measured spans and the
        // accepted ranges are compared in ascending order.
        auto spans = facts.axisSpans();
        std::sort(spans.begin(), spans.end());
        auto expected = task.geometry.axisSpanMm;
        std::sort(expected.begin(), expected.end(), [](Range const & a, Range const & b)
        {
            return a.min < b.min;
        });
        for (std::size_t i = 0; i < spans.size(); ++i)
        {
            auto const & range = expected[i];
            if (range.isDisabled())
            {
                continue;
            }
            bool const ok = range.contains(spans[i]);
            checks.push_back({std::string{spanNames[i]} + "_span_mm",
                              ok,
                              ok ? std::to_string(spans[i]) : rangeText(spans[i], range)});
        }

        if (!task.geometry.volumeMm3.isDisabled())
        {
            auto const & range = task.geometry.volumeMm3;
            bool const ok = range.contains(facts.volumeMm3);
            checks.push_back({"volume_mm3",
                              ok,
                              ok ? std::to_string(facts.volumeMm3)
                                 : rangeText(facts.volumeMm3, range)});
        }

        if (task.geometry.minTriangles > 0u)
        {
            bool const ok = facts.triangleCount >= task.geometry.minTriangles;
            checks.push_back({"triangle_count",
                              ok,
                              ok ? std::to_string(facts.triangleCount)
                                 : std::to_string(facts.triangleCount) + " < "
                                       + std::to_string(task.geometry.minTriangles)});
        }

        if (!task.geometry.fillFraction.isDisabled())
        {
            auto const & range = task.geometry.fillFraction;
            bool const ok = range.contains(facts.fillFraction);
            checks.push_back({"fill_fraction",
                              ok,
                              ok ? std::to_string(facts.fillFraction)
                                 : rangeText(facts.fillFraction, range)});
        }
        return checks;
    }

    bool geometryPassed(std::vector<GeometryCheck> const & checks)
    {
        return !checks.empty()
               && std::all_of(checks.begin(), checks.end(), [](GeometryCheck const & c) {
                      return c.passed;
                  });
    }

    nlohmann::json checksToJson(std::vector<GeometryCheck> const & checks)
    {
        nlohmann::json out = nlohmann::json::array();
        for (auto const & c : checks)
        {
            out.push_back({{"name", c.name}, {"passed", c.passed}, {"detail", c.detail}});
        }
        return out;
    }
} // namespace gladius::agent::bench
