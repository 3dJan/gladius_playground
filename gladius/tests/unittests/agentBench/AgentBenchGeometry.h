#pragma once

#include "AgentBenchTask.h"

#include <array>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>

namespace gladius::agent::bench
{
    /// @brief Measurements taken from the produced model, independent of the vision judge
    struct GeometryFacts
    {
        bool boundsUsable = false;
        std::array<double, 3> boundsMin{};
        std::array<double, 3> boundsMax{};
        bool meshValid = false;
        std::size_t triangleCount = 0;
        double volumeMm3 = 0.0;
        std::array<double, 3> meshMin{};
        std::array<double, 3> meshMax{};
        /// @brief Mesh volume divided by the mesh bounding box volume; below 1 for a hollow part
        double fillFraction = 0.0;
        /// @brief Bounding box volume of the mesh in mm^3
        double boundingBoxVolumeMm3 = 0.0;
        /// @brief Empty when the mesh was measured successfully
        std::string error;

        [[nodiscard]] std::array<double, 3> axisSpans() const;
        /// @brief Recomputes fillFraction and boundingBoxVolumeMm3 from volumeMm3 and the mesh bbox
        void updateDerivedFacts();
        [[nodiscard]] nlohmann::json toJson() const;
    };

    /// @brief One violated or satisfied deterministic expectation
    struct GeometryCheck
    {
        std::string name;
        bool passed = false;
        std::string detail;
    };

    /// @brief Reads a binary STL file and computes signed volume, bbox and triangle count
    [[nodiscard]] GeometryFacts measureStl(std::string const & filePath);

    /// @brief Runs the deterministic gates of a task against measured facts
    [[nodiscard]] std::vector<GeometryCheck> evaluateGeometry(AgentBenchTask const & task,
                                                              GeometryFacts const & facts);

    /// @brief True only when every gate passed
    [[nodiscard]] bool geometryPassed(std::vector<GeometryCheck> const & checks);

    [[nodiscard]] nlohmann::json checksToJson(std::vector<GeometryCheck> const & checks);
} // namespace gladius::agent::bench
