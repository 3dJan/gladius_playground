#include "ManufacturingValidator.h"

#include "../Document.h"
#include "../nodes/Validator.h"
#include "DesignCatalogue.h"
#include "Renderability.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <set>

namespace gladius::design
{
    namespace
    {
        constexpr double kPi = 3.14159265358979323846;

        [[nodiscard]] std::array<double, 3> sub(std::array<double, 3> const & a,
                                                std::array<double, 3> const & b) noexcept
        {
            return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        }

        [[nodiscard]] std::array<double, 3> cross(std::array<double, 3> const & a,
                                                  std::array<double, 3> const & b) noexcept
        {
            return {a[1] * b[2] - a[2] * b[1],
                    a[2] * b[0] - a[0] * b[2],
                    a[0] * b[1] - a[1] * b[0]};
        }

        [[nodiscard]] double dot(std::array<double, 3> const & a,
                                 std::array<double, 3> const & b) noexcept
        {
            return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
        }

        [[nodiscard]] double length(std::array<double, 3> const & v) noexcept
        {
            return std::sqrt(dot(v, v));
        }

        [[nodiscard]] std::array<double, 3> normalize(std::array<double, 3> const & v) noexcept
        {
            double const len = length(v);
            if (len <= 1e-12)
            {
                return {0.0, 0.0, 0.0};
            }
            return {v[0] / len, v[1] / len, v[2] / len};
        }

        /// @brief Möller–Trumbore ray-triangle intersection returning positive hit distance if hit
        [[nodiscard]] bool intersectRayTriangle(std::array<double, 3> const & origin,
                                                std::array<double, 3> const & dir,
                                                MeshTriangle const & tri,
                                                double & tOut) noexcept
        {
            constexpr double kEps = 1e-7;
            auto const edge1 = sub(tri.vertices[1], tri.vertices[0]);
            auto const edge2 = sub(tri.vertices[2], tri.vertices[0]);
            auto const h = cross(dir, edge2);
            double const a = dot(edge1, h);
            if (a > -kEps && a < kEps)
            {
                return false;
            }
            double const f = 1.0 / a;
            auto const s = sub(origin, tri.vertices[0]);
            double const u = f * dot(s, h);
            if (u < 0.0 || u > 1.0)
            {
                return false;
            }
            auto const q = cross(s, edge1);
            double const v = f * dot(dir, q);
            if (v < 0.0 || u + v > 1.0)
            {
                return false;
            }
            double const t = f * dot(edge2, q);
            if (t > 1e-4)
            {
                tOut = t;
                return true;
            }
            return false;
        }

