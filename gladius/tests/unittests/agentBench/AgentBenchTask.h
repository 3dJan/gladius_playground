#pragma once

#include <array>
#include <cstddef>
#include <string>
#include <vector>

namespace gladius::agent::bench
{
    /// @brief Inclusive numeric range used by the deterministic expectations
    struct Range
    {
        double min = 0.0;
        double max = 0.0;

        [[nodiscard]] bool isDisabled() const noexcept
        {
            return max <= 0.0;
        }

        [[nodiscard]] bool contains(double value) const noexcept
        {
            return value >= min && value <= max;
        }
    };

    /// @brief Deterministic geometry expectation for one benchmark task (all values in mm)
    struct GeometryExpectation
    {
        /// @brief Accepted model spans, paired with the measured spans in ascending order so the
        ///        verdict does not depend on how the agent oriented the part. A disabled range
        ///        skips that span.
        std::array<Range, 3> axisSpanMm{};
        Range volumeMm3{};
        /// @brief Minimum triangle count of the exported mesh (0 disables the check)
        std::size_t minTriangles = 0;
        /// @brief Accepted mesh volume divided by bounding box volume. A solid block fills about
        ///        1.0, a solid sphere about 0.52, a hollow shell or open lattice clearly less.
        ///        A range with max 0 disables the check.
        Range fillFraction{};
        /// @brief Maximum allowed flattened graph node count (0 disables the check)
        std::size_t maxFlatNodes = 0;
    };

    /// @brief Declarative definition of one design-agent benchmark task
    struct AgentBenchTask
    {
        std::string id;
        std::string title;
        /// @brief Prompt handed to the design agent verbatim
        std::string prompt;
        /// @brief Rubric handed to the independent vision judge
        std::string judgeRubric;
        /// @brief Camera presets rendered for the judge after the run
        std::vector<std::string> judgeViews{"isometric", "front", "top"};
        GeometryExpectation geometry;
    };

    /// @brief The three complexity levels: primitive, boolean+lattice, dimensional parametric part
    [[nodiscard]] inline std::vector<AgentBenchTask> defaultBenchTasks()
    {
        return std::vector<AgentBenchTask>{
          AgentBenchTask{
            "sphere_r5",
            "Sphere with 5 mm radius",
            "Create a sphere with a radius of 5 mm.",
            "The model must be a single solid sphere with a radius of 5 mm (diameter 10 mm). "
            "It must not be a cube, not a lattice of cells, and not multiple separate objects.",
            {"isometric", "front", "top"},
            GeometryExpectation{{Range{8.0, 12.0}, Range{8.0, 12.0}, Range{8.0, 12.0}},
                                Range{300.0, 780.0},
                                200u,
                                // A sphere of the measured spans fills 0.52 of its box; a cube 1.0.
                                Range{0.45, 0.60}}},
          AgentBenchTask{
            "sphere_gyroid",
            "Sphere intersected with a gyroid",
            "Create a sphere with a 5 mm radius intersected with a gyroid triply periodic minimal "
            "surface structure, so the result is a spherical volume filled with an open gyroid "
            "lattice that stays inside the sphere.",
            "The model must show a round overall silhouette (about 10 mm across) whose interior is "
            "an open, bicontinuous gyroid-like lattice. A plain solid sphere, a solid cube, or a "
            "grid of straight rods does not satisfy the task.",
            {"isometric", "front", "top"},
            GeometryExpectation{{Range{8.0, 12.0}, Range{8.0, 12.0}, Range{8.0, 12.0}},
                                Range{10.0, 523.0},
                                500u,
                                // An open lattice fills far less of its box than the solid sphere
                                // it is cut from (0.52), so this separates lattice from solid. The
                                // lower bound only excludes a degenerate model, which the volume
                                // floor already does: how much material a gyroid wall carries is a
                                // free choice of wall thickness, and a thin one is still a lattice.
                                Range{0.01, 0.46}}},
          AgentBenchTask{
            "lego_brick_3x4",
            "3x4 LEGO compatible brick",
            "Create a 3x4 LEGO compatible brick: a rectangular brick whose footprint matches the "
            "LEGO grid for 3 studs by 4 studs, with the standard stud layout on top and the "
            "tubular underside.",
            "The model must be a rectangular building brick sized for a 3x4 stud grid, with 12 "
            "round studs in three rows of four on the top face. A plain solid box without studs, or "
            "a brick with a wrong stud count, does not satisfy the task.",
            // The hollow underside cannot be lit by the renderer (downward facing faces shade to
            // black), so it is verified by the deterministic fill-fraction gate instead.
            {"isometric", "front", "top"},
            GeometryExpectation{{Range{20.0, 28.0}, Range{28.0, 36.0}, Range{7.0, 14.0}},
                                Range{1000.0, 9000.0},
                                500u,
                                // A brick is a shell around a cavity, so it fills well under three
                                // quarters of its box. A solid block (no hollow underside) does not.
                                Range{0.20, 0.75}}},
        };
    }

    /// @brief Multi-component implicit-CAD benchmark tasks for comparing single vs hierarchical runs
    [[nodiscard]] inline std::vector<AgentBenchTask> multiComponentBenchTasks()
    {
        return std::vector<AgentBenchTask>{
          AgentBenchTask{
            "wheel_assembly",
            "Wheel assembly (tyre + rim)",
            "Design a two-part wheel assembly (~40 mm outer diameter, ~14 mm width): an outer tyre "
            "with periodic radial tread grooves around its circumference and an inner rim with a "
            "central axle bore (6 mm diameter) that mates cleanly to the tyre inner bore.",
            "The model must look like a wheel with a distinct outer tyre (showing periodic tread "
            "pattern around the circumference) and an inner rim/hub with a central axle hole. "
            "A plain solid cylinder or a featureless torus without tread and hub bore does not "
            "satisfy the task.",
            {"isometric", "front", "top"},
            GeometryExpectation{{Range{34.0, 46.0}, Range{34.0, 46.0}, Range{10.0, 18.0}},
                                Range{3000.0, 18000.0},
                                1000u,
                                Range{0.25, 0.75},
                                600u}},
        };
    }
} // namespace gladius::agent::bench
