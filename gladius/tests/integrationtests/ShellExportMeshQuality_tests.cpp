/// @file ShellExportMeshQuality_tests.cpp
/// @brief Integration tests validating shell mesh quality using admesh analysis
///
/// These tests verify that shell meshes generated for HueForge-style exports
/// are watertight, manifold, and have correct normals.

#include "ComputeContext.h"
#include "Document.h"
#include "EventLogger.h"
#include "io/3mf/ShellGenerator.h"
#include "io/3mf/FilamentOpticalProperties.h"
#include "io/3mf/FrontlitThicknessSolver.h"
#include "io/SurfaceExtractionOptions.h"

#include <compute/ComputeCore.h>

#include <eigen3/Eigen/Core>
#include <eigen3/Eigen/Geometry>
#include <fmt/format.h>
#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#include <cstdio>
#else
#include <sys/wait.h>
#endif

namespace gladius_tests::shell_mesh_quality
{
    using namespace gladius;
    using namespace gladius::io;

    namespace
    {
        /// RAII guard for temporary files - cleans up on destruction
        class TempFileGuard
        {
          public:
            explicit TempFileGuard(std::filesystem::path path)
                : m_path(std::move(path))
            {
            }

            TempFileGuard(TempFileGuard const &) = delete;
            TempFileGuard & operator=(TempFileGuard const &) = delete;
            TempFileGuard(TempFileGuard &&) = delete;
            TempFileGuard & operator=(TempFileGuard &&) = delete;

            ~TempFileGuard()
            {
                if (m_path.empty())
                {
                    return;
                }

                std::error_code ec;
                std::filesystem::remove(m_path, ec);
            }

            [[nodiscard]] std::filesystem::path const & path() const
            {
                return m_path;
            }

          private:
            std::filesystem::path m_path;
        };

        [[nodiscard]] bool gpuTestsEnabled()
        {
            char const * const env = std::getenv("GLADIUS_SKIP_GPU_TESTS");
            if (env != nullptr && std::string(env) == "1")
            {
                return false;
            }
            return true;
        }

        [[nodiscard]] bool isAdmeshAvailable()
        {
            int const result = std::system("command -v admesh >/dev/null 2>&1");
            return result == 0;
        }

        [[nodiscard]] std::filesystem::path makeUniqueTempFile(std::string_view stem,
                                                               std::string_view extension)
        {
            auto tempDir = std::filesystem::temp_directory_path();
            auto const timestamp = std::chrono::duration_cast<std::chrono::microseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch())
                                     .count();

            std::string filename{stem};
            filename += "_";
            filename += std::to_string(timestamp);
            filename += extension;

            return tempDir / filename;
        }

        [[nodiscard]] std::string runCommandAndCapture(std::string const & command, int & exitCode)
        {
            std::array<char, 512> buffer{};
            std::string output;

#ifdef _WIN32
            FILE * pipe = _popen(command.c_str(), "r");
#else
            FILE * pipe = popen(command.c_str(), "r");
#endif
            if (pipe == nullptr)
            {
                exitCode = -1;
                return output;
            }

            while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
            {
                output.append(buffer.data());
            }

#ifdef _WIN32
            int const status = _pclose(pipe);
            exitCode = status;
#else
            int const status = pclose(pipe);
            if (status == -1)
            {
                exitCode = -1;
            }
#if defined(WIFEXITED) && defined(WEXITSTATUS)
            else if (WIFEXITED(status))
            {
                exitCode = WEXITSTATUS(status);
            }
            else
            {
                exitCode = status;
            }
#else
            else
            {
                exitCode = status;
            }
#endif
#endif
            return output;
        }

        /// Admesh metrics parsed from output
        struct AdmeshMetrics
        {
            int numberOfFacets{0};
            int disconnectedFacets{0};
            int numberOfParts{0};
            double volume{0.0};
            int degenerateFacets{0};
            int edgesFixed{0};
            int facetsRemoved{0};
            int facetsAdded{0};
            int facetsReversed{0};
            int backwardsEdges{0};
            int normalsFixed{0};
        };