        void evaluateMeshMetrics(std::vector<MeshTriangle> const & triangles,
                                 PrintabilityLimits const & limits,
                                 ManufacturingReport & report)
        {
            report.triangleCount = triangles.size();
            if (triangles.empty())
            {
                return;
            }

            report.hasMeshMetrics = true;
            std::array<double, 3> bMin{
              std::numeric_limits<double>::max(),
              std::numeric_limits<double>::max(),
              std::numeric_limits<double>::max()};
            std::array<double, 3> bMax{
              std::numeric_limits<double>::lowest(),
              std::numeric_limits<double>::lowest(),
              std::numeric_limits<double>::lowest()};

            double signedVolume6 = 0.0;
            double totalArea = 0.0;

            for (auto const & tri : triangles)
            {
                for (auto const & v : tri.vertices)
                {
                    for (int axis = 0; axis < 3; ++axis)
                    {
                        bMin[axis] = std::min(bMin[axis], v[axis]);
                        bMax[axis] = std::max(bMax[axis], v[axis]);
                    }
                }
                auto const e1 = sub(tri.vertices[1], tri.vertices[0]);
                auto const e2 = sub(tri.vertices[2], tri.vertices[0]);
                totalArea += 0.5 * length(cross(e1, e2));
                signedVolume6 += dot(tri.vertices[0], cross(tri.vertices[1], tri.vertices[2]));
            }

            if (!report.hasBounds)
            {
                report.hasBounds = true;
                report.boundingBoxMin = bMin;
                report.boundingBoxMax = bMax;
                report.dimensionsMm = {bMax[0] - bMin[0], bMax[1] - bMin[1], bMax[2] - bMin[2]};
                report.withinBuildVolume = report.dimensionsMm[0] <= limits.maxBuildVolumeMm[0]
                                           && report.dimensionsMm[1] <= limits.maxBuildVolumeMm[1]
                                           && report.dimensionsMm[2] <= limits.maxBuildVolumeMm[2];
            }

            report.surfaceAreaMm2 = totalArea;
            report.volumeMm3 = std::abs(signedVolume6) / 6.0;

            double const overhangCosThreshold =
              -std::cos((90.0 - limits.maxOverhangAngleDeg) * kPi / 180.0);
            double const bedZ = report.boundingBoxMin[2] + limits.bedZ_ToleranceMm;

            double overhangArea = 0.0;
            std::array<double, 3> bridgeMin{
              std::numeric_limits<double>::max(),
              std::numeric_limits<double>::max(),
              0.0};
            std::array<double, 3> bridgeMax{
              std::numeric_limits<double>::lowest(),
              std::numeric_limits<double>::lowest(),
              0.0};
            bool hasBridgeFaces = false;

            for (auto const & tri : triangles)
            {
                auto const e1 = sub(tri.vertices[1], tri.vertices[0]);
                auto const e2 = sub(tri.vertices[2], tri.vertices[0]);
                auto const rawNormal = cross(e1, e2);
                double const area = 0.5 * length(rawNormal);
                if (area <= 1e-12)
                {
                    continue;
                }
                auto const n = normalize(length(tri.normal) > 1e-9 ? tri.normal : rawNormal);
                double const centroidZ =
                  (tri.vertices[0][2] + tri.vertices[1][2] + tri.vertices[2][2]) / 3.0;

                if (centroidZ > bedZ && n[2] < overhangCosThreshold)
                {
                    overhangArea += area;
                }
                if (centroidZ > bedZ && n[2] < -0.85)
                {
                    hasBridgeFaces = true;
                    for (auto const & v : tri.vertices)
                    {
                        bridgeMin[0] = std::min(bridgeMin[0], v[0]);
                        bridgeMin[1] = std::min(bridgeMin[1], v[1]);
                        bridgeMax[0] = std::max(bridgeMax[0], v[0]);
                        bridgeMax[1] = std::max(bridgeMax[1], v[1]);
                    }
                }
            }

            report.unsupportedOverhangAreaMm2 = overhangArea;
            report.overhangRatio = totalArea > 1e-9 ? (overhangArea / totalArea) : 0.0;
            if (hasBridgeFaces)
            {
                double const dx = bridgeMax[0] - bridgeMin[0];
                double const dy = bridgeMax[1] - bridgeMin[1];
                report.maxBridgeLengthMm = std::min(dx, dy);
            }

            // Sample up to 512 evenly-spaced triangles to compute deterministic minimum wall thickness.
            std::size_t const step = std::max<std::size_t>(1, triangles.size() / 512);
            double minThickness = std::numeric_limits<double>::max();
            for (std::size_t i = 0; i < triangles.size(); i += step)
            {
                auto const & src = triangles[i];
                auto const e1 = sub(src.vertices[1], src.vertices[0]);
                auto const e2 = sub(src.vertices[2], src.vertices[0]);
                auto const rawNormal = cross(e1, e2);
                auto const n = normalize(length(src.normal) > 1e-9 ? src.normal : rawNormal);
                if (length(n) <= 1e-9)
                {
                    continue;
                }
                std::array<double, 3> const centroid{
                  (src.vertices[0][0] + src.vertices[1][0] + src.vertices[2][0]) / 3.0,
                  (src.vertices[0][1] + src.vertices[1][1] + src.vertices[2][1]) / 3.0,
                  (src.vertices[0][2] + src.vertices[1][2] + src.vertices[2][2]) / 3.0};
                std::array<double, 3> const inwardDir{-n[0], -n[1], -n[2]};

                double closestHit = std::numeric_limits<double>::max();
                for (std::size_t j = 0; j < triangles.size(); ++j)
                {
                    if (i == j)
                    {
                        continue;
                    }
                    double t = 0.0;
                    if (intersectRayTriangle(centroid, inwardDir, triangles[j], t))
                    {
                        closestHit = std::min(closestHit, t);
                    }
                }
                if (closestHit < std::numeric_limits<double>::max())
                {
                    minThickness = std::min(minThickness, closestHit);
                }
            }
            if (minThickness < std::numeric_limits<double>::max())
            {
                report.minWallThicknessMm = minThickness;
            }

            if (report.withinBuildVolume)
            {
                report.passedChecks.push_back("Within build volume");
            }
            else
            {
                report.warnings.push_back("Model exceeds build volume limits");
            }

            if (report.minWallThicknessMm > 0.0)
            {
                if (report.minWallThicknessMm >= limits.minWallThicknessMm)
                {
                    report.passedChecks.push_back("Minimum wall thickness >= " +
                                                  std::to_string(limits.minWallThicknessMm) + " mm");
                }
                else
                {
                    report.warnings.push_back("Thin wall detected: " +
                                              std::to_string(report.minWallThicknessMm) + " mm < " +
                                              std::to_string(limits.minWallThicknessMm) + " mm");
                }
            }

            if (report.overhangRatio <= limits.maxUnsupportedOverhangRatio)
            {
                report.passedChecks.push_back("Unsupported overhang ratio within limit");
            }
            else
            {
                report.warnings.push_back("Excessive unsupported overhang ratio: " +
                                          std::to_string(report.overhangRatio * 100.0) + "%");
            }

            if (report.maxBridgeLengthMm <= limits.maxBridgeLengthMm)
            {
                report.passedChecks.push_back("Bridge spans within printable limit");
            }
            else
            {
                report.warnings.push_back("Long unsupported bridge span: " +
                                          std::to_string(report.maxBridgeLengthMm) + " mm");
            }
        }
    } // namespace

