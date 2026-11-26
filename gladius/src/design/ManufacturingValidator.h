#pragma once

#include "DesignCatalogue.h"
#include "DesignGraph.h"
#include "DesignTypes.h"

#include <array>
#include <cstddef>
#include <functional>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace gladius
{
    class Document;
}

namespace gladius::design
{
    /// @brief Configurable manufacturing constraints for deterministic printability evaluation
    struct PrintabilityLimits
    {
        std::array<double, 3> maxBuildVolumeMm{250.0, 250.0, 250.0};
        double minWallThicknessMm = 1.2;
        double maxOverhangAngleDeg = 45.0;
        double maxUnsupportedOverhangRatio = 0.25;
        double maxBridgeLengthMm = 15.0;
        double minClearanceMm = 0.2;
        double bedZ_ToleranceMm = 0.5;
    };

    /// @brief Lightweight triangle representation decoupled from GPU buffers for deterministic analysis
    struct MeshTriangle
    {
        std::array<std::array<double, 3>, 3> vertices{};
        std::array<double, 3> normal{0.0, 0.0, 1.0};
    };

    /// @brief Deterministic engineering and printability metrics for a 3D model
    struct ManufacturingReport
    {
        bool valid3mf = false;
        bool renderable = false;
        bool hasBounds = false;
        bool withinBuildVolume = false;
        bool hasMeshMetrics = false;

        std::array<double, 3> boundingBoxMin{0.0, 0.0, 0.0};
        std::array<double, 3> boundingBoxMax{0.0, 0.0, 0.0};
        std::array<double, 3> dimensionsMm{0.0, 0.0, 0.0};

        std::size_t triangleCount = 0;
        double volumeMm3 = 0.0;
        double surfaceAreaMm2 = 0.0;
        double minWallThicknessMm = 0.0;
        double unsupportedOverhangAreaMm2 = 0.0;
        double overhangRatio = 0.0;
        double maxBridgeLengthMm = 0.0;
        double minClearanceMm = 0.0;
        std::size_t materialCount = 1;

        std::vector<std::string> passedChecks;
        std::vector<std::string> warnings;

        [[nodiscard]] nlohmann::json toJson() const;

        /// @brief Convert deterministic facts into a DesignGraph Evaluation record
        [[nodiscard]] Evaluation toEvaluation(DesignGraph const & graph,
                                              Configuration const & cfg,
                                              DesignCatalogue const * catalogue = nullptr) const;
    };

    /// @brief Deterministic analyzer computing printability, thickness, overhang, bridge, and 3MF metrics
    class ManufacturingValidator
    {
      public:
        /// @brief Analyze a triangle mesh against printability limits (pure CPU, deterministic)
        [[nodiscard]] static ManufacturingReport analyzeTriangles(std::vector<MeshTriangle> const & triangles,
                                                                  PrintabilityLimits const & limits = {});

        /// @brief Analyze a live Gladius Document (bounds, 3MF graph validity, renderability, and mesh metrics)
        [[nodiscard]] static ManufacturingReport analyzeDocument(Document & document,
                                                                 DesignGraph const & graph,
                                                                 PrintabilityLimits const & limits = {});

        /// @brief Build a ModelValidationCallback suitable for DesignToolContext
        [[nodiscard]] static std::function<Evaluation(DesignGraph const &, Configuration const &)>
        makeValidationCallback(Document * & documentRef,
                               DesignCatalogue const & catalogue,
                               PrintabilityLimits const & limits = {});
    };
} // namespace gladius::design