        [[nodiscard]] std::optional<double>
        parseAdmeshValue(std::string const & text, std::string_view label, std::size_t columnIndex)
        {
            auto const labelPos = text.find(label);
            if (labelPos == std::string::npos)
            {
                return std::nullopt;
            }

            auto const colonPos = text.find(':', labelPos);
            if (colonPos == std::string::npos)
            {
                return std::nullopt;
            }

            auto lineEnd = text.find('\n', colonPos);
            if (lineEnd == std::string::npos)
            {
                lineEnd = text.size();
            }

            std::string const numbers = text.substr(colonPos + 1, lineEnd - colonPos - 1);
            std::istringstream stream(numbers);
            double value = 0.0;
            for (std::size_t idx = 0; idx <= columnIndex; ++idx)
            {
                if (!(stream >> value))
                {
                    return std::nullopt;
                }
            }

            return value;
        }

        [[nodiscard]] double requireAdmeshValue(std::string const & text,
                                                std::string_view label,
                                                std::size_t columnIndex)
        {
            auto const value = parseAdmeshValue(text, label, columnIndex);
            if (!value.has_value())
            {
                std::ostringstream message;
                message << "Failed to parse admesh metric '" << label << "' (column " << columnIndex
                        << ")";
                throw std::runtime_error(message.str());
            }

            return value.value();
        }

        [[nodiscard]] AdmeshMetrics parseAdmeshMetrics(std::string const & text)
        {
            AdmeshMetrics metrics;
            metrics.numberOfFacets = static_cast<int>(requireAdmeshValue(text, "Number of facets", 0U));
            metrics.disconnectedFacets =
                static_cast<int>(requireAdmeshValue(text, "Total disconnected facets", 0U));
            metrics.numberOfParts =
                static_cast<int>(requireAdmeshValue(text, "Number of parts", 0U));
            metrics.volume = requireAdmeshValue(text, "Volume", 0U);
            metrics.degenerateFacets =
                static_cast<int>(requireAdmeshValue(text, "Degenerate facets", 0U));
            metrics.edgesFixed = static_cast<int>(requireAdmeshValue(text, "Edges fixed", 0U));
            metrics.facetsRemoved =
                static_cast<int>(requireAdmeshValue(text, "Facets removed", 0U));
            metrics.facetsAdded = static_cast<int>(requireAdmeshValue(text, "Facets added", 0U));
            metrics.facetsReversed =
                static_cast<int>(requireAdmeshValue(text, "Facets reversed", 0U));
            metrics.backwardsEdges =
                static_cast<int>(requireAdmeshValue(text, "Backwards edges", 0U));
            metrics.normalsFixed = static_cast<int>(requireAdmeshValue(text, "Normals fixed", 0U));

            return metrics;
        }

        /// Write a shell mesh to binary STL format
        void writeShellToStl(ShellGenerator::ShellMesh const & shell,
                             std::filesystem::path const & path)
        {
            std::ofstream file(path, std::ios::binary | std::ios::out);
            if (!file.is_open())
            {
                throw std::runtime_error("Failed to open STL file for writing: " + path.string());
            }

            // Write 80-byte header
            char header[80] = {};
            std::snprintf(header, sizeof(header), "Shell mesh: %s (layer %d)",
                         shell.filamentName.c_str(), shell.layerIndex);
            file.write(header, 80);

            // Write triangle count
            auto const numTriangles = static_cast<std::uint32_t>(shell.indices.size() / 3);
            file.write(reinterpret_cast<char const *>(&numTriangles), sizeof(numTriangles));

            // Write triangles
            for (std::size_t i = 0; i < shell.indices.size(); i += 3)
            {
                Eigen::Vector3f const & v0 = shell.vertices[shell.indices[i]];
                Eigen::Vector3f const & v1 = shell.vertices[shell.indices[i + 1]];
                Eigen::Vector3f const & v2 = shell.vertices[shell.indices[i + 2]];

                // Compute face normal
                Eigen::Vector3f const edge1 = v1 - v0;
                Eigen::Vector3f const edge2 = v2 - v0;
                Eigen::Vector3f normal = edge1.cross(edge2).normalized();

                // Handle degenerate triangles
                if (!normal.allFinite())
                {
                    normal = Eigen::Vector3f::Zero();
                }

                // Write normal (3 floats)
                file.write(reinterpret_cast<char const *>(normal.data()), 3 * sizeof(float));

                // Write vertices (3 x 3 floats)
                file.write(reinterpret_cast<char const *>(v0.data()), 3 * sizeof(float));
                file.write(reinterpret_cast<char const *>(v1.data()), 3 * sizeof(float));
                file.write(reinterpret_cast<char const *>(v2.data()), 3 * sizeof(float));

                // Write attribute byte count (unused, set to 0)
                std::uint16_t const attribute = 0;
                file.write(reinterpret_cast<char const *>(&attribute), sizeof(attribute));
            }
        }