    nlohmann::json ManufacturingReport::toJson() const
    {
        return {{"valid3mf", valid3mf},
                {"renderable", renderable},
                {"hasBounds", hasBounds},
                {"withinBuildVolume", withinBuildVolume},
                {"hasMeshMetrics", hasMeshMetrics},
                {"boundingBoxMin", boundingBoxMin},
                {"boundingBoxMax", boundingBoxMax},
                {"dimensionsMm", dimensionsMm},
                {"triangleCount", triangleCount},
                {"volumeMm3", volumeMm3},
                {"surfaceAreaMm2", surfaceAreaMm2},
                {"minWallThicknessMm", minWallThicknessMm},
                {"unsupportedOverhangAreaMm2", unsupportedOverhangAreaMm2},
                {"overhangRatio", overhangRatio},
                {"maxBridgeLengthMm", maxBridgeLengthMm},
                {"minClearanceMm", minClearanceMm},
                {"materialCount", materialCount},
                {"passedChecks", passedChecks},
                {"warnings", warnings}};
    }

    Evaluation ManufacturingReport::toEvaluation(DesignGraph const & graph,
                                                 Configuration const & cfg,
                                                 DesignCatalogue const * catalogue) const
    {
        Evaluation eval;
        eval.deterministicPassed = passedChecks;
        eval.deterministicWarnings = warnings;

        if (catalogue != nullptr)
        {
            std::vector<std::string> violations;
            if (catalogue->checkCompatibility(cfg.selectedVariants, violations))
            {
                eval.deterministicPassed.push_back("All selected variants satisfy compatibility rules");
            }
            else
            {
                eval.deterministicWarnings.insert(
                  eval.deterministicWarnings.end(), violations.begin(), violations.end());
            }
        }

        std::size_t coveredNodes = 0;
        for (auto const & [nodeId, _node] : graph.nodes())
        {
            if (cfg.selectedVariants.contains(nodeId))
            {
                ++coveredNodes;
            }
        }
        if (coveredNodes == graph.nodes().size() && !graph.nodes().empty())
        {
            eval.deterministicPassed.push_back("All design nodes covered by configuration");
        }
        else if (!graph.nodes().empty())
        {
            eval.deterministicWarnings.push_back("Some design nodes lack an assigned variant");
        }

        eval.pruningScore = static_cast<double>(eval.deterministicPassed.size()) * 10.0
                            - static_cast<double>(eval.deterministicWarnings.size()) * 15.0;
        return eval;
    }

    ManufacturingReport ManufacturingValidator::analyzeTriangles(std::vector<MeshTriangle> const & triangles,
                                                                 PrintabilityLimits const & limits)
    {
        ManufacturingReport report;
        report.valid3mf = !triangles.empty();
        report.renderable = !triangles.empty();
        evaluateMeshMetrics(triangles, limits, report);
        return report;
    }

