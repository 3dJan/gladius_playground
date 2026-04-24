/// @file MeshSdfReference.cpp
/// @brief Implementation of the brute-force reference SDF.

#include "MeshSdfReference.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>

namespace gladius::mesh_sdf_reference
{
    namespace
    {
        struct Vec3
        {
            float x;
            float y;
            float z;
        };

        inline Vec3 toVec3(float4 const & v) noexcept
        {
            return Vec3{v.x, v.y, v.z};
        }

        inline Vec3 sub(Vec3 const & a, Vec3 const & b) noexcept
        {
            return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
        }

        inline float dot(Vec3 const & a, Vec3 const & b) noexcept
        {
            return a.x * b.x + a.y * b.y + a.z * b.z;
        }

        inline Vec3 cross(Vec3 const & a, Vec3 const & b) noexcept
        {
            return Vec3{a.y * b.z - a.z * b.y,
                        a.z * b.x - a.x * b.z,
                        a.x * b.y - a.y * b.x};
        }

        inline float length(Vec3 const & v) noexcept
        {
            return std::sqrt(dot(v, v));
        }

        /// Squared distance from @p p to the triangle (a, b, c). Adapted
        /// from "Real-Time Collision Detection" (Christer Ericson, §5.1.5).
        float pointTriangleDistanceSq(Vec3 const & p,
                                      Vec3 const & a,
                                      Vec3 const & b,
                                      Vec3 const & c) noexcept
        {
            Vec3 const ab = sub(b, a);
            Vec3 const ac = sub(c, a);
            Vec3 const ap = sub(p, a);
            float const d1 = dot(ab, ap);
            float const d2 = dot(ac, ap);
            if (d1 <= 0.f && d2 <= 0.f)
            {
                return dot(ap, ap); // vertex region a
            }

            Vec3 const bp = sub(p, b);
            float const d3 = dot(ab, bp);
            float const d4 = dot(ac, bp);
            if (d3 >= 0.f && d4 <= d3)
            {
                return dot(bp, bp); // vertex region b
            }

            float const vc = d1 * d4 - d3 * d2;
            if (vc <= 0.f && d1 >= 0.f && d3 <= 0.f)
            {
                float const v = d1 / (d1 - d3);
                Vec3 const closest{a.x + v * ab.x, a.y + v * ab.y, a.z + v * ab.z};
                Vec3 const diff = sub(p, closest);
                return dot(diff, diff); // edge region ab
            }

            Vec3 const cp = sub(p, c);
            float const d5 = dot(ab, cp);
            float const d6 = dot(ac, cp);
            if (d6 >= 0.f && d5 <= d6)
            {
                return dot(cp, cp); // vertex region c
            }

            float const vb = d5 * d2 - d1 * d6;
            if (vb <= 0.f && d2 >= 0.f && d6 <= 0.f)
            {
                float const w = d2 / (d2 - d6);
                Vec3 const closest{a.x + w * ac.x, a.y + w * ac.y, a.z + w * ac.z};
                Vec3 const diff = sub(p, closest);
                return dot(diff, diff); // edge region ac
            }

            float const va = d3 * d6 - d5 * d4;
            if (va <= 0.f && (d4 - d3) >= 0.f && (d5 - d6) >= 0.f)
            {
                float const w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
                Vec3 const closest{b.x + w * (c.x - b.x),
                                   b.y + w * (c.y - b.y),
                                   b.z + w * (c.z - b.z)};
                Vec3 const diff = sub(p, closest);
                return dot(diff, diff); // edge region bc
            }

            float const denom = 1.f / (va + vb + vc);
            float const v = vb * denom;
            float const w = vc * denom;
            Vec3 const closest{a.x + ab.x * v + ac.x * w,
                               a.y + ab.y * v + ac.y * w,
                               a.z + ab.z * v + ac.z * w};
            Vec3 const diff = sub(p, closest);
            return dot(diff, diff); // face region
        }

        /// Solid angle subtended at the origin by the triangle (a, b, c)
        /// using the Van Oosterom-Strang formula. Returns a signed value:
        /// positive if (a, b, c) wind counter-clockwise as seen from the
        /// origin, negative otherwise. The generalised winding number is
        /// the sum of these contributions divided by 4π.
        float solidAngleOriginCentred(Vec3 const & a,
                                      Vec3 const & b,
                                      Vec3 const & c) noexcept
        {
            float const la = length(a);
            float const lb = length(b);
            float const lc = length(c);
            // Numerator: triple product a · (b × c)
            float const numerator = dot(a, cross(b, c));
            // Denominator from Van Oosterom & Strang (1983), eq. (7).
            float const denominator =
                la * lb * lc + dot(a, b) * lc + dot(b, c) * la + dot(c, a) * lb;
            return 2.f * std::atan2(numerator, denominator);
        }
    } // namespace

    float referenceUnsignedDistance(std::span<float4 const> vertices,
                                    std::span<TriangleIndices const> indices,
                                    float4 const & query)
    {
        if (indices.empty())
        {
            return 0.f;
        }
        Vec3 const p = toVec3(query);
        float bestSq = std::numeric_limits<float>::infinity();
        for (auto const & tri : indices)
        {
            std::size_t const i0 = static_cast<std::size_t>(tri.i0);
            std::size_t const i1 = static_cast<std::size_t>(tri.i1);
            std::size_t const i2 = static_cast<std::size_t>(tri.i2);
            if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
            {
                continue;
            }
            float const dSq = pointTriangleDistanceSq(p,
                                                      toVec3(vertices[i0]),
                                                      toVec3(vertices[i1]),
                                                      toVec3(vertices[i2]));
            if (dSq < bestSq)
            {
                bestSq = dSq;
            }
        }
        return std::sqrt(bestSq);
    }

    float referenceWindingNumber(std::span<float4 const> vertices,
                                 std::span<TriangleIndices const> indices,
                                 float4 const & query)
    {
        if (indices.empty())
        {
            return 0.f;
        }
        Vec3 const p = toVec3(query);
        double accumulated = 0.0; // double for stable summation
        for (auto const & tri : indices)
        {
            std::size_t const i0 = static_cast<std::size_t>(tri.i0);
            std::size_t const i1 = static_cast<std::size_t>(tri.i1);
            std::size_t const i2 = static_cast<std::size_t>(tri.i2);
            if (i0 >= vertices.size() || i1 >= vertices.size() || i2 >= vertices.size())
            {
                continue;
            }
            Vec3 const a = sub(toVec3(vertices[i0]), p);
            Vec3 const b = sub(toVec3(vertices[i1]), p);
            Vec3 const c = sub(toVec3(vertices[i2]), p);
            accumulated += static_cast<double>(solidAngleOriginCentred(a, b, c));
        }
        constexpr double kInvFourPi = 1.0 / (4.0 * 3.14159265358979323846);
        return static_cast<float>(accumulated * kInvFourPi);
    }

    float referenceSignedDistance(std::span<float4 const> vertices,
                                  std::span<TriangleIndices const> indices,
                                  float4 const & query)
    {
        float const unsignedDist = referenceUnsignedDistance(vertices, indices, query);
        float const winding = referenceWindingNumber(vertices, indices, query);
        return (winding > 0.5f) ? -unsignedDist : unsignedDist;
    }

} // namespace gladius::mesh_sdf_reference