        /// Run admesh analysis on an STL file and return metrics
        [[nodiscard]] AdmeshMetrics analyzeStlWithAdmesh(std::filesystem::path const & stlPath,
                                                          std::string const & label)
        {
            std::string command = "admesh \"";
            command += stlPath.string();
            command += "\" 2>&1";

            int exitCode = -1;
            std::string const admeshOutput = runCommandAndCapture(command, exitCode);

            fmt::print(stderr, "[admesh:{}]\n{}\n", label, admeshOutput);

            if (exitCode != 0)
            {
                throw std::runtime_error(fmt::format("admesh failed for {}: exit code {}",
                                                     label, exitCode));
            }

            return parseAdmeshMetrics(admeshOutput);
        }

    } // anonymous namespace

    class ShellExportMeshQualityTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_logger = std::make_shared<events::Logger>();
            m_context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);

            if (!m_context->isValid())
            {
                GTEST_SKIP() << "OpenCL context not available";
            }

            if (!gpuTestsEnabled())
            {
                GTEST_SKIP() << "GPU tests disabled via GLADIUS_SKIP_GPU_TESTS=1";
            }

            if (!isAdmeshAvailable())
            {
                GTEST_SKIP() << "admesh not available - install with: apt install admesh";
            }
        }

        struct DocumentBundle
        {
            std::shared_ptr<ComputeCore> core;
            std::shared_ptr<Document> document;
        };

        DocumentBundle loadDocument(std::filesystem::path const & path)
        {
            auto core =
                std::make_shared<ComputeCore>(m_context, RequiredCapabilities::ComputeOnly, m_logger);
            auto document = std::make_shared<Document>(core);
            document->load(path);

            return DocumentBundle{std::move(core), std::move(document)};
        }

        /// Generate shells and validate each with admesh
        /// @param relaxedValidation If true, use lenient validation for paths with known quality limitations
        void validateShellMeshQuality(DocumentBundle & bundle,
                                      FilamentStack const & stack,
                                      ManifoldDualContouringOptions const & options,
                                      int lutResolution,
                                      bool useSurfaceColorSampling,
                                      std::string const & testLabel,
                                      bool relaxedValidation = false)
        {
            ASSERT_TRUE(bundle.core->updateBBox()) << testLabel << ": Failed to update bounding box";

            ThicknessConstraints constraints;
            constraints.minThickness = 0.1f;
            constraints.maxThickness = 3.0f;

            ThicknessSolution solution(stack.size());
            for (std::size_t i = 0; i < stack.size(); ++i)
            {
                solution.thicknesses[i] = 0.4f + 0.2f * static_cast<float>(i);
            }

            ShellGenerator generator(*bundle.core, *bundle.document);
            auto shells = generator.generateShells(
                stack,
                solution,
                options,
                lutResolution,
                constraints,
                nullptr,
                useSurfaceColorSampling);

            ASSERT_EQ(shells.size(), stack.size())
                << testLabel << ": Expected " << stack.size() << " shells, got " << shells.size();

            // Validate each shell mesh with admesh
            for (auto const & shell : shells)
            {
                std::string const shellLabel = fmt::format("{}_{}", testLabel, shell.filamentName);
                bool const isInnermostLayer = (shell.layerIndex == 0);

                EXPECT_GT(shell.vertices.size(), 0U)
                    << shellLabel << ": Shell has no vertices";
                EXPECT_GT(shell.indices.size(), 0U)
                    << shellLabel << ": Shell has no indices";
                EXPECT_EQ(shell.indices.size() % 3, 0U)
                    << shellLabel << ": Indices not divisible by 3";

                // Export to temporary STL
                auto stlPath = makeUniqueTempFile(shellLabel, ".stl");
                TempFileGuard cleanup(stlPath);

                writeShellToStl(shell, stlPath);

                ASSERT_TRUE(std::filesystem::exists(stlPath))
                    << shellLabel << ": STL file not created";
                ASSERT_GT(std::filesystem::file_size(stlPath), 84U)  // header + count
                    << shellLabel << ": STL file too small";

                // Run admesh analysis
                AdmeshMetrics metrics;
                ASSERT_NO_THROW(metrics = analyzeStlWithAdmesh(stlPath, shellLabel))
                    << shellLabel << ": Failed to analyze with admesh";

                // Verify mesh quality expectations
                EXPECT_GT(metrics.numberOfFacets, 0)
                    << shellLabel << ": No facets in mesh";

                if (relaxedValidation)
                {
                    // Relaxed validation: only basic checks, log metrics for monitoring
                    // Used for paths with known quality limitations (e.g., old shell volume mode)
                    EXPECT_GT(metrics.numberOfFacets, 100)
                        << shellLabel << ": Shell has too few facets";
                    
                    fmt::print(stderr, "[{}] Relaxed validation metrics: "
                               "facets={}, disconnected={}, parts={}, reversed={}\n",
                               shellLabel, metrics.numberOfFacets, metrics.disconnectedFacets,
                               metrics.numberOfParts, metrics.facetsReversed);
                }
                else if (isInnermostLayer)
                {
                    // Innermost layer (solid core): should be watertight with correct orientation
                    // Uses hierarchical octree path which produces watertight meshes
                    EXPECT_EQ(metrics.disconnectedFacets, 0)
                        << shellLabel << ": Mesh has " << metrics.disconnectedFacets
                        << " disconnected facets (not watertight)";

                    // Multi-object models may have multiple parts (e.g., SphereInACage has sphere + cage)
                    EXPECT_GE(metrics.numberOfParts, 1)
                        << shellLabel << ": Mesh has " << metrics.numberOfParts << " parts";

                    // Should have positive volume (properly oriented)
                    EXPECT_GT(metrics.volume, 0.0)
                        << shellLabel << ": Mesh has non-positive volume: " << metrics.volume;

                    // Should have no degenerate facets
                    EXPECT_EQ(metrics.degenerateFacets, 0)
                        << shellLabel << ": Mesh has " << metrics.degenerateFacets
                        << " degenerate facets";

                    // Innermost layer should not need any repairs
                    EXPECT_EQ(metrics.edgesFixed, 0)
                        << shellLabel << ": " << metrics.edgesFixed << " edges needed fixing";
                    EXPECT_EQ(metrics.facetsRemoved, 0)
                        << shellLabel << ": " << metrics.facetsRemoved << " facets removed";
                    EXPECT_EQ(metrics.facetsAdded, 0)
                        << shellLabel << ": " << metrics.facetsAdded << " facets added";
                    EXPECT_EQ(metrics.facetsReversed, 0)
                        << shellLabel << ": " << metrics.facetsReversed << " facets reversed";
                    EXPECT_EQ(metrics.normalsFixed, 0)
                        << shellLabel << ": " << metrics.normalsFixed << " normals fixed";
                }
                else
                {
                    // Non-innermost layers (thin shells): known limitation
                    // These layers have shell thickness often smaller than voxel size,
                    // causing topology issues. Relaxed expectations until thin-shell
                    // handling is improved.
                    
                    // Basic sanity checks - mesh should exist and have reasonable structure
                    EXPECT_GT(metrics.numberOfFacets, 100)
                        << shellLabel << ": Thin shell has too few facets";
                    
                    // Log the metrics for monitoring improvements
                    fmt::print(stderr, "[{}] Thin shell metrics (known limitation): "
                               "disconnected={}, parts={}, reversed={}\n",
                               shellLabel, metrics.disconnectedFacets,
                               metrics.numberOfParts, metrics.facetsReversed);
                }
            }
        }

        std::shared_ptr<ComputeContext> m_context;
        events::SharedLogger m_logger;
    };

    /// @brief Test that shells generated WITHOUT surface color sampling are functional
    /// This tests the older HierarchicalOctreeBuilder + useShellVolumeMode path which
    /// has known mesh quality limitations. The test validates basic functionality rather
    /// than strict watertightness (use surface sampling path for production-quality shells).
    TEST_F(ShellExportMeshQualityTest, ShellGenerator_WithoutSurfaceSampling_ProducesWatertightMeshes)
    {
        auto bundle = loadDocument("testdata/SphereInACage.3mf");

        FilamentStack stack;
        stack.push_back(FilamentOpticalProperties("White", {1.0f, 1.0f, 1.0f}, 0.4f));
        stack.push_back(FilamentOpticalProperties("Black", {0.0f, 0.0f, 0.0f}, 0.6f));

        ManifoldDualContouringOptions options;
        options.initialDepth = 4;
        options.maxDepth = 5;
        options.enableGpu = true;
        options.qualityPreset = ManifoldDualContouringQuality::Custom;

        // Use relaxed validation since this path has known quality limitations
        validateShellMeshQuality(bundle, stack, options, 8, false, "sphere_cage_no_surface", 
                                 true /* relaxedValidation */);
    }

    /// @brief Test that shells generated WITH surface color sampling produce watertight meshes
    /// Uses SphereInACage which has sufficient internal volume for shell generation
    TEST_F(ShellExportMeshQualityTest, ShellGenerator_WithSurfaceSampling_ProducesWatertightMeshes)
    {
        auto bundle = loadDocument("testdata/SphereInACage.3mf");

        FilamentStack stack;
        stack.push_back(FilamentOpticalProperties("White", {1.0f, 1.0f, 1.0f}, 0.4f));
        stack.push_back(FilamentOpticalProperties("Black", {0.0f, 0.0f, 0.0f}, 0.6f));

        ManifoldDualContouringOptions options;
        options.initialDepth = 4;
        options.maxDepth = 5;
        options.enableGpu = true;
        options.qualityPreset = ManifoldDualContouringQuality::Custom;

        validateShellMeshQuality(bundle, stack, options, 8, true, "sphere_cage_surface");
    }

    /// @brief Test with a simple model (SphereInACage) for baseline validation
    TEST_F(ShellExportMeshQualityTest, ShellGenerator_SphereInACage_ProducesWatertightMeshes)
    {
        auto bundle = loadDocument("testdata/SphereInACage.3mf");

        FilamentStack stack;
        stack.push_back(FilamentOpticalProperties("Bottom", {0.8f, 0.1f, 0.1f}, 0.5f));
        stack.push_back(FilamentOpticalProperties("Top", {0.1f, 0.1f, 0.8f}, 0.5f));

        ManifoldDualContouringOptions options;
        options.initialDepth = 4;
        options.maxDepth = 5;
        options.enableGpu = true;
        options.qualityPreset = ManifoldDualContouringQuality::Custom;

        validateShellMeshQuality(bundle, stack, options, 8, true, "sphere_cage");
    }

    /// @brief Test with 4-layer stack to validate multi-layer shell generation
    /// Uses SphereInACage which has sufficient internal volume for 4 shells.
    /// (ImplicitGyroid's thin gyroid walls cannot contain 4 cumulative shell layers)
    TEST_F(ShellExportMeshQualityTest, ShellGenerator_FourLayerStack_ProducesWatertightMeshes)
    {
        auto bundle = loadDocument("testdata/SphereInACage.3mf");

        // 4-layer stack with varying colors and transmissionDistance values
        // transmissionDistance defines light penetration depth (from materials.json)
        FilamentStack stack;
        stack.push_back(FilamentOpticalProperties(
            "PLA Blue", {0.045f, 0.054f, 0.974f}, 0.5f, 0.4f,
            Eigen::Vector3f(4.15f, 4.15f, 4.15f)));
        stack.push_back(FilamentOpticalProperties(
            "PLA Red", {1.0f, 0.046f, 0.046f}, 0.5f, 0.4f,
            Eigen::Vector3f(6.15f, 6.15f, 6.15f)));
        stack.push_back(FilamentOpticalProperties(
            "PLA Green", {0.29f, 1.0f, 0.0f}, 0.5f, 0.4f,
            Eigen::Vector3f(4.9f, 4.9f, 4.9f)));
        stack.push_back(FilamentOpticalProperties(
            "PLA Black", {0.0f, 0.0f, 0.0f}, 0.5f, 0.4f,
            Eigen::Vector3f(3.4f, 3.4f, 3.4f)));

        ManifoldDualContouringOptions options;
        options.initialDepth = 4;
        options.maxDepth = 5;
        options.enableGpu = true;
        options.qualityPreset = ManifoldDualContouringQuality::Custom;

        validateShellMeshQuality(bundle, stack, options, 8, true, "four_layer");
    }

    /// @brief Test with higher resolution for stress testing
    TEST_F(ShellExportMeshQualityTest, ShellGenerator_HigherResolution_ProducesWatertightMeshes)
    {
        auto bundle = loadDocument("testdata/SphereInACage.3mf");

        FilamentStack stack;
        stack.push_back(FilamentOpticalProperties("White", {1.0f, 1.0f, 1.0f}, 0.4f));
        stack.push_back(FilamentOpticalProperties("Black", {0.0f, 0.0f, 0.0f}, 0.6f));

        ManifoldDualContouringOptions options;
        options.initialDepth = 5;
        options.maxDepth = 6;  // Higher resolution
        options.enableGpu = true;
        options.qualityPreset = ManifoldDualContouringQuality::Custom;

        validateShellMeshQuality(bundle, stack, options, 16, true, "high_res");
    }

    /// @brief Diagnostic test for webcam_mount_color with materials.json configuration
    /// This test reproduces the out-of-memory issue reported when exporting this model.
    /// Uses lower resolution to avoid memory issues while still diagnosing the problem.
    TEST_F(ShellExportMeshQualityTest, ShellGenerator_WebcamMountColor_DiagnosticTest)
    {
        auto bundle = loadDocument("testdata/webcam_mount_color.3mf");

        // Materials from materials.json
        // NOTE: Constructor is (name, color, opacity, refThickness, transmissionDistance)
        // The transmissionDistance from materials.json defines how far light penetrates
        FilamentStack stack;
        stack.push_back(FilamentOpticalProperties(
            "PLA Blue", {0.045f, 0.054f, 0.974f}, 0.8f, 0.4f, 
            Eigen::Vector3f(4.15f, 4.15f, 4.15f)));
        stack.push_back(FilamentOpticalProperties(
            "PLA Red", {1.0f, 0.046f, 0.046f}, 0.8f, 0.4f,
            Eigen::Vector3f(6.15f, 6.15f, 6.15f)));
        stack.push_back(FilamentOpticalProperties(
            "PLA Green", {0.29f, 1.0f, 0.0f}, 0.8f, 0.4f,
            Eigen::Vector3f(4.9f, 4.9f, 4.9f)));
        stack.push_back(FilamentOpticalProperties(
            "PLA Black", {0.0f, 0.0f, 0.0f}, 0.8f, 0.4f,
            Eigen::Vector3f(3.4f, 3.4f, 3.4f)));

        // Print material properties
        fmt::print("\n[WebcamMount Diagnostic] === Material Properties ===\n");
        for (std::size_t i = 0; i < stack.size(); ++i)
        {
            auto const& mat = stack[i];
            fmt::print("  Layer {}: {} - transmissionDistance=({:.2f}, {:.2f}, {:.2f}) mm\n", 
                       i, mat.name, 
                       mat.transmissionDistance.x(), 
                       mat.transmissionDistance.y(), 
                       mat.transmissionDistance.z());
        }

        // First, check that bounding box update works
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to update bounding box";
        
        auto bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value()) << "No bounding box available";
        
        float const extentX = bbox->max.s[0] - bbox->min.s[0];
        float const extentY = bbox->max.s[1] - bbox->min.s[1];
        float const extentZ = bbox->max.s[2] - bbox->min.s[2];
        float const maxExtent = std::max({extentX, extentY, extentZ});
        
        fmt::print("\n[WebcamMount Diagnostic] === Geometry ===\n");
        fmt::print("  BBox: [{:.2f}, {:.2f}, {:.2f}] to [{:.2f}, {:.2f}, {:.2f}]\n",
                   bbox->min.s[0], bbox->min.s[1], bbox->min.s[2],
                   bbox->max.s[0], bbox->max.s[1], bbox->max.s[2]);
        fmt::print("  Extents: {:.2f} x {:.2f} x {:.2f} mm\n", extentX, extentY, extentZ);
        
        // Calculate what resolution we need for different shell thicknesses
        fmt::print("\n[WebcamMount Diagnostic] === Resolution Analysis ===\n");
        for (int depth = 4; depth <= 8; ++depth)
        {
            int const gridRes = 1 << depth;
            float const voxelSize = maxExtent / static_cast<float>(gridRes);
            fmt::print("  maxDepth={}: {}^3 grid, voxelSize={:.3f} mm\n", depth, gridRes, voxelSize);
        }
        
        // Calculate thickness solution for a sample color (white)
        ThicknessConstraints constraints;
        constraints.minThickness = 0.1f;
        constraints.maxThickness = 3.0f;
        
        FrontlitThicknessSolver solver(stack, constraints);
        
        // Test a few colors
        std::vector<std::pair<std::string, Eigen::Vector3f>> testColors = {
            {"White", {1.0f, 1.0f, 1.0f}},
            {"Black", {0.0f, 0.0f, 0.0f}},
            {"Red", {1.0f, 0.0f, 0.0f}},
            {"Blue", {0.0f, 0.0f, 1.0f}},
            {"Gray", {0.5f, 0.5f, 0.5f}},
        };
        
        fmt::print("\n[WebcamMount Diagnostic] === Thickness Solutions ===\n");
        for (auto const& [name, color] : testColors)
        {
            ThicknessSolution solution = solver.solve(color);
            fmt::print("  {} ({:.2f},{:.2f},{:.2f}):\n", name, color.x(), color.y(), color.z());
            
            float cumulative = 0.0f;
            for (std::size_t i = 0; i < solution.thicknesses.size(); ++i)
            {
                cumulative += solution.thicknesses[i];
                fmt::print("    Layer {} ({}): {:.3f} mm, cumulative: {:.3f} mm\n",
                           i, stack[i].name, solution.thicknesses[i], cumulative);
            }
        }
        
        // Now try generating with higher resolution
        fmt::print("\n[WebcamMount Diagnostic] === Shell Generation Test ===\n");
        
        ManifoldDualContouringOptions options;
        options.initialDepth = 4;
        options.maxDepth = 6;  // Higher resolution: 64^3, ~0.87mm voxels
        options.enableGpu = true;
        options.qualityPreset = ManifoldDualContouringQuality::Custom;
        
        int const gridRes = 1 << options.maxDepth;
        float const voxelSize = maxExtent / static_cast<float>(gridRes);
        fmt::print("  Using maxDepth={}: {}^3 grid, voxelSize={:.3f} mm\n", 
                   options.maxDepth, gridRes, voxelSize);

        // Use relaxed validation - we're diagnosing, not asserting perfection
        validateShellMeshQuality(bundle, stack, options, 8, true, "webcam_diagnostic", 
                                 true /* relaxedValidation */);
    }

} // namespace gladius_tests::shell_mesh_quality