    ManufacturingReport ManufacturingValidator::analyzeDocument(Document & document,
                                                                DesignGraph const & graph,
                                                                PrintabilityLimits const & limits)
    {
        ManufacturingReport report;

        auto const renderFacts = collectRenderabilityFacts(&document);
        report.renderable = renderFacts.isRenderable();
        if (report.renderable)
        {
            report.passedChecks.push_back("Model has renderable root output chain");
        }
        else
        {
            report.warnings.push_back("Model is not renderable: " + renderFacts.blockersAsText());
        }

        if (auto assembly = document.getAssembly())
        {
            nodes::Validator validator;
            nodes::IssueList issues;
            report.valid3mf = validator.validate(*assembly, issues);
            if (report.valid3mf)
            {
                report.passedChecks.push_back("3MF node graph is valid");
            }
            else
            {
                report.warnings.push_back("3MF node graph has validation issues");
            }

            std::set<uint32_t> uniqueMaterials;
            for (auto const & [resId, model] : assembly->getFunctions())
            {
                if (!model)
                {
                    continue;
                }
                if (auto const displayName = model->getDisplayName())
                {
                    if (displayName->find("color") != std::string::npos
                        || displayName->find("material") != std::string::npos)
                    {
                        uniqueMaterials.insert(resId);
                    }
                }
            }
            report.materialCount = std::max<std::size_t>(1, uniqueMaterials.size());
        }

        auto const bbox = document.computeBoundingBox();
        if (bbox.min.x < bbox.max.x && bbox.min.y < bbox.max.y && bbox.min.z < bbox.max.z)
        {
            report.hasBounds = true;
            report.boundingBoxMin = {bbox.min.x, bbox.min.y, bbox.min.z};
            report.boundingBoxMax = {bbox.max.x, bbox.max.y, bbox.max.z};
            report.dimensionsMm = {
              bbox.max.x - bbox.min.x, bbox.max.y - bbox.min.y, bbox.max.z - bbox.min.z};
            report.withinBuildVolume = report.dimensionsMm[0] <= limits.maxBuildVolumeMm[0]
                                       && report.dimensionsMm[1] <= limits.maxBuildVolumeMm[1]
                                       && report.dimensionsMm[2] <= limits.maxBuildVolumeMm[2];
            if (report.withinBuildVolume)
            {
                report.passedChecks.push_back("Within build volume");
            }
            else
            {
                report.warnings.push_back("Model exceeds build volume limits");
            }
        }

        try
        {
            auto mesh = document.generateMesh();
            std::size_t const faceCount = mesh.getNumberOfFaces();
            std::vector<MeshTriangle> triangles;
            triangles.reserve(faceCount);
            for (std::size_t i = 0; i < faceCount; ++i)
            {
                auto const face = mesh.getFace(i);
                MeshTriangle tri;
                tri.normal = {face.normal.x(), face.normal.y(), face.normal.z()};
                for (int v = 0; v < 3; ++v)
                {
                    tri.vertices[v] = {face.vertices[v].x(), face.vertices[v].y(), face.vertices[v].z()};
                }
                triangles.push_back(tri);
            }
            evaluateMeshMetrics(triangles, limits, report);
        }
        catch (...)
        {
            // Mesh generation may be unavailable on non-OpenCL/non-OpenVDB backends; bounds and graph checks still apply.
        }

        // Check parameter-level wall thickness on decomposed variants if mesh thickness wasn't measured.
        if (!report.hasMeshMetrics)
        {
            for (auto const & [_vid, var] : graph.variants())
            {
                if (var.status == DesignStatus::Rejected)
                {
                    continue;
                }
                if (var.parameters.contains("wallThickness") && var.parameters["wallThickness"].is_number())
                {
                    double const wt = var.parameters["wallThickness"].get<double>();
                    if (report.minWallThicknessMm <= 0.0 || wt < report.minWallThicknessMm)
                    {
                        report.minWallThicknessMm = wt;
                    }
                }
            }
            if (report.minWallThicknessMm > 0.0)
            {
                if (report.minWallThicknessMm >= limits.minWallThicknessMm)
                {
                    report.passedChecks.push_back("Variant wallThickness parameter >= " +
                                                  std::to_string(limits.minWallThicknessMm) + " mm");
                }
                else
                {
                    report.warnings.push_back("Variant wallThickness parameter < " +
                                              std::to_string(limits.minWallThicknessMm) + " mm");
                }
            }
        }

        return report;
    }

    std::function<Evaluation(DesignGraph const &, Configuration const &)>
    ManufacturingValidator::makeValidationCallback(Document * & documentRef,
                                                   DesignCatalogue const & catalogue,
                                                   PrintabilityLimits const & limits)
    {
        return [&documentRef, &catalogue, limits](DesignGraph const & graph,
                                                  Configuration const & cfg) -> Evaluation {
            if (documentRef == nullptr)
            {
                ManufacturingReport fallback;
                return fallback.toEvaluation(graph, cfg, &catalogue);
            }
            auto const report = analyzeDocument(*documentRef, graph, limits);
            return report.toEvaluation(graph, cfg, &catalogue);
        };
    }
} // namespace gladius::design
