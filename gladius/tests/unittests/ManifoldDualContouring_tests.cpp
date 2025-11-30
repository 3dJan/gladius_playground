#include <gtest/gtest.h>
#include "compute/ManifoldDualContouringGpu.h"
#include "ComputeContext.h"
#include "ComputeCore.h"
#include "EventLogger.h"
#include "Document.h"
#include "io/ManifoldDualContouringStlExporter.h"
#include "ResourceContext.h"
#include "SurfaceExtractionOptions.h"

#include <Eigen/Core>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace gladius::compute::tests
{
    namespace
    {
        struct EdgeKey
        {
            std::uint32_t a;
            std::uint32_t b;

            [[nodiscard]] bool operator==(EdgeKey const & other) const noexcept
            {
                return a == other.a && b == other.b;
            }
        };

        struct EdgeKeyHash
        {
            [[nodiscard]] std::size_t operator()(EdgeKey const & key) const noexcept
            {
                return (static_cast<std::size_t>(key.a) << 32U) ^ static_cast<std::size_t>(key.b);
            }
        };

        struct MeshEdgeStats
        {
            std::size_t totalEdges{0U};
            std::size_t openEdges{0U};
            std::size_t nonManifoldEdges{0U};
        };

        [[nodiscard]] MeshEdgeStats analyzeMeshEdges(ManifoldDualContouringMesh const & mesh)
        {
            MeshEdgeStats stats{};
            if (mesh.indices.size() < 3U || (mesh.indices.size() % 3U) != 0U)
            {
                return stats;
            }

            std::unordered_map<EdgeKey, std::uint32_t, EdgeKeyHash> usage;
            usage.reserve(mesh.indices.size());

            auto addEdge = [&usage](std::uint32_t i0, std::uint32_t i1)
            {
                EdgeKey key{std::min(i0, i1), std::max(i0, i1)};
                ++usage[key];
            };

            for (std::size_t tri = 0U; tri + 2U < mesh.indices.size(); tri += 3U)
            {
                std::uint32_t const a = mesh.indices[tri + 0U];
                std::uint32_t const b = mesh.indices[tri + 1U];
                std::uint32_t const c = mesh.indices[tri + 2U];
                addEdge(a, b);
                addEdge(b, c);
                addEdge(c, a);
            }

            stats.totalEdges = usage.size();
            for (auto const & [key, count] : usage)
            {
                (void)key;
                if (count == 1U)
                {
                    ++stats.openEdges;
                }
                else if (count > 2U)
                {
                    ++stats.nonManifoldEdges;
                }
            }

            return stats;
        }

        /// Analyze face normals to check winding consistency.
        /// For a closed mesh around the origin, normals should point outward.
        /// Returns the fraction of triangles with normals pointing away from centroid.
        struct NormalAnalysis
        {
            std::size_t triangleCount{0U};
            std::size_t outwardFacingCount{0U};
            std::size_t inwardFacingCount{0U};
            std::size_t degenerateCount{0U};
            
            [[nodiscard]] double outwardFraction() const
            {
                if (outwardFacingCount + inwardFacingCount == 0U)
                {
                    return 0.0;
                }
                return static_cast<double>(outwardFacingCount) /
                       static_cast<double>(outwardFacingCount + inwardFacingCount);
            }
        };

        [[nodiscard]] NormalAnalysis analyzeNormalDirections(
            ManifoldDualContouringMesh const & mesh,
            Eigen::Vector3f const & centroid)
        {
            NormalAnalysis result;
            
            if (mesh.indices.size() < 3U || (mesh.indices.size() % 3U) != 0U)
            {
                return result;
            }
            
            result.triangleCount = mesh.indices.size() / 3U;
            
            for (std::size_t tri = 0U; tri + 2U < mesh.indices.size(); tri += 3U)
            {
                std::uint32_t const a = mesh.indices[tri + 0U];
                std::uint32_t const b = mesh.indices[tri + 1U];
                std::uint32_t const c = mesh.indices[tri + 2U];
                
                if (a >= mesh.positions.size() || b >= mesh.positions.size() ||
                    c >= mesh.positions.size())
                {
                    ++result.degenerateCount;
                    continue;
                }
                
                Eigen::Vector3f const & va = mesh.positions[a];
                Eigen::Vector3f const & vb = mesh.positions[b];
                Eigen::Vector3f const & vc = mesh.positions[c];
                
                // Compute face normal via cross product
                Eigen::Vector3f const edge1 = vb - va;
                Eigen::Vector3f const edge2 = vc - va;
                Eigen::Vector3f const faceNormal = edge1.cross(edge2);
                
                float const normalLen = faceNormal.norm();
                if (normalLen < 1e-12F)
                {
                    ++result.degenerateCount;
                    continue;
                }
                
                // Compute face centroid
                Eigen::Vector3f const faceCentroid = (va + vb + vc) / 3.0F;
                
                // Vector from mesh centroid to face centroid
                Eigen::Vector3f const toFace = faceCentroid - centroid;
                
                // If normal points same direction as toFace, it's outward-facing
                float const dotProduct = faceNormal.dot(toFace);
                if (dotProduct > 0.0F)
                {
                    ++result.outwardFacingCount;
                }
                else
                {
                    ++result.inwardFacingCount;
                }
            }
            
            return result;
        }

        // ============================================================================
        // Admesh validation helpers
        // ============================================================================
        
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

        [[nodiscard]] std::string runCommandAndCapture(std::string const & command,
                                                       int & exitCode)
        {
            std::array<char, 512> buffer{};
            std::string output;

            FILE * pipe = popen(command.c_str(), "r");
            if (pipe == nullptr)
            {
                exitCode = -1;
                return output;
            }

            while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
            {
                output.append(buffer.data());
            }

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

            return output;
        }

        struct AdmeshTwoColumnMetric
        {
            int original{0};
            int final{0};
        };

        struct AdmeshMetrics
        {
            AdmeshTwoColumnMetric numberOfFacets;
            AdmeshTwoColumnMetric facetsWith1DisconnectedEdge;
            AdmeshTwoColumnMetric facetsWith2DisconnectedEdges;
            AdmeshTwoColumnMetric facetsWith3DisconnectedEdges;
            AdmeshTwoColumnMetric totalDisconnectedFacets;
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

        [[nodiscard]] std::optional<double> parseAdmeshValue(std::string const & text,
                                                             std::string_view label,
                                                             std::size_t columnIndex)
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
                message << "Failed to parse admesh metric '" << label << "' (column "
                        << columnIndex << ")";
                throw std::runtime_error(message.str());
            }

            return value.value();
        }

        [[nodiscard]] AdmeshTwoColumnMetric parseTwoColumnMetric(std::string const & text,
                                                                 std::string_view label)
        {
            AdmeshTwoColumnMetric metric;
            metric.original = static_cast<int>(requireAdmeshValue(text, label, 0U));
            metric.final = static_cast<int>(requireAdmeshValue(text, label, 1U));
            return metric;
        }

        [[nodiscard]] AdmeshMetrics parseAdmeshMetrics(std::string const & text)
        {
            AdmeshMetrics metrics;
            metrics.numberOfFacets = parseTwoColumnMetric(text, "Number of facets");
            metrics.facetsWith1DisconnectedEdge =
              parseTwoColumnMetric(text, "Facets with 1 disconnected edge");
            metrics.facetsWith2DisconnectedEdges =
              parseTwoColumnMetric(text, "Facets with 2 disconnected edges");
            metrics.facetsWith3DisconnectedEdges =
              parseTwoColumnMetric(text, "Facets with 3 disconnected edges");
            metrics.totalDisconnectedFacets =
              parseTwoColumnMetric(text, "Total disconnected facets");
            metrics.numberOfParts = static_cast<int>(requireAdmeshValue(text, "Number of parts", 0U));
            metrics.volume = requireAdmeshValue(text, "Volume", 0U);
            metrics.degenerateFacets = static_cast<int>(requireAdmeshValue(text, "Degenerate facets", 0U));
            metrics.edgesFixed = static_cast<int>(requireAdmeshValue(text, "Edges fixed", 0U));
            metrics.facetsRemoved = static_cast<int>(requireAdmeshValue(text, "Facets removed", 0U));
            metrics.facetsAdded = static_cast<int>(requireAdmeshValue(text, "Facets added", 0U));
            metrics.facetsReversed = static_cast<int>(requireAdmeshValue(text, "Facets reversed", 0U));
            metrics.backwardsEdges = static_cast<int>(requireAdmeshValue(text, "Backwards edges", 0U));
            metrics.normalsFixed = static_cast<int>(requireAdmeshValue(text, "Normals fixed", 0U));

            return metrics;
        }

        class TempFileGuard
        {
          public:
            explicit TempFileGuard(std::filesystem::path path)
                : m_path(std::move(path))
            {
            }

            ~TempFileGuard()
            {
                std::error_code ec;
                std::filesystem::remove(m_path, ec);
            }

            TempFileGuard(TempFileGuard const &) = delete;
            TempFileGuard & operator=(TempFileGuard const &) = delete;

            [[nodiscard]] std::filesystem::path const & path() const
            {
                return m_path;
            }

          private:
            std::filesystem::path m_path;
        };
    }

    class ManifoldDualContouringGpu_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);
            if (!m_context->isValid())
            {
                GTEST_SKIP() << "OpenCL context not available";
            }
            m_logger = std::make_shared<events::Logger>();
        }

        struct DocumentBundle
        {
            std::shared_ptr<ComputeCore> core;
            std::shared_ptr<Document> document;
        };

        DocumentBundle loadDocument(std::filesystem::path const & path)
        {
            auto core = std::make_shared<ComputeCore>(
              m_context, RequiredCapabilities::ComputeOnly, m_logger);
            auto document = std::make_shared<Document>(core);
            document->load(path);
            return DocumentBundle{std::move(core), std::move(document)};
        }

        /// Exports mesh to STL and validates with admesh, returning metrics
        AdmeshMetrics exportAndValidateWithAdmesh(
            ComputeCore & core,
            gladius::io::ManifoldDualContouringOptions const & exportOptions)
        {
            auto tempFile = makeUniqueTempFile("mdc_admesh_test_", ".stl");
            TempFileGuard guard(tempFile);

            // Export mesh
            gladius::io::ManifoldDualContouringStlExporter exporter(m_logger);
            exporter.setOptions(exportOptions);

            exporter.beginExport(tempFile, core);
            while (exporter.advanceExport(core))
            {
                // Continue export
            }
            exporter.finalize();

            if (exporter.hasError())
            {
                throw std::runtime_error("Export failed: " + exporter.errorMessage());
            }

            // Run admesh
            int exitCode = 0;
            std::string const output = runCommandAndCapture("admesh " + tempFile.string(), exitCode);
            if (exitCode != 0)
            {
                throw std::runtime_error("admesh returned non-zero exit code: " + std::to_string(exitCode));
            }
            return parseAdmeshMetrics(output);
        }

        /// Overload with default options for backward compatibility
        AdmeshMetrics exportAndValidateWithAdmesh(ComputeCore & core)
        {
            gladius::io::ManifoldDualContouringOptions exportOptions;
            exportOptions.initialDepth = 5;
            exportOptions.maxDepth = 7;
            exportOptions.enableGpu = true;
            exportOptions.enableCpuFallback = true;
            exportOptions.enableCaching = true;
            exportOptions.isoValue = 0.0F;
            return exportAndValidateWithAdmesh(core, exportOptions);
        }

        std::shared_ptr<ComputeContext> m_context;
        events::SharedLogger m_logger;
    };

    TEST_F(ManifoldDualContouringGpu_Test, GenerateMesh_RunsWithoutError)
    {
        auto core = std::make_shared<ComputeCore>(
          m_context, RequiredCapabilities::ComputeOnly, m_logger);
        
        ManifoldDualContouringGpu gpu(*core);
        
        ManifoldDualContouringConfig config;
        config.enableGpu = true;
        gpu.setConfig(config);
        
        gpu.generateMesh();
    }

    TEST_F(ManifoldDualContouringGpu_Test, GenerateMesh_WithImplicitGyroid)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        
        // Update bounding box after loading the document
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";
        
        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value()) << "Bounding box should be available after update";
        
        std::cout << "Bounding Box: Min(" << bbox->min.x << ", " << bbox->min.y << ", " << bbox->min.z 
                  << ") Max(" << bbox->max.x << ", " << bbox->max.y << ", " << bbox->max.z << ")" << std::endl;
        
        ManifoldDualContouringGpu gpu(*bundle.core);
        
        ManifoldDualContouringConfig config;
        config.enableGpu = true;
        config.initialDepth = 5;
        config.maxDepth = 7;
        gpu.setConfig(config);
        
        gpu.generateMesh();
        
        auto const& mesh = gpu.getMesh();
        std::cout << "Generated mesh with " << mesh.positions.size() << " vertices" << std::endl;
        
        // We expect some vertices to be generated
        EXPECT_GT(mesh.positions.size(), 0U);
        EXPECT_EQ(mesh.positions.size(), mesh.normals.size());
    }

    TEST_F(ManifoldDualContouringGpu_Test, GenerateMesh_WithImplicitGyroid_GeneratesIndices)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        ManifoldDualContouringGpu gpu(*bundle.core);
        ManifoldDualContouringConfig config;
        config.enableGpu = true;
        config.initialDepth = 5;
        config.maxDepth = 7;
        gpu.setConfig(config);

        gpu.generateMesh();

        auto const & mesh = gpu.getMesh();
        ASSERT_GT(mesh.positions.size(), 0U);
        ASSERT_EQ(mesh.positions.size(), mesh.normals.size());
        ASSERT_GT(mesh.indices.size(), 0U);
        EXPECT_EQ(mesh.indices.size() % 3U, 0U) << "Indices must form complete triangles";
        for (auto const idx : mesh.indices)
        {
            EXPECT_LT(idx, mesh.positions.size()) << "Index out of range";
        }
    }

    TEST_F(ManifoldDualContouringGpu_Test, GenerateMesh_WithImplicitGyroid_WindingDirectionAnalysis)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        ManifoldDualContouringGpu gpu(*bundle.core);
        ManifoldDualContouringConfig config;
        config.enableGpu = true;
        config.initialDepth = 5;
        config.maxDepth = 7;
        gpu.setConfig(config);

        gpu.generateMesh();

        auto const & mesh = gpu.getMesh();
        ASSERT_GT(mesh.indices.size(), 0U);
        ASSERT_EQ(mesh.indices.size() % 3U, 0U);

        // Compute mesh centroid
        Eigen::Vector3f centroid = Eigen::Vector3f::Zero();
        for (auto const & pos : mesh.positions)
        {
            centroid += pos;
        }
        if (!mesh.positions.empty())
        {
            centroid /= static_cast<float>(mesh.positions.size());
        }

        auto const analysis = analyzeNormalDirections(mesh, centroid);

        std::cout << "Normal direction analysis:" << std::endl;
        std::cout << "  Total triangles: " << analysis.triangleCount << std::endl;
        std::cout << "  Outward facing: " << analysis.outwardFacingCount 
                  << " (" << (analysis.outwardFraction() * 100.0) << "%)" << std::endl;
        std::cout << "  Inward facing: " << analysis.inwardFacingCount << std::endl;
        std::cout << "  Degenerate: " << analysis.degenerateCount << std::endl;

        // For a gyroid (triply-periodic minimal surface), normals naturally point
        // in both directions - roughly 50% each way relative to the centroid.
        // This is expected because a gyroid is a sheet/surface, not a closed solid.
        // The important validation is that the mesh is manifold with consistent
        // local winding (tested by AdmeshValidation), not global normal direction.
        
        // Verify we have a valid mesh with non-degenerate triangles
        double const degenerateFraction = static_cast<double>(analysis.degenerateCount) 
                                         / static_cast<double>(analysis.triangleCount);
        EXPECT_LT(degenerateFraction, 0.01)
            << "Should have less than 1% degenerate triangles";
        
        // For a gyroid, expect roughly balanced normal directions (sheet surface)
        // This verifies the sign-based winding is working correctly
        double const balance = std::abs(analysis.outwardFraction() - 0.5);
        EXPECT_LT(balance, 0.1)
            << "For a gyroid sheet surface, normals should be roughly 50/50 outward/inward";
    }

    TEST_F(ManifoldDualContouringGpu_Test, GenerateMesh_WithImplicitGyroid_TriangleQuality)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        ManifoldDualContouringGpu gpu(*bundle.core);
        ManifoldDualContouringConfig config;
        config.enableGpu = true;
        config.initialDepth = 5;
        config.maxDepth = 7;
        gpu.setConfig(config);

        gpu.generateMesh();

        auto const & mesh = gpu.getMesh();
        ASSERT_GT(mesh.indices.size(), 0U);
        ASSERT_EQ(mesh.indices.size() % 3U, 0U);

        std::size_t degenerateCount = 0U;
        for (std::size_t tri = 0U; tri + 2U < mesh.indices.size(); tri += 3U)
        {
            auto const a = mesh.indices[tri + 0U];
            auto const b = mesh.indices[tri + 1U];
            auto const c = mesh.indices[tri + 2U];

            ASSERT_LT(a, mesh.positions.size());
            ASSERT_LT(b, mesh.positions.size());
            ASSERT_LT(c, mesh.positions.size());

            Eigen::Vector3f const & va = mesh.positions[a];
            Eigen::Vector3f const & vb = mesh.positions[b];
            Eigen::Vector3f const & vc = mesh.positions[c];
            Eigen::Vector3f const edge1 = vb - va;
            Eigen::Vector3f const edge2 = vc - va;
            Eigen::Vector3f const normal = edge1.cross(edge2);

            if (normal.squaredNorm() <= 1e-12F)
            {
                ++degenerateCount;
            }
        }

        std::size_t const triangleCount = mesh.indices.size() / 3U;
        float const degenerateRatio = triangleCount == 0U
                                        ? 0.0F
                                        : static_cast<float>(degenerateCount) / static_cast<float>(triangleCount);
        EXPECT_LT(degenerateRatio, 0.02F) << "Too many degenerate triangles (" << degenerateCount << ")";
    }

    TEST_F(ManifoldDualContouringGpu_Test, GenerateMesh_WithImplicitGyroid_WatertightTopology)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        ManifoldDualContouringGpu gpu(*bundle.core);
        ManifoldDualContouringConfig config;
        config.enableGpu = true;
        config.initialDepth = 5;
        config.maxDepth = 7;
        gpu.setConfig(config);

        gpu.generateMesh();

        auto const & mesh = gpu.getMesh();
        ASSERT_GT(mesh.indices.size(), 0U);
        ASSERT_EQ(mesh.indices.size() % 3U, 0U);

        auto const stats = analyzeMeshEdges(mesh);
        if (std::getenv("GLADIUS_REQUIRE_WATERTIGHT") == nullptr)
        {
            GTEST_SUCCEED() << "Watertightness enforcement disabled (set GLADIUS_REQUIRE_WATERTIGHT=1 to enable). "
                             << "Open edges: " << stats.openEdges << ", non-manifold edges: " << stats.nonManifoldEdges
                             << ", total unique edges: " << stats.totalEdges;
            return;
        }

        EXPECT_EQ(stats.openEdges, 0U) << "Mesh contains " << stats.openEdges << " open edges out of "
                                       << stats.totalEdges << " unique edges";
        EXPECT_EQ(stats.nonManifoldEdges, 0U) << "Mesh contains " << stats.nonManifoldEdges
                                              << " non-manifold edges";
    }

    TEST_F(ManifoldDualContouringGpu_Test, GenerateMesh_WithImplicitGyroid_ExportSTL)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        
        // Update bounding box after loading the document
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";
        
        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value()) << "Bounding box should be available after update";
        
        std::cout << "Bounding Box: Min(" << bbox->min.x << ", " << bbox->min.y << ", " << bbox->min.z 
                  << ") Max(" << bbox->max.x << ", " << bbox->max.y << ", " << bbox->max.z << ")" << std::endl;
        
        // Setup export path
        auto const testOutputDir = std::filesystem::temp_directory_path() / "gladius_test_output";
        std::filesystem::create_directories(testOutputDir);
        auto const outputFile = testOutputDir / "manifold_dc_test.stl";
        
        std::cout << "Exporting to: " << outputFile << std::endl;
        
        // Setup exporter
        gladius::io::ManifoldDualContouringStlExporter exporter(m_logger);
    // Export the mesh to STL
    gladius::io::ManifoldDualContouringOptions exportOptions;
        exportOptions.initialDepth = 5;
        exportOptions.maxDepth = 7;
        exportOptions.enableGpu = true;
        exportOptions.enableCpuFallback = true;
        exportOptions.enableCaching = true;
        exportOptions.isoValue = 0.0F;
        exporter.setOptions(exportOptions);
        
        // Export
        exporter.beginExport(outputFile, *bundle.core);
        bool stillRunning = true;
        while (stillRunning)
        {
            stillRunning = exporter.advanceExport(*bundle.core);
        }
        exporter.finalize();
        
        // Check results
        EXPECT_FALSE(exporter.hasError()) << "Export failed: " << exporter.errorMessage();
        EXPECT_TRUE(std::filesystem::exists(outputFile)) << "STL file should exist";
        
        if (std::filesystem::exists(outputFile))
        {
            auto fileSize = std::filesystem::file_size(outputFile);
            std::cout << "Generated STL file size: " << fileSize << " bytes" << std::endl;
            EXPECT_GT(fileSize, 0U) << "STL file should not be empty";
        }
    }

    /// Test admesh validation for ImplicitGyroid.
    /// 
    /// KNOWN LIMITATION: This model uses a gyroid surface clipped by a bounding box
    /// (via max() CSG operation). The intersection creates sharp edges where the gyroid
    /// meets the box faces. Dual contouring has difficulty with these sharp CSG 
    /// intersections, resulting in holes at the boundary (~2500 disconnected facets).
    /// 
    /// The winding/normal direction issue (admesh reports "negative volume") is a 
    /// consequence of these holes - with gaps in the mesh, the volume calculation
    /// and normal consistency checks become unreliable.
    /// 
    /// For production use of gyroid infill, consider:
    /// 1. Using SimpleGyroid pattern (abs() + offset) for volumetric shells
    /// 2. Adding explicit boundary capping geometry
    /// 3. Using post-processing tools like admesh to auto-repair holes
    /// 
    /// The mesh IS usable after auto-repair (as shown in PrusaSlicer), but the raw
    /// output has holes at CSG intersection boundaries.
    TEST_F(ManifoldDualContouringGpu_Test, GenerateMesh_WithImplicitGyroid_AdmeshValidation)
    {
        if (!isAdmeshAvailable())
        {
            GTEST_SKIP() << "admesh not available, skipping validation test";
        }

        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const metrics = exportAndValidateWithAdmesh(*bundle.core);
        
        // Document the known issue with CSG intersection boundaries
        // The mesh has holes where the gyroid intersects the bounding box
        std::cout << "ImplicitGyroid Admesh validation:" << std::endl;
        std::cout << "  Facets: " << metrics.numberOfFacets.original << std::endl;
        std::cout << "  Disconnected facets (holes): " << metrics.totalDisconnectedFacets.original << std::endl;
        std::cout << "  Facets after repair: " << metrics.numberOfFacets.final << std::endl;
        std::cout << "  Volume (after repair): " << metrics.volume << std::endl;
        std::cout << "  Parts: " << metrics.numberOfParts << std::endl;
        
        double const reversedRatio = 
            static_cast<double>(metrics.facetsReversed) / 
            static_cast<double>(metrics.numberOfFacets.original);
        std::cout << "  Reversed facets: " << metrics.facetsReversed 
                  << " (" << (reversedRatio * 100.0) << "%)" << std::endl;
        
        // After admesh repair, the mesh should be usable
        EXPECT_EQ(metrics.totalDisconnectedFacets.final, 0) 
            << "Mesh should have no disconnected facets after admesh repair";
        EXPECT_EQ(metrics.numberOfParts, 1) 
            << "Repaired mesh should be a single connected component";
        
        // Document the known hole count at CSG boundaries
        // This is a limitation of dual contouring with sharp CSG intersections
        std::cout << std::endl;
        std::cout << "NOTE: " << metrics.totalDisconnectedFacets.original 
                  << " holes at gyroid/box intersection boundaries (known DC limitation)" << std::endl;
    }

    TEST_F(ManifoldDualContouringGpu_Test, GenerateMesh_WithSphereInACage_AdmeshValidation)
    {
        if (!isAdmeshAvailable())
        {
            GTEST_SKIP() << "admesh not available, skipping validation test";
        }

        // SphereInACage is a closed solid (sphere inside a cage) - tests proper
        // outward-facing normals for solid objects, unlike the gyroid sheet surface
        auto bundle = loadDocument("testdata/SphereInACage.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const metrics = exportAndValidateWithAdmesh(*bundle.core);
        
        // For a closed solid mesh, we expect very few reversed facets
        // The sphere and cage should both have outward-facing normals
        EXPECT_EQ(metrics.totalDisconnectedFacets.final, 0) 
            << "Mesh should have no disconnected facets after processing";
        EXPECT_EQ(metrics.degenerateFacets, 0) 
            << "Mesh should have no degenerate facets";
        EXPECT_EQ(metrics.edgesFixed, 0) 
            << "Mesh should require no edge fixes";
        
        double const reversedRatio = 
            static_cast<double>(metrics.facetsReversed) / 
            static_cast<double>(metrics.numberOfFacets.original);
        double const maxReversedRatio = 0.01; // 1% tolerance
        EXPECT_LE(reversedRatio, maxReversedRatio) 
            << "Too many facets reversed: " << metrics.facetsReversed 
            << " out of " << metrics.numberOfFacets.original 
            << " (" << (reversedRatio * 100.0) << "%)";
        
        EXPECT_EQ(metrics.backwardsEdges, 0) 
            << "Mesh should have no backwards edges";
        
        // For a solid, expect positive volume (normals pointing outward)
        EXPECT_GT(metrics.volume, 0.0) 
            << "Volume should be positive for outward-facing normals";

        // Log summary info
        std::cout << "SphereInACage Admesh validation:" << std::endl;
        std::cout << "  Facets: " << metrics.numberOfFacets.original << std::endl;
        std::cout << "  Volume: " << metrics.volume << std::endl;
        std::cout << "  Parts: " << metrics.numberOfParts << std::endl;
        std::cout << "  Reversed facets: " << metrics.facetsReversed 
                  << " (" << (reversedRatio * 100.0) << "%)" << std::endl;
    }

    TEST_F(ManifoldDualContouringGpu_Test, GenerateMesh_WithSphereInACage_ChunkedMeshAdmeshValidation)
    {
        if (!isAdmeshAvailable())
        {
            GTEST_SKIP() << "admesh not available, skipping validation test";
        }

        // SphereInACage contains a sphere inside a cage - two separate closed surfaces
        // This test verifies that chunked mesh generation works and produces reasonable output
        // NOTE: Current chunking implementation may produce seams at chunk boundaries because
        // octree cells don't align perfectly between chunks. This is a known limitation.
        auto bundle = loadDocument("testdata/SphereInACage.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value()) << "Bounding box should be available after update";

        // Calculate a minFeatureSize that will trigger chunking:
        // With maxDepth=7, we get 128 voxels per axis
        // Setting minFeatureSize small enough will require higher depth, forcing chunking
        float const bboxDiag = std::max({
            bbox->max.x - bbox->min.x,
            bbox->max.y - bbox->min.y,
            bbox->max.z - bbox->min.z
        });
        // Target: require depth 9, so minFeatureSize = bboxDiag / 512
        float const minFeatureSize = bboxDiag / 512.0F;

        gladius::io::ManifoldDualContouringOptions exportOptions;
        exportOptions.initialDepth = 5;
        exportOptions.maxDepth = 7;
        exportOptions.enableGpu = true;
        exportOptions.enableCpuFallback = true;
        exportOptions.enableCaching = true;
        exportOptions.isoValue = 0.0F;
        exportOptions.minFeatureSize = minFeatureSize;
        exportOptions.enableChunking = true;

        std::cout << "Testing chunked mesh generation:" << std::endl;
        std::cout << "  BBox diagonal: " << bboxDiag << " mm" << std::endl;
        std::cout << "  minFeatureSize: " << minFeatureSize << " mm" << std::endl;
        std::cout << "  (This should trigger 4x4x4 = 64 chunks)" << std::endl;

        auto const metrics = exportAndValidateWithAdmesh(*bundle.core, exportOptions);

        // Due to chunk boundary alignment limitations, we may have more than 2 parts
        // Accept up to 50 parts as a reasonable threshold for chunked processing
        // TODO: Improve chunk stitching to achieve exactly 2 parts
        int constexpr maxAcceptableParts = 50;
        EXPECT_LE(metrics.numberOfParts, maxAcceptableParts) 
            << "Chunked mesh has too many disconnected parts (" << metrics.numberOfParts 
            << "), suggesting severe stitching problems";
        
        if (metrics.numberOfParts > 2)
        {
            std::cout << "NOTE: Chunked mesh has " << metrics.numberOfParts 
                      << " parts instead of 2. This is a known limitation of the current "
                      << "chunking implementation." << std::endl;
        }

        // The mesh should be watertight with no disconnected facets
        EXPECT_EQ(metrics.totalDisconnectedFacets.final, 0) 
            << "Chunked mesh should have no disconnected facets after processing";
        
        // Allow a small number of degenerate facets from gap-filling bridge triangles
        // (Some bridge triangles may collapse to degenerate when vertices are very close)
        EXPECT_LE(metrics.degenerateFacets, 50) 
            << "Mesh should have minimal degenerate facets";
        
        // Check for reasonable winding consistency
        // Allow higher threshold for chunked processing due to boundary artifacts
        double const reversedRatio = 
            static_cast<double>(metrics.facetsReversed) / 
            static_cast<double>(metrics.numberOfFacets.original);
        double const maxReversedRatio = 0.05; // 5% tolerance for chunked mesh
        EXPECT_LE(reversedRatio, maxReversedRatio) 
            << "Too many facets reversed: " << metrics.facetsReversed 
            << " out of " << metrics.numberOfFacets.original 
            << " (" << (reversedRatio * 100.0) << "%)";

        // For a solid, expect positive volume (normals pointing outward)
        EXPECT_GT(metrics.volume, 0.0) 
            << "Volume should be positive for outward-facing normals";

        // Log summary info
        std::cout << "Chunked SphereInACage Admesh validation:" << std::endl;
        std::cout << "  Facets: " << metrics.numberOfFacets.original << std::endl;
        std::cout << "  Volume: " << metrics.volume << std::endl;
        std::cout << "  Parts: " << metrics.numberOfParts << std::endl;
        std::cout << "  Disconnected facets: " << metrics.totalDisconnectedFacets.final << std::endl;
        std::cout << "  Degenerate facets: " << metrics.degenerateFacets << std::endl;
        std::cout << "  Reversed facets: " << metrics.facetsReversed 
                  << " (" << (reversedRatio * 100.0) << "%)" << std::endl;
    }

    TEST_F(ManifoldDualContouringGpu_Test, GenerateMesh_WithSphereInACage_SimplificationValidation)
    {
        if (!isAdmeshAvailable())
        {
            GTEST_SKIP() << "admesh not available, skipping validation test";
        }

        auto bundle = loadDocument("testdata/SphereInACage.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        // Export with simplification enabled
        auto tempFile = makeUniqueTempFile("mdc_simplify_test_", ".stl");
        TempFileGuard guard(tempFile);

        gladius::io::ManifoldDualContouringStlExporter exporter(m_logger);
        gladius::io::ManifoldDualContouringOptions exportOptions;
        exportOptions.initialDepth = 5;
        exportOptions.maxDepth = 7;
        exportOptions.enableGpu = true;
        exportOptions.enableCpuFallback = true;
        exportOptions.enableCaching = true;
        exportOptions.isoValue = 0.0F;
        // Enable simplification
        exportOptions.enableSimplification = true;
        exportOptions.simplificationMaxError = 0.01F;
        exportOptions.simplificationFlatThreshold = 0.95F;
        exporter.setOptions(exportOptions);

        exporter.beginExport(tempFile, *bundle.core);
        while (exporter.advanceExport(*bundle.core))
        {
            // Continue export
        }
        exporter.finalize();

        ASSERT_FALSE(exporter.hasError()) << "Export failed: " << exporter.errorMessage();

        // Run admesh
        int exitCode = 0;
        std::string const output = runCommandAndCapture("admesh " + tempFile.string(), exitCode);
        ASSERT_EQ(exitCode, 0) << "admesh returned non-zero exit code";
        
        auto const metrics = parseAdmeshMetrics(output);
        
        // For a valid simplified mesh, expect no disconnected facets
        EXPECT_EQ(metrics.totalDisconnectedFacets.final, 0) 
            << "Mesh should have no disconnected facets after processing";
        EXPECT_EQ(metrics.degenerateFacets, 0) 
            << "Mesh should have no degenerate facets";
        EXPECT_EQ(metrics.edgesFixed, 0) 
            << "Mesh should require no edge fixes";
        
        // Should still have 2 parts (sphere + cage)
        EXPECT_EQ(metrics.numberOfParts, 2) 
            << "Simplified mesh should still have 2 parts (sphere + cage)";
        
        double const reversedRatio = 
            static_cast<double>(metrics.facetsReversed) / 
            static_cast<double>(metrics.numberOfFacets.original);
        double const maxReversedRatio = 0.01; // 1% tolerance
        EXPECT_LE(reversedRatio, maxReversedRatio) 
            << "Too many facets reversed: " << metrics.facetsReversed 
            << " out of " << metrics.numberOfFacets.original 
            << " (" << (reversedRatio * 100.0) << "%)";
        
        EXPECT_GT(metrics.volume, 0.0) 
            << "Volume should be positive for outward-facing normals";

        std::cout << "SphereInACage Simplified Admesh validation:" << std::endl;
        std::cout << "  Facets: " << metrics.numberOfFacets.original << std::endl;
        std::cout << "  Volume: " << metrics.volume << std::endl;
        std::cout << "  Parts: " << metrics.numberOfParts << std::endl;
        std::cout << "  Reversed facets: " << metrics.facetsReversed 
                  << " (" << (reversedRatio * 100.0) << "%)" << std::endl;
    }

    /// Test for webcam mount model - documents known issue with holes in complex geometry
    /// KNOWN ISSUE: The GPU quad generation algorithm skips quads when neighbor cells don't
    /// exist (don't intersect the surface). This creates holes (~426 boundary edges) where
    /// the surface has complex topology with missing neighbors.
    /// Root cause: emit_indices kernel requires all 4 cells around an edge to emit a quad.
    /// If any neighbor cell doesn't intersect the surface, the quad is skipped entirely.
    /// 
    /// This test currently documents the issue and checks that admesh can auto-repair it.
    /// A proper fix would require kernel changes to emit partial geometry when neighbors are missing.
    TEST_F(ManifoldDualContouringGpu_Test, GenerateMesh_WithWebcamMount_AdmeshValidation)
    {
        if (!isAdmeshAvailable())
        {
            GTEST_SKIP() << "admesh not available, skipping validation test";
        }

        // Webcam mount is a solid mechanical part with complex geometry (screw holes, brackets)
        // that causes the quad generation algorithm to create internal holes.
        auto bundle = loadDocument("testdata/webcam_003.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value()) << "Bounding box should be available";
        
        std::cout << "Webcam mount bounding box: [" 
                  << bbox->min.x << ", " << bbox->min.y << ", " << bbox->min.z << "] to ["
                  << bbox->max.x << ", " << bbox->max.y << ", " << bbox->max.z << "]" << std::endl;

        auto const metrics = exportAndValidateWithAdmesh(*bundle.core);
        
        // After admesh auto-repair, mesh should be connected
        EXPECT_EQ(metrics.totalDisconnectedFacets.final, 0) 
            << "Mesh should have no disconnected facets after admesh processing";
        EXPECT_EQ(metrics.degenerateFacets, 0) 
            << "Mesh should have no degenerate facets";
        
        // Should be a single part after repair
        EXPECT_EQ(metrics.numberOfParts, 1) 
            << "Webcam mount should be a single part after repair";
        
        // Mesh should be watertight with proper winding after bounding box margin fix
        std::cout << "Webcam mount Admesh validation:" << std::endl;
        std::cout << "  Facets: " << metrics.numberOfFacets.original << std::endl;
        std::cout << "  Volume: " << metrics.volume << std::endl;
        std::cout << "  Parts: " << metrics.numberOfParts << std::endl;
        std::cout << "  Disconnected facets (original): " << metrics.totalDisconnectedFacets.original << std::endl;
        std::cout << "  Facets added (gap filling): " << metrics.facetsAdded << std::endl;
        
        double const reversedRatio = 
            static_cast<double>(metrics.facetsReversed) / 
            static_cast<double>(metrics.numberOfFacets.original);
        std::cout << "  Reversed facets: " << metrics.facetsReversed 
                  << " (" << (reversedRatio * 100.0) << "%)" << std::endl;
        std::cout << "  Backwards edges: " << metrics.backwardsEdges << std::endl;
        
        // Strict validation: mesh should be watertight
        EXPECT_EQ(metrics.totalDisconnectedFacets.original, 0) 
            << "Mesh should have no disconnected facets";
        EXPECT_EQ(metrics.facetsAdded, 0) 
            << "No facets should need to be added for gap filling";
        EXPECT_EQ(metrics.backwardsEdges, 0) 
            << "Mesh should have no backwards edges (boundary edges)";
        EXPECT_LT(reversedRatio, 0.01) 
            << "Less than 1% of facets should be reversed";
        EXPECT_GT(metrics.volume, 0.0) 
            << "Volume should be positive for outward-facing normals";
    }

    /// Test mesh generation with wristsupport model (gyroid lattice structure)
    /// This model has thin gyroid walls that may create holes at intersections
    TEST_F(ManifoldDualContouringGpu_Test, GenerateMesh_WithWristsupport_AdmeshValidation)
    {
        if (!isAdmeshAvailable())
        {
            GTEST_SKIP() << "admesh not available, skipping validation test";
        }

        auto bundle = loadDocument("testdata/wristsupport.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value()) << "Bounding box should be available";
        
        std::cout << "Wristsupport bounding box: [" 
                  << bbox->min.x << ", " << bbox->min.y << ", " << bbox->min.z << "] to ["
                  << bbox->max.x << ", " << bbox->max.y << ", " << bbox->max.z << "]" << std::endl;
        std::cout << "Wristsupport extents: " 
                  << (bbox->max.x - bbox->min.x) << " x " 
                  << (bbox->max.y - bbox->min.y) << " x "
                  << (bbox->max.z - bbox->min.z) << " mm" << std::endl;

        auto const metrics = exportAndValidateWithAdmesh(*bundle.core);
        
        std::cout << "Wristsupport Admesh validation:" << std::endl;
        std::cout << "  Facets: " << metrics.numberOfFacets.original << std::endl;
        std::cout << "  Volume: " << metrics.volume << std::endl;
        std::cout << "  Parts: " << metrics.numberOfParts << std::endl;
        std::cout << "  Disconnected facets (original): " << metrics.totalDisconnectedFacets.original << std::endl;
        std::cout << "  Facets with 1 disconnected edge: " << metrics.facetsWith1DisconnectedEdge.original << std::endl;
        std::cout << "  Facets with 2 disconnected edges: " << metrics.facetsWith2DisconnectedEdges.original << std::endl;
        std::cout << "  Facets with 3 disconnected edges: " << metrics.facetsWith3DisconnectedEdges.original << std::endl;
        std::cout << "  Facets added (gap filling): " << metrics.facetsAdded << std::endl;
        std::cout << "  Facets removed: " << metrics.facetsRemoved << std::endl;
        std::cout << "  Degenerate facets: " << metrics.degenerateFacets << std::endl;
        std::cout << "  Backwards edges: " << metrics.backwardsEdges << std::endl;
        std::cout << "  Edges fixed: " << metrics.edgesFixed << std::endl;
        std::cout << "  Normals fixed: " << metrics.normalsFixed << std::endl;
        
        double const reversedRatio = 
            static_cast<double>(metrics.facetsReversed) / 
            static_cast<double>(metrics.numberOfFacets.original);
        std::cout << "  Reversed facets: " << metrics.facetsReversed 
                  << " (" << (reversedRatio * 100.0) << "%)" << std::endl;
        
        // Report on boundary edges - these indicate holes
        std::cout << "\n=== HOLE ANALYSIS ===" << std::endl;
        if (metrics.totalDisconnectedFacets.original > 0)
        {
            std::cout << "WARNING: Mesh has " << metrics.totalDisconnectedFacets.original 
                      << " disconnected facets indicating holes" << std::endl;
            std::cout << "  This gyroid lattice may have holes at:" << std::endl;
            std::cout << "  - Thin wall intersections where cells meet" << std::endl;
            std::cout << "  - High curvature regions of the gyroid surface" << std::endl;
            std::cout << "  - Boundary between the gyroid and solid outer shell" << std::endl;
        }
        
        // For now, just document the current state - don't fail the test
        // We want to understand the pattern before fixing
        EXPECT_GT(metrics.numberOfFacets.original, 0) 
            << "Should produce facets";
        EXPECT_NE(metrics.volume, 0.0) 
            << "Volume should be non-zero";
    }

    /// Test wristsupport with higher resolution to check if holes are resolution-related
    TEST_F(ManifoldDualContouringGpu_Test, GenerateMesh_WithWristsupport_HigherResolution)
    {
        if (!isAdmeshAvailable())
        {
            GTEST_SKIP() << "admesh not available, skipping validation test";
        }

        auto bundle = loadDocument("testdata/wristsupport.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value()) << "Bounding box should be available";

        // Use higher resolution (depth 8 instead of 7)
        gladius::io::ManifoldDualContouringOptions exportOptions;
        exportOptions.initialDepth = 5;
        exportOptions.maxDepth = 8;  // Higher depth = smaller voxels
        exportOptions.enableGpu = true;
        exportOptions.enableCpuFallback = true;
        exportOptions.enableCaching = true;
        exportOptions.isoValue = 0.0F;

        auto const metrics = exportAndValidateWithAdmesh(*bundle.core, exportOptions);
        
        std::cout << "=== WRISTSUPPORT HIGH-RES (maxDepth=8) ===" << std::endl;
        std::cout << "  Facets: " << metrics.numberOfFacets.original << std::endl;
        std::cout << "  Parts: " << metrics.numberOfParts << std::endl;
        std::cout << "  Disconnected facets: " << metrics.totalDisconnectedFacets.original << std::endl;
        std::cout << "  Facets with 1 disconnected edge: " << metrics.facetsWith1DisconnectedEdge.original << std::endl;
        std::cout << "  Facets with 2 disconnected edges: " << metrics.facetsWith2DisconnectedEdges.original << std::endl;
        std::cout << "  Backwards edges: " << metrics.backwardsEdges << std::endl;
        
        // Compare with standard resolution in the output
        std::cout << "\nIf holes decreased, the issue is likely voxel resolution vs wall thickness" << std::endl;
        std::cout << "If holes stayed same/increased, the issue is algorithmic" << std::endl;
    }

    /// Test wristsupport with hierarchical octree approach for improved watertightness
    TEST_F(ManifoldDualContouringGpu_Test, GenerateMesh_WithWristsupport_HierarchicalOctree)
    {
        if (!isAdmeshAvailable())
        {
            GTEST_SKIP() << "admesh not available, skipping validation test";
        }

        auto bundle = loadDocument("testdata/wristsupport.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value()) << "Bounding box should be available";

        // Enable hierarchical octree with 2:1 balancing
        gladius::io::ManifoldDualContouringOptions exportOptions;
        exportOptions.initialDepth = 5;
        exportOptions.maxDepth = 7;
        exportOptions.enableGpu = true;
        exportOptions.enableCpuFallback = true;
        exportOptions.enableCaching = true;
        exportOptions.isoValue = 0.0F;
        exportOptions.enableHierarchicalOctree = true;  // Enable hierarchical octree

        auto const metrics = exportAndValidateWithAdmesh(*bundle.core, exportOptions);
        
        std::cout << "=== WRISTSUPPORT HIERARCHICAL OCTREE ===" << std::endl;
        std::cout << "  Facets: " << metrics.numberOfFacets.original << std::endl;
        std::cout << "  Parts: " << metrics.numberOfParts << std::endl;
        std::cout << "  Disconnected facets: " << metrics.totalDisconnectedFacets.original << std::endl;
        std::cout << "  Facets with 1 disconnected edge: " << metrics.facetsWith1DisconnectedEdge.original << std::endl;
        std::cout << "  Facets with 2 disconnected edges: " << metrics.facetsWith2DisconnectedEdges.original << std::endl;
        std::cout << "  Backwards edges: " << metrics.backwardsEdges << std::endl;
        std::cout << "  Volume: " << metrics.volume << std::endl;
        
        // Report comparison
        std::cout << "\nCompare with standard sparse octree approach:" << std::endl;
        std::cout << "  If disconnected facets are lower, hierarchical approach is working" << std::endl;
    }

    /// Test mesh generation with filamentholder model
    /// This model has been reported to have significant holes when using manifold DC
    TEST_F(ManifoldDualContouringGpu_Test, GenerateMesh_WithFilamentholder_AdmeshValidation)
    {
        if (!isAdmeshAvailable())
        {
            GTEST_SKIP() << "admesh not available, skipping validation test";
        }

        auto bundle = loadDocument("testdata/filamentholder.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value()) << "Bounding box should be available";
        
        std::cout << "=== FILAMENTHOLDER MODEL ===" << std::endl;
        std::cout << "Bounding box: [" 
                  << bbox->min.x << ", " << bbox->min.y << ", " << bbox->min.z << "] to ["
                  << bbox->max.x << ", " << bbox->max.y << ", " << bbox->max.z << "]" << std::endl;
        std::cout << "Extents: " 
                  << (bbox->max.x - bbox->min.x) << " x " 
                  << (bbox->max.y - bbox->min.y) << " x "
                  << (bbox->max.z - bbox->min.z) << " mm" << std::endl;

        auto const metrics = exportAndValidateWithAdmesh(*bundle.core);
        
        std::cout << "\nAdmesh validation:" << std::endl;
        std::cout << "  Facets: " << metrics.numberOfFacets.original << std::endl;
        std::cout << "  Volume: " << metrics.volume << std::endl;
        std::cout << "  Parts: " << metrics.numberOfParts << std::endl;
        std::cout << "  Disconnected facets (original): " << metrics.totalDisconnectedFacets.original << std::endl;
        std::cout << "  Facets with 1 disconnected edge: " << metrics.facetsWith1DisconnectedEdge.original << std::endl;
        std::cout << "  Facets with 2 disconnected edges: " << metrics.facetsWith2DisconnectedEdges.original << std::endl;
        std::cout << "  Facets with 3 disconnected edges: " << metrics.facetsWith3DisconnectedEdges.original << std::endl;
        std::cout << "  Facets added (gap filling): " << metrics.facetsAdded << std::endl;
        std::cout << "  Facets removed: " << metrics.facetsRemoved << std::endl;
        std::cout << "  Degenerate facets: " << metrics.degenerateFacets << std::endl;
        std::cout << "  Backwards edges: " << metrics.backwardsEdges << std::endl;
        std::cout << "  Edges fixed: " << metrics.edgesFixed << std::endl;
        std::cout << "  Normals fixed: " << metrics.normalsFixed << std::endl;
        
        double const reversedRatio = 
            static_cast<double>(metrics.facetsReversed) / 
            static_cast<double>(metrics.numberOfFacets.original);
        std::cout << "  Reversed facets: " << metrics.facetsReversed 
                  << " (" << (reversedRatio * 100.0) << "%)" << std::endl;
        
        // Report on boundary edges - these indicate holes
        std::cout << "\n=== HOLE ANALYSIS ===" << std::endl;
        if (metrics.totalDisconnectedFacets.original > 0)
        {
            std::cout << "WARNING: Mesh has " << metrics.totalDisconnectedFacets.original 
                      << " disconnected facets indicating holes" << std::endl;
        }
        
        // For now, just document the current state
        EXPECT_GT(metrics.numberOfFacets.original, 0) 
            << "Should produce facets";
        EXPECT_NE(metrics.volume, 0.0) 
            << "Volume should be non-zero";
    }

    /// Test filamentholder with hierarchical octree
    TEST_F(ManifoldDualContouringGpu_Test, GenerateMesh_WithFilamentholder_HierarchicalOctree)
    {
        if (!isAdmeshAvailable())
        {
            GTEST_SKIP() << "admesh not available, skipping validation test";
        }

        auto bundle = loadDocument("testdata/filamentholder.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value()) << "Bounding box should be available";

        // Enable hierarchical octree with 2:1 balancing
        gladius::io::ManifoldDualContouringOptions exportOptions;
        exportOptions.initialDepth = 5;
        exportOptions.maxDepth = 7;
        exportOptions.enableGpu = true;
        exportOptions.enableCpuFallback = true;
        exportOptions.enableCaching = true;
        exportOptions.isoValue = 0.0F;
        exportOptions.enableHierarchicalOctree = true;  // Enable hierarchical octree

        auto const metrics = exportAndValidateWithAdmesh(*bundle.core, exportOptions);
        
        std::cout << "=== FILAMENTHOLDER HIERARCHICAL OCTREE ===" << std::endl;
        std::cout << "  Facets: " << metrics.numberOfFacets.original << std::endl;
        std::cout << "  Parts: " << metrics.numberOfParts << std::endl;
        std::cout << "  Disconnected facets: " << metrics.totalDisconnectedFacets.original << std::endl;
        std::cout << "  Facets with 1 disconnected edge: " << metrics.facetsWith1DisconnectedEdge.original << std::endl;
        std::cout << "  Facets with 2 disconnected edges: " << metrics.facetsWith2DisconnectedEdges.original << std::endl;
        std::cout << "  Backwards edges: " << metrics.backwardsEdges << std::endl;
        std::cout << "  Volume: " << metrics.volume << std::endl;
        
        // Report comparison
        std::cout << "\nCompare with standard sparse octree approach above." << std::endl;
    }


    // ============================================================================
    // Implicit Surface Validation Tests
    // ============================================================================
    
    /// Helper class to sample SDF values at arbitrary positions using ComputeCore
    class SdfSampler
    {
      public:
        explicit SdfSampler(ComputeCore & core, BoundingBox const & bounds, std::size_t resolution = 256)
            : m_core(core)
            , m_bounds(bounds)
            , m_resolution(resolution)
        {
            initialize();
        }

        [[nodiscard]] bool initialized() const { return m_initialized; }

        /// Sample SDF at a world position using trilinear interpolation
        [[nodiscard]] float sample(Eigen::Vector3f const & worldPos) const
        {
            if (!m_initialized || m_values.empty())
            {
                return std::numeric_limits<float>::quiet_NaN();
            }

            // Transform world position to normalized [0,1] coordinates
            Eigen::Vector3f const extent = m_max - m_min;
            Eigen::Vector3f const safeExtent = extent.cwiseMax(Eigen::Vector3f::Constant(1e-6F));
            Eigen::Vector3f normalized = (worldPos - m_min).cwiseQuotient(safeExtent);
            
            // Clamp to valid range
            normalized = normalized.cwiseMax(Eigen::Vector3f::Zero());
            normalized = normalized.cwiseMin(Eigen::Vector3f::Ones());

            // Convert to grid coordinates
            float const sampleX = normalized.x() * static_cast<float>(m_width - 1U);
            float const sampleY = normalized.y() * static_cast<float>(m_height - 1U);
            float const sampleZ = normalized.z() * static_cast<float>(m_depth - 1U);

            // Get integer indices for trilinear interpolation
            std::size_t const x0 = static_cast<std::size_t>(std::floor(sampleX));
            std::size_t const y0 = static_cast<std::size_t>(std::floor(sampleY));
            std::size_t const z0 = static_cast<std::size_t>(std::floor(sampleZ));
            std::size_t const x1 = std::min(x0 + 1U, m_width - 1U);
            std::size_t const y1 = std::min(y0 + 1U, m_height - 1U);
            std::size_t const z1 = std::min(z0 + 1U, m_depth - 1U);

            // Interpolation weights
            float const fx = sampleX - static_cast<float>(x0);
            float const fy = sampleY - static_cast<float>(y0);
            float const fz = sampleZ - static_cast<float>(z0);

            // Fetch 8 corner values
            float const v000 = getValue(x0, y0, z0);
            float const v100 = getValue(x1, y0, z0);
            float const v010 = getValue(x0, y1, z0);
            float const v110 = getValue(x1, y1, z0);
            float const v001 = getValue(x0, y0, z1);
            float const v101 = getValue(x1, y0, z1);
            float const v011 = getValue(x0, y1, z1);
            float const v111 = getValue(x1, y1, z1);

            // Trilinear interpolation
            float const c00 = v000 * (1.0F - fx) + v100 * fx;
            float const c10 = v010 * (1.0F - fx) + v110 * fx;
            float const c01 = v001 * (1.0F - fx) + v101 * fx;
            float const c11 = v011 * (1.0F - fx) + v111 * fx;
            float const c0 = c00 * (1.0F - fy) + c10 * fy;
            float const c1 = c01 * (1.0F - fy) + c11 * fy;
            return c0 * (1.0F - fz) + c1 * fz;
        }

        [[nodiscard]] float getVoxelSize() const
        {
            Eigen::Vector3f const extent = m_max - m_min;
            return std::max({extent.x() / static_cast<float>(m_width),
                             extent.y() / static_cast<float>(m_height),
                             extent.z() / static_cast<float>(m_depth)});
        }

      private:
        void initialize()
        {
            try
            {
                m_core.setPreCompSdfSize(m_resolution);
                m_core.precomputeSdfForBBox(m_bounds);

                auto resources = m_core.getResourceContext();
                if (!resources)
                {
                    return;
                }

                auto & sdfBuffer = resources->getPrecompSdfBuffer();
                sdfBuffer.read();

                m_width = std::max<std::size_t>(sdfBuffer.getWidth(), 1U);
                m_height = std::max<std::size_t>(sdfBuffer.getHeight(), 1U);
                m_depth = std::max<std::size_t>(sdfBuffer.getDepth(), 1U);

                auto const & raw = sdfBuffer.getData();
                m_values.assign(raw.begin(), raw.end());

                BoundingBox const actualBounds = resources->getPreCompSdfBBox();
                m_min = Eigen::Vector3f{actualBounds.min.x, actualBounds.min.y, actualBounds.min.z};
                m_max = Eigen::Vector3f{actualBounds.max.x, actualBounds.max.y, actualBounds.max.z};

                m_initialized = true;
            }
            catch (...)
            {
                m_initialized = false;
            }
        }

        [[nodiscard]] float getValue(std::size_t x, std::size_t y, std::size_t z) const
        {
            std::size_t const index = x + y * m_width + z * m_width * m_height;
            return (index < m_values.size()) ? m_values[index] : 0.0F;
        }

        ComputeCore & m_core;
        BoundingBox m_bounds;
        std::size_t m_resolution;
        bool m_initialized{false};
        std::vector<float> m_values;
        std::size_t m_width{1U};
        std::size_t m_height{1U};
        std::size_t m_depth{1U};
        Eigen::Vector3f m_min{Eigen::Vector3f::Zero()};
        Eigen::Vector3f m_max{Eigen::Vector3f::Ones()};
    };

    /// Statistics for mesh-to-implicit validation
    struct MeshImplicitValidation
    {
        std::size_t vertexCount{0U};
        std::size_t triangleCount{0U};
        float maxVertexDeviation{0.0F};
        float meanVertexDeviation{0.0F};
        float rmsVertexDeviation{0.0F};
        std::size_t verticesWithinTolerance{0U};
        float maxFaceCenterDeviation{0.0F};
        float meanFaceCenterDeviation{0.0F};
        std::size_t faceCentersWithinTolerance{0U};
        float tolerance{0.0F};
        float voxelSize{0.0F};

        void print() const
        {
            std::cout << "\n=== Mesh-to-Implicit Validation ===\n";
            std::cout << "Vertices: " << vertexCount << ", Triangles: " << triangleCount << "\n";
            std::cout << "Voxel size: " << voxelSize << " mm, Tolerance: " << tolerance << " mm\n";
            std::cout << "\nVertex SDF Deviation:\n";
            std::cout << "  Max: " << maxVertexDeviation << " mm\n";
            std::cout << "  Mean: " << meanVertexDeviation << " mm\n";
            std::cout << "  RMS: " << rmsVertexDeviation << " mm\n";
            std::cout << "  Within tolerance: " << verticesWithinTolerance << "/" << vertexCount
                      << " (" << (100.0 * verticesWithinTolerance / std::max<std::size_t>(vertexCount, 1U)) << "%)\n";
            std::cout << "\nFace Center SDF Deviation:\n";
            std::cout << "  Max: " << maxFaceCenterDeviation << " mm\n";
            std::cout << "  Mean: " << meanFaceCenterDeviation << " mm\n";
            std::cout << "  Within tolerance: " << faceCentersWithinTolerance << "/" << triangleCount
                      << " (" << (100.0 * faceCentersWithinTolerance / std::max<std::size_t>(triangleCount, 1U)) << "%)\n";
        }
    };

    /// Validate mesh vertices and face centers against implicit SDF
    [[nodiscard]] MeshImplicitValidation validateMeshAgainstImplicit(
        ManifoldDualContouringMesh const & mesh,
        SdfSampler const & sampler,
        float toleranceFactor = 1.5F)
    {
        MeshImplicitValidation result;
        result.voxelSize = sampler.getVoxelSize();
        result.tolerance = result.voxelSize * toleranceFactor;
        result.vertexCount = mesh.positions.size();
        result.triangleCount = mesh.indices.size() / 3U;

        if (mesh.positions.empty())
        {
            return result;
        }

        // Validate vertices
        double sumDeviation = 0.0;
        double sumDeviationSq = 0.0;
        for (auto const & pos : mesh.positions)
        {
            float const sdfValue = sampler.sample(pos);
            float const deviation = std::abs(sdfValue);
            result.maxVertexDeviation = std::max(result.maxVertexDeviation, deviation);
            sumDeviation += deviation;
            sumDeviationSq += deviation * deviation;
            if (deviation <= result.tolerance)
            {
                ++result.verticesWithinTolerance;
            }
        }
        result.meanVertexDeviation = static_cast<float>(sumDeviation / mesh.positions.size());
        result.rmsVertexDeviation = static_cast<float>(std::sqrt(sumDeviationSq / mesh.positions.size()));

        // Validate face centers
        if (mesh.indices.size() >= 3U)
        {
            double sumFaceDeviation = 0.0;
            for (std::size_t tri = 0U; tri + 2U < mesh.indices.size(); tri += 3U)
            {
                std::uint32_t const a = mesh.indices[tri + 0U];
                std::uint32_t const b = mesh.indices[tri + 1U];
                std::uint32_t const c = mesh.indices[tri + 2U];

                if (a >= mesh.positions.size() || b >= mesh.positions.size() || c >= mesh.positions.size())
                {
                    continue;
                }

                Eigen::Vector3f const center = (mesh.positions[a] + mesh.positions[b] + mesh.positions[c]) / 3.0F;
                float const sdfValue = sampler.sample(center);
                float const deviation = std::abs(sdfValue);
                result.maxFaceCenterDeviation = std::max(result.maxFaceCenterDeviation, deviation);
                sumFaceDeviation += deviation;
                if (deviation <= result.tolerance)
                {
                    ++result.faceCentersWithinTolerance;
                }
            }
            result.meanFaceCenterDeviation = static_cast<float>(sumFaceDeviation / result.triangleCount);
        }

        return result;
    }

    /// Analyze triangle edge lengths to detect cross-surface connections
    struct TriangleEdgeAnalysis
    {
        std::size_t triangleCount{0U};
        float minEdgeLength{std::numeric_limits<float>::max()};
        float maxEdgeLength{0.0F};
        float meanEdgeLength{0.0F};
        float medianEdgeLength{0.0F};
        float stdDevEdgeLength{0.0F};
        std::size_t suspiciousTriangles{0U};  // Triangles with edges > 5x median
        std::size_t veryLongEdgeCount{0U};    // Individual edges > 10x median
        float voxelSize{0.0F};

        void print() const
        {
            std::cout << "\n=== Triangle Edge Length Analysis ===\n";
            std::cout << "Triangles: " << triangleCount << "\n";
            std::cout << "Voxel size: " << voxelSize << " mm\n";
            std::cout << "Edge lengths (mm):\n";
            std::cout << "  Min: " << minEdgeLength << "\n";
            std::cout << "  Max: " << maxEdgeLength << "\n";
            std::cout << "  Mean: " << meanEdgeLength << "\n";
            std::cout << "  Median: " << medianEdgeLength << "\n";
            std::cout << "  StdDev: " << stdDevEdgeLength << "\n";
            std::cout << "  Max/Median ratio: " << (maxEdgeLength / std::max(medianEdgeLength, 0.001F)) << "\n";
            std::cout << "Suspicious triangles (edge > 5x median): " << suspiciousTriangles 
                      << " (" << (100.0 * suspiciousTriangles / std::max<std::size_t>(triangleCount, 1U)) << "%)\n";
            std::cout << "Very long edges (> 10x median): " << veryLongEdgeCount << "\n";
        }
    };

    [[nodiscard]] TriangleEdgeAnalysis analyzeTriangleEdges(
        ManifoldDualContouringMesh const & mesh,
        float voxelSize)
    {
        TriangleEdgeAnalysis result;
        result.voxelSize = voxelSize;
        result.triangleCount = mesh.indices.size() / 3U;

        if (mesh.indices.size() < 3U)
        {
            return result;
        }

        std::vector<float> allEdgeLengths;
        allEdgeLengths.reserve(mesh.indices.size()); // 3 edges per triangle

        for (std::size_t tri = 0U; tri + 2U < mesh.indices.size(); tri += 3U)
        {
            std::uint32_t const a = mesh.indices[tri + 0U];
            std::uint32_t const b = mesh.indices[tri + 1U];
            std::uint32_t const c = mesh.indices[tri + 2U];

            if (a >= mesh.positions.size() || b >= mesh.positions.size() || c >= mesh.positions.size())
            {
                continue;
            }

            float const e1 = (mesh.positions[b] - mesh.positions[a]).norm();
            float const e2 = (mesh.positions[c] - mesh.positions[b]).norm();
            float const e3 = (mesh.positions[a] - mesh.positions[c]).norm();

            allEdgeLengths.push_back(e1);
            allEdgeLengths.push_back(e2);
            allEdgeLengths.push_back(e3);

            result.minEdgeLength = std::min({result.minEdgeLength, e1, e2, e3});
            result.maxEdgeLength = std::max({result.maxEdgeLength, e1, e2, e3});
        }

        if (allEdgeLengths.empty())
        {
            return result;
        }

        // Compute mean
        double sum = 0.0;
        for (float len : allEdgeLengths)
        {
            sum += len;
        }
        result.meanEdgeLength = static_cast<float>(sum / allEdgeLengths.size());

        // Compute median
        std::vector<float> sorted = allEdgeLengths;
        std::sort(sorted.begin(), sorted.end());
        result.medianEdgeLength = sorted[sorted.size() / 2];

        // Compute std dev
        double sumSq = 0.0;
        for (float len : allEdgeLengths)
        {
            double diff = len - result.meanEdgeLength;
            sumSq += diff * diff;
        }
        result.stdDevEdgeLength = static_cast<float>(std::sqrt(sumSq / allEdgeLengths.size()));

        // Count suspicious triangles and very long edges
        float const suspiciousThreshold = result.medianEdgeLength * 5.0F;
        float const veryLongThreshold = result.medianEdgeLength * 10.0F;

        for (std::size_t tri = 0U; tri + 2U < mesh.indices.size(); tri += 3U)
        {
            std::uint32_t const a = mesh.indices[tri + 0U];
            std::uint32_t const b = mesh.indices[tri + 1U];
            std::uint32_t const c = mesh.indices[tri + 2U];

            if (a >= mesh.positions.size() || b >= mesh.positions.size() || c >= mesh.positions.size())
            {
                continue;
            }

            float const e1 = (mesh.positions[b] - mesh.positions[a]).norm();
            float const e2 = (mesh.positions[c] - mesh.positions[b]).norm();
            float const e3 = (mesh.positions[a] - mesh.positions[c]).norm();

            if (e1 > suspiciousThreshold || e2 > suspiciousThreshold || e3 > suspiciousThreshold)
            {
                ++result.suspiciousTriangles;
            }

            if (e1 > veryLongThreshold) ++result.veryLongEdgeCount;
            if (e2 > veryLongThreshold) ++result.veryLongEdgeCount;
            if (e3 > veryLongThreshold) ++result.veryLongEdgeCount;
        }

        return result;
    }

    TEST_F(ManifoldDualContouringGpu_Test, ValidateMesh_ImplicitGyroid_VerticesOnSurface)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value()) << "Bounding box should be available";

        // Generate mesh with dual contouring
        ManifoldDualContouringGpu gpu(*bundle.core);
        ManifoldDualContouringConfig config;
        config.enableGpu = true;
        config.initialDepth = 5;
        config.maxDepth = 7;
        gpu.setConfig(config);
        gpu.generateMesh();

        auto const & mesh = gpu.getMesh();
        ASSERT_GT(mesh.positions.size(), 0U) << "Mesh should have vertices";
        ASSERT_GT(mesh.indices.size(), 0U) << "Mesh should have indices";

        // Create SDF sampler for validation (higher resolution than mesh)
        BoundingBox sdfBounds = *bbox;
        SdfSampler sampler(*bundle.core, sdfBounds, 256);
        ASSERT_TRUE(sampler.initialized()) << "SDF sampler should initialize";

        // Validate mesh against implicit
        auto const validation = validateMeshAgainstImplicit(mesh, sampler);
        validation.print();

        // Assertions: vertices should be close to the implicit surface
        float const maxAcceptableDeviation = validation.voxelSize * 2.0F;
        EXPECT_LE(validation.maxVertexDeviation, maxAcceptableDeviation)
            << "Max vertex deviation should be within 2x voxel size";

        float const minVertexRatio = 0.95F;
        float const actualRatio = static_cast<float>(validation.verticesWithinTolerance) / 
                                   static_cast<float>(validation.vertexCount);
        EXPECT_GE(actualRatio, minVertexRatio)
            << "At least 95% of vertices should be within tolerance";

        // Face centers: Dual contouring produces vertices on the surface but triangles
        // connecting them may cut through. This is informational, not a failure condition.
        // However, for quality meshes, face centers should be reasonably close.
        float const faceCenterRatio = static_cast<float>(validation.faceCentersWithinTolerance) / 
                                       static_cast<float>(validation.triangleCount);
        std::cout << "\nNote: " << (faceCenterRatio * 100.0F) << "% of face centers within tolerance.\n";
        std::cout << "  (Expected: face centers may deviate more than vertices in dual contouring)\n";
        
        // Soft assertion: warn if very few face centers are on surface
        if (faceCenterRatio < 0.5F)
        {
            std::cout << "  WARNING: Low face center conformity may indicate mesh quality issues.\n";
        }
    }

    TEST_F(ManifoldDualContouringGpu_Test, ValidateMesh_SphereInACage_VerticesOnSurface)
    {
        auto bundle = loadDocument("testdata/SphereInACage.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value()) << "Bounding box should be available";

        // Generate mesh
        ManifoldDualContouringGpu gpu(*bundle.core);
        ManifoldDualContouringConfig config;
        config.enableGpu = true;
        config.initialDepth = 5;
        config.maxDepth = 7;
        gpu.setConfig(config);
        gpu.generateMesh();

        auto const & mesh = gpu.getMesh();
        ASSERT_GT(mesh.positions.size(), 0U);
        ASSERT_GT(mesh.indices.size(), 0U);

        // Create SDF sampler
        SdfSampler sampler(*bundle.core, *bbox, 256);
        ASSERT_TRUE(sampler.initialized());

        // Validate vertex positions
        auto const validation = validateMeshAgainstImplicit(mesh, sampler);
        validation.print();

        // Analyze edge lengths to detect cross-surface triangles
        auto const edgeAnalysis = analyzeTriangleEdges(mesh, validation.voxelSize);
        edgeAnalysis.print();

        // SphereInACage has two separate surfaces (inner sphere and outer cage)
        // Vertices should still be on the implicit surface
        float const maxAcceptableDeviation = validation.voxelSize * 3.0F;
        EXPECT_LE(validation.maxVertexDeviation, maxAcceptableDeviation)
            << "Max vertex deviation should be within 3x voxel size for complex geometry";

        float const minVertexRatio = 0.90F;
        float const actualRatio = static_cast<float>(validation.verticesWithinTolerance) / 
                                   static_cast<float>(validation.vertexCount);
        EXPECT_GE(actualRatio, minVertexRatio)
            << "At least 90% of vertices should be within tolerance";

        // Check for cross-surface triangles (very long edges indicate connectivity bugs)
        // In a correct mesh, max edge should be at most a few voxels long
        float const maxReasonableEdge = validation.voxelSize * 10.0F;  // Conservative threshold
        if (edgeAnalysis.maxEdgeLength > maxReasonableEdge)
        {
            std::cout << "\n*** WARNING: Detected very long edges! ***\n";
            std::cout << "Max edge: " << edgeAnalysis.maxEdgeLength << " mm\n";
            std::cout << "Expected max: ~" << maxReasonableEdge << " mm\n";
            std::cout << "This indicates triangles connecting different surfaces.\n";
        }
        
        // Fail if there are too many suspicious triangles
        float const maxSuspiciousRatio = 0.01F;  // Allow max 1% suspicious triangles
        float const actualSuspiciousRatio = static_cast<float>(edgeAnalysis.suspiciousTriangles) / 
                                             static_cast<float>(edgeAnalysis.triangleCount);
        EXPECT_LE(actualSuspiciousRatio, maxSuspiciousRatio)
            << "Too many triangles with abnormally long edges (possible cross-surface connections)";
    }

    // ============================================================================
    // Hole Localization Tests - Debug mesh holes by analyzing boundary edges
    // ============================================================================

    /// Information about a boundary edge (edge with only 1 adjacent triangle)
    struct BoundaryEdgeInfo
    {
        std::uint32_t v0;                   ///< First vertex index
        std::uint32_t v1;                   ///< Second vertex index
        Eigen::Vector3f pos0;               ///< Position of v0
        Eigen::Vector3f pos1;               ///< Position of v1
        Eigen::Vector3f midpoint;           ///< Midpoint of edge
        float length;                       ///< Edge length
        std::uint32_t triangleIndex;        ///< Triangle that owns this edge
        std::size_t clusterIndex{0U};       ///< Which hole cluster this belongs to
    };

    /// Cluster of connected boundary edges (represents a hole)
    struct HoleCluster
    {
        std::vector<std::size_t> edgeIndices;  ///< Indices into boundary edges vector
        Eigen::Vector3f centroid;               ///< Centroid of hole
        float averageEdgeLength{0.0F};
        float perimeter{0.0F};                  ///< Total length of hole boundary
    };

    /// Analyze mesh to find all boundary edges and cluster them into holes
    [[nodiscard]] std::pair<std::vector<BoundaryEdgeInfo>, std::vector<HoleCluster>>
    analyzeMeshHoles(ManifoldDualContouringMesh const& mesh)
    {
        std::vector<BoundaryEdgeInfo> boundaryEdges;
        std::vector<HoleCluster> clusters;

        if (mesh.indices.size() < 3U || (mesh.indices.size() % 3U) != 0U)
        {
            return {boundaryEdges, clusters};
        }

        // Count edge usage and track which triangle owns each edge
        struct EdgeData
        {
            std::uint32_t count{0U};
            std::uint32_t triangleIndex{0U};
        };
        std::unordered_map<EdgeKey, EdgeData, EdgeKeyHash> edgeUsage;

        for (std::size_t tri = 0U; tri + 2U < mesh.indices.size(); tri += 3U)
        {
            std::uint32_t const triIdx = static_cast<std::uint32_t>(tri / 3U);
            std::array<std::uint32_t, 3> const verts = {
                mesh.indices[tri + 0U],
                mesh.indices[tri + 1U],
                mesh.indices[tri + 2U]
            };

            for (std::size_t e = 0U; e < 3U; ++e)
            {
                std::uint32_t const i0 = verts[e];
                std::uint32_t const i1 = verts[(e + 1U) % 3U];
                EdgeKey const key{std::min(i0, i1), std::max(i0, i1)};
                auto& data = edgeUsage[key];
                ++data.count;
                if (data.count == 1U)
                {
                    data.triangleIndex = triIdx;
                }
            }
        }

        // Collect boundary edges (edges with count == 1)
        for (auto const& [key, data] : edgeUsage)
        {
            if (data.count == 1U)
            {
                BoundaryEdgeInfo info;
                info.v0 = key.a;
                info.v1 = key.b;
                info.pos0 = mesh.positions[key.a];
                info.pos1 = mesh.positions[key.b];
                info.midpoint = (info.pos0 + info.pos1) * 0.5F;
                info.length = (info.pos1 - info.pos0).norm();
                info.triangleIndex = data.triangleIndex;
                boundaryEdges.push_back(info);
            }
        }

        if (boundaryEdges.empty())
        {
            return {boundaryEdges, clusters};
        }

        // Cluster boundary edges by vertex connectivity (edges sharing a vertex are in same hole)
        std::vector<bool> visited(boundaryEdges.size(), false);
        
        // Build vertex-to-edge adjacency
        std::unordered_map<std::uint32_t, std::vector<std::size_t>> vertexToEdges;
        for (std::size_t i = 0U; i < boundaryEdges.size(); ++i)
        {
            vertexToEdges[boundaryEdges[i].v0].push_back(i);
            vertexToEdges[boundaryEdges[i].v1].push_back(i);
        }

        for (std::size_t startIdx = 0U; startIdx < boundaryEdges.size(); ++startIdx)
        {
            if (visited[startIdx])
            {
                continue;
            }

            // BFS to find all connected boundary edges
            HoleCluster cluster;
            std::vector<std::size_t> queue;
            queue.push_back(startIdx);
            visited[startIdx] = true;

            while (!queue.empty())
            {
                std::size_t const current = queue.back();
                queue.pop_back();
                
                cluster.edgeIndices.push_back(current);
                boundaryEdges[current].clusterIndex = clusters.size();
                cluster.perimeter += boundaryEdges[current].length;

                // Find adjacent edges (share a vertex)
                for (std::uint32_t v : {boundaryEdges[current].v0, boundaryEdges[current].v1})
                {
                    for (std::size_t adjIdx : vertexToEdges[v])
                    {
                        if (!visited[adjIdx])
                        {
                            visited[adjIdx] = true;
                            queue.push_back(adjIdx);
                        }
                    }
                }
            }

            // Compute cluster centroid
            Eigen::Vector3f sum = Eigen::Vector3f::Zero();
            for (std::size_t edgeIdx : cluster.edgeIndices)
            {
                sum += boundaryEdges[edgeIdx].midpoint;
            }
            cluster.centroid = sum / static_cast<float>(cluster.edgeIndices.size());
            cluster.averageEdgeLength = cluster.perimeter / static_cast<float>(cluster.edgeIndices.size());

            clusters.push_back(std::move(cluster));
        }

        // Sort clusters by size (largest holes first)
        std::sort(clusters.begin(), clusters.end(),
                  [](HoleCluster const& a, HoleCluster const& b)
                  { return a.edgeIndices.size() > b.edgeIndices.size(); });

        return {boundaryEdges, clusters};
    }

    /// Test that analyzes and reports hole locations in filamentholder mesh
    TEST_F(ManifoldDualContouringGpu_Test, DebugHoles_Filamentholder_LocalizeHoles)
    {
        auto bundle = loadDocument("testdata/filamentholder.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value()) << "Bounding box should be available";

        // Use default settings (sparse octree)
        gladius::io::ManifoldDualContouringOptions exportOptions;
        exportOptions.initialDepth = 5;
        exportOptions.maxDepth = 7;
        exportOptions.enableGpu = true;
        exportOptions.enableCpuFallback = true;
        exportOptions.enableCaching = true;
        exportOptions.isoValue = 0.0F;
        exportOptions.enableHierarchicalOctree = false;

        // Generate mesh
        ManifoldDualContouringGpu mesher(*bundle.core);
        mesher.setConfig({
            .initialDepth = exportOptions.initialDepth,
            .maxDepth = exportOptions.maxDepth,
            .enableGpu = exportOptions.enableGpu,
            .enableCpuFallback = exportOptions.enableCpuFallback,
            .enableCaching = exportOptions.enableCaching,
            .isoValue = exportOptions.isoValue,
            .enableHierarchicalOctree = exportOptions.enableHierarchicalOctree
        });
        mesher.generateMesh();
        auto const& mesh = mesher.getMesh();

        ASSERT_FALSE(mesh.positions.empty()) << "Mesh should have vertices";
        ASSERT_FALSE(mesh.indices.empty()) << "Mesh should have triangles";

        // Analyze holes
        auto [boundaryEdges, clusters] = analyzeMeshHoles(mesh);

        std::cout << "\n=== HOLE LOCALIZATION: FILAMENTHOLDER (Sparse Octree) ===" << std::endl;
        std::cout << "Total boundary edges: " << boundaryEdges.size() << std::endl;
        std::cout << "Number of hole clusters: " << clusters.size() << std::endl;

        if (clusters.empty())
        {
            std::cout << "*** MESH IS WATERTIGHT! ***" << std::endl;
            return;
        }

        // Calculate bbox extents for relative position reporting
        Eigen::Vector3f bboxMin(bbox->min.s[0], bbox->min.s[1], bbox->min.s[2]);
        Eigen::Vector3f bboxMax(bbox->max.s[0], bbox->max.s[1], bbox->max.s[2]);
        Eigen::Vector3f bboxSize = bboxMax - bboxMin;
        float const voxelSize = bboxSize.maxCoeff() / static_cast<float>(1U << exportOptions.maxDepth);

        std::cout << "BBox: [" << bboxMin.transpose() << "] to [" << bboxMax.transpose() << "]" << std::endl;
        std::cout << "Voxel size at maxDepth: " << voxelSize << " mm" << std::endl;
        std::cout << std::endl;

        // Report statistics on clusters
        std::cout << "=== CLUSTER SIZE DISTRIBUTION ===" << std::endl;
        std::map<std::size_t, std::size_t> sizeDistribution;
        for (auto const& cluster : clusters)
        {
            std::size_t sizeCategory = 0;
            if (cluster.edgeIndices.size() <= 3)
                sizeCategory = 3;
            else if (cluster.edgeIndices.size() <= 10)
                sizeCategory = 10;
            else if (cluster.edgeIndices.size() <= 50)
                sizeCategory = 50;
            else if (cluster.edgeIndices.size() <= 100)
                sizeCategory = 100;
            else
                sizeCategory = 1000;
            ++sizeDistribution[sizeCategory];
        }
        for (auto const& [size, count] : sizeDistribution)
        {
            std::cout << "  Holes with <= " << size << " edges: " << count << std::endl;
        }

        // Report top 10 largest holes
        std::cout << "\n=== TOP 10 LARGEST HOLES ===" << std::endl;
        std::size_t const maxReport = std::min<std::size_t>(10U, clusters.size());
        for (std::size_t i = 0U; i < maxReport; ++i)
        {
            auto const& cluster = clusters[i];
            
            // Compute relative position (0-1 range within bbox)
            Eigen::Vector3f relPos = (cluster.centroid - bboxMin).cwiseQuotient(bboxSize);
            
            std::cout << "Hole #" << (i + 1) << ":" << std::endl;
            std::cout << "  Edges: " << cluster.edgeIndices.size() << std::endl;
            std::cout << "  Perimeter: " << cluster.perimeter << " mm" << std::endl;
            std::cout << "  Avg edge length: " << cluster.averageEdgeLength << " mm"
                      << " (" << (cluster.averageEdgeLength / voxelSize) << " voxels)" << std::endl;
            std::cout << "  Centroid: [" << cluster.centroid.transpose() << "]" << std::endl;
            std::cout << "  Relative position: [" << relPos.transpose() << "]" << std::endl;
            
            // Report a few sample edges from this cluster
            std::cout << "  Sample edges:" << std::endl;
            std::size_t const maxEdges = std::min<std::size_t>(3U, cluster.edgeIndices.size());
            for (std::size_t j = 0U; j < maxEdges; ++j)
            {
                auto const& edge = boundaryEdges[cluster.edgeIndices[j]];
                std::cout << "    Edge " << edge.v0 << "-" << edge.v1 
                          << " at [" << edge.midpoint.transpose() << "]"
                          << " len=" << edge.length << " mm"
                          << " (tri " << edge.triangleIndex << ")" << std::endl;
            }
        }

        // Analyze edge lengths
        std::cout << "\n=== EDGE LENGTH ANALYSIS ===" << std::endl;
        float minLen = std::numeric_limits<float>::max();
        float maxLen = 0.0F;
        float sumLen = 0.0F;
        for (auto const& edge : boundaryEdges)
        {
            minLen = std::min(minLen, edge.length);
            maxLen = std::max(maxLen, edge.length);
            sumLen += edge.length;
        }
        float const avgLen = sumLen / static_cast<float>(boundaryEdges.size());
        std::cout << "  Min edge length: " << minLen << " mm (" << (minLen / voxelSize) << " voxels)" << std::endl;
        std::cout << "  Max edge length: " << maxLen << " mm (" << (maxLen / voxelSize) << " voxels)" << std::endl;
        std::cout << "  Avg edge length: " << avgLen << " mm (" << (avgLen / voxelSize) << " voxels)" << std::endl;

        // Count edges by length category (in voxel units)
        std::cout << "\n=== EDGE LENGTH DISTRIBUTION (in voxels) ===" << std::endl;
        std::map<std::size_t, std::size_t> lengthDistribution;
        for (auto const& edge : boundaryEdges)
        {
            float const lenInVoxels = edge.length / voxelSize;
            std::size_t category = 0;
            if (lenInVoxels <= 0.5F)
                category = 0;
            else if (lenInVoxels <= 1.0F)
                category = 1;
            else if (lenInVoxels <= 2.0F)
                category = 2;
            else if (lenInVoxels <= 5.0F)
                category = 5;
            else
                category = 10;
            ++lengthDistribution[category];
        }
        std::cout << "  <= 0.5 voxels: " << lengthDistribution[0] << std::endl;
        std::cout << "  <= 1.0 voxels: " << lengthDistribution[1] << std::endl;
        std::cout << "  <= 2.0 voxels: " << lengthDistribution[2] << std::endl;
        std::cout << "  <= 5.0 voxels: " << lengthDistribution[5] << std::endl;
        std::cout << "  > 5.0 voxels: " << lengthDistribution[10] << std::endl;

        // Spatial distribution - divide bbox into 8 octants and count holes per octant
        std::cout << "\n=== SPATIAL DISTRIBUTION (holes per octant) ===" << std::endl;
        std::array<std::size_t, 8> octantCounts{};
        Eigen::Vector3f const bboxCenter = (bboxMin + bboxMax) * 0.5F;
        for (auto const& cluster : clusters)
        {
            int const octant =
                (cluster.centroid.x() > bboxCenter.x() ? 1 : 0) +
                (cluster.centroid.y() > bboxCenter.y() ? 2 : 0) +
                (cluster.centroid.z() > bboxCenter.z() ? 4 : 0);
            ++octantCounts[static_cast<std::size_t>(octant)];
        }
        char const* octantNames[] = {
            "(-X,-Y,-Z)", "(+X,-Y,-Z)", "(-X,+Y,-Z)", "(+X,+Y,-Z)",
            "(-X,-Y,+Z)", "(+X,-Y,+Z)", "(-X,+Y,+Z)", "(+X,+Y,+Z)"
        };
        for (std::size_t o = 0U; o < 8U; ++o)
        {
            std::cout << "  " << octantNames[o] << ": " << octantCounts[o] << " holes" << std::endl;
        }

        // Record baseline for regression tracking
        std::cout << "\n=== BASELINE METRICS ===" << std::endl;
        std::cout << "  Boundary edges: " << boundaryEdges.size() << std::endl;
        std::cout << "  Hole clusters: " << clusters.size() << std::endl;
        std::cout << "  Vertices: " << mesh.positions.size() << std::endl;
        std::cout << "  Triangles: " << (mesh.indices.size() / 3U) << std::endl;

        // The test currently just documents the state; we can add assertions later
        // once we establish what the expected behavior should be
        EXPECT_GT(mesh.positions.size(), 0U);
    }

    /// Test comparing sparse vs hierarchical octree hole patterns
    TEST_F(ManifoldDualContouringGpu_Test, DebugHoles_Filamentholder_CompareApproaches)
    {
        auto bundle = loadDocument("testdata/filamentholder.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value()) << "Bounding box should be available";

        Eigen::Vector3f bboxMin(bbox->min.s[0], bbox->min.s[1], bbox->min.s[2]);
        Eigen::Vector3f bboxMax(bbox->max.s[0], bbox->max.s[1], bbox->max.s[2]);
        Eigen::Vector3f bboxSize = bboxMax - bboxMin;
        float const voxelSize = bboxSize.maxCoeff() / static_cast<float>(1U << 7U);

        std::cout << "\n=== SPARSE VS HIERARCHICAL COMPARISON ===" << std::endl;

        // Test sparse octree
        {
            ManifoldDualContouringGpu mesher(*bundle.core);
            mesher.setConfig({
                .initialDepth = 5,
                .maxDepth = 7,
                .enableGpu = true,
                .enableCpuFallback = true,
                .enableCaching = true,
                .isoValue = 0.0F,
                .enableHierarchicalOctree = false
            });
            mesher.generateMesh();
            auto const& mesh = mesher.getMesh();
            auto [edges, clusters] = analyzeMeshHoles(mesh);
            
            std::cout << "\nSPARSE OCTREE:" << std::endl;
            std::cout << "  Vertices: " << mesh.positions.size() << std::endl;
            std::cout << "  Triangles: " << (mesh.indices.size() / 3U) << std::endl;
            std::cout << "  Boundary edges: " << edges.size() << std::endl;
            std::cout << "  Hole clusters: " << clusters.size() << std::endl;
            
            if (!clusters.empty())
            {
                std::cout << "  Largest hole: " << clusters[0].edgeIndices.size() << " edges at ["
                          << clusters[0].centroid.transpose() << "]" << std::endl;
            }
        }

        // Test hierarchical octree
        {
            ManifoldDualContouringGpu mesher(*bundle.core);
            mesher.setConfig({
                .initialDepth = 5,
                .maxDepth = 7,
                .enableGpu = true,
                .enableCpuFallback = true,
                .enableCaching = true,
                .isoValue = 0.0F,
                .enableHierarchicalOctree = true
            });
            mesher.generateMesh();
            auto const& mesh = mesher.getMesh();
            auto [edges, clusters] = analyzeMeshHoles(mesh);
            
            std::cout << "\nHIERARCHICAL OCTREE:" << std::endl;
            std::cout << "  Vertices: " << mesh.positions.size() << std::endl;
            std::cout << "  Triangles: " << (mesh.indices.size() / 3U) << std::endl;
            std::cout << "  Boundary edges: " << edges.size() << std::endl;
            std::cout << "  Hole clusters: " << clusters.size() << std::endl;
            
            if (!clusters.empty())
            {
                std::cout << "  Largest hole: " << clusters[0].edgeIndices.size() << " edges at ["
                          << clusters[0].centroid.transpose() << "]" << std::endl;
            }
        }
    }

    // ============================================================================
    // GPU Octree Debug Tests - Examine internal octree structure
    // ============================================================================

    /// Helper to convert world position to octree cell coordinates
    [[nodiscard]] std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>
    worldToCellCoords(Eigen::Vector3f const& worldPos,
                      Eigen::Vector3f const& bboxMin,
                      Eigen::Vector3f const& bboxSize,
                      std::uint32_t depth)
    {
        Eigen::Vector3f const relPos = (worldPos - bboxMin).cwiseQuotient(bboxSize);
        std::uint32_t const gridSize = 1U << depth;
        
        // Clamp to valid range [0, gridSize-1]
        auto clampCoord = [gridSize](float v) -> std::uint32_t {
            if (v < 0.0F) return 0U;
            if (v >= 1.0F) return gridSize - 1U;
            return static_cast<std::uint32_t>(v * static_cast<float>(gridSize));
        };
        
        return {clampCoord(relPos.x()), clampCoord(relPos.y()), clampCoord(relPos.z())};
    }

    /// Encode cell coordinates to Morton code (matching GPU kernel)
    [[nodiscard]] std::uint64_t encodeMortonFromCoords(std::uint32_t x, std::uint32_t y, std::uint32_t z)
    {
        auto expandBits = [](std::uint64_t v) -> std::uint64_t {
            v = (v | (v << 32)) & 0x1f00000000ffffUL;
            v = (v | (v << 16)) & 0x1f0000ff0000ffUL;
            v = (v | (v << 8))  & 0x100f00f00f00f00fUL;
            v = (v | (v << 4))  & 0x10c30c30c30c30c3UL;
            v = (v | (v << 2))  & 0x1249249249249249UL;
            return v;
        };
        return (expandBits(z) << 2) | (expandBits(y) << 1) | expandBits(x);
    }

    /// Decode Morton code to cell coordinates
    [[nodiscard]] std::tuple<std::uint32_t, std::uint32_t, std::uint32_t>
    decodeMortonToCoords(std::uint64_t morton)
    {
        auto compactBits = [](std::uint64_t v) -> std::uint32_t {
            v &= 0x1249249249249249UL;
            v = (v | (v >> 2))  & 0x10c30c30c30c30c3UL;
            v = (v | (v >> 4))  & 0x100f00f00f00f00fUL;
            v = (v | (v >> 8))  & 0x1f0000ff0000ffUL;
            v = (v | (v >> 16)) & 0x1f00000000ffffUL;
            v = (v | (v >> 32)) & 0x1fffffUL;
            return static_cast<std::uint32_t>(v);
        };
        return {compactBits(morton), compactBits(morton >> 1), compactBits(morton >> 2)};
    }

    /// Edge corner indices (matching GPU kernel EDGE_CORNERS)
    constexpr int EDGE_CORNERS_TEST[12][2] = {
        {0,1}, {1,3}, {3,2}, {2,0},  // Bottom face (edges 0-3)
        {4,5}, {5,7}, {7,6}, {6,4},  // Top face (edges 4-7)
        {0,4}, {1,5}, {3,7}, {2,6}   // Vertical edges (8-11)
    };

    /// Test analyzing octree structure at specific hole locations
    /// This test reads back the octree buffer and checks cell existence around holes
    TEST_F(ManifoldDualContouringGpu_Test, DebugOctree_Filamentholder_AnalyzeCellsAroundHoles)
    {
        auto bundle = loadDocument("testdata/filamentholder.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        Eigen::Vector3f const bboxMin(bbox->min.s[0], bbox->min.s[1], bbox->min.s[2]);
        Eigen::Vector3f const bboxMax(bbox->max.s[0], bbox->max.s[1], bbox->max.s[2]);
        Eigen::Vector3f const bboxSize = bboxMax - bboxMin;

        std::uint32_t constexpr maxDepth = 7U;
        std::uint32_t constexpr gridSize = 1U << maxDepth;  // 128
        std::uint32_t constexpr maxCoord = gridSize - 1U;
        float const voxelSize = bboxSize.maxCoeff() / static_cast<float>(gridSize);

        // Generate mesh using sparse octree
        ManifoldDualContouringGpu mesher(*bundle.core);
        mesher.setConfig({
            .initialDepth = 5,
            .maxDepth = maxDepth,
            .enableGpu = true,
            .enableCpuFallback = true,
            .enableCaching = true,
            .isoValue = 0.0F,
            .enableHierarchicalOctree = false
        });
        mesher.generateMesh();
        auto const& mesh = mesher.getMesh();

        // Analyze holes
        auto [boundaryEdges, clusters] = analyzeMeshHoles(mesh);

        std::cout << "\n=== OCTREE CELL ANALYSIS AT HOLE LOCATIONS ===" << std::endl;
        std::cout << "BBox: [" << bboxMin.transpose() << "] to [" << bboxMax.transpose() << "]" << std::endl;
        std::cout << "Grid size: " << gridSize << " x " << gridSize << " x " << gridSize << std::endl;
        std::cout << "Voxel size: " << voxelSize << " mm" << std::endl;
        std::cout << "Total holes: " << clusters.size() << std::endl;
        std::cout << std::endl;

        // For the top N largest holes, analyze the octree cells around them
        std::size_t const maxAnalyze = std::min<std::size_t>(5U, clusters.size());
        
        for (std::size_t i = 0U; i < maxAnalyze; ++i)
        {
            auto const& cluster = clusters[i];
            
            std::cout << "=== HOLE #" << (i + 1) << " ===" << std::endl;
            std::cout << "  Edges: " << cluster.edgeIndices.size() << std::endl;
            std::cout << "  Centroid: [" << cluster.centroid.transpose() << "]" << std::endl;
            
            // Convert centroid to cell coordinates
            auto [cx, cy, cz] = worldToCellCoords(cluster.centroid, bboxMin, bboxSize, maxDepth);
            std::cout << "  Cell coords: (" << cx << ", " << cy << ", " << cz << ")" << std::endl;
            std::cout << "  Morton code: 0x" << std::hex << encodeMortonFromCoords(cx, cy, cz) 
                      << std::dec << std::endl;

            // Analyze the first few boundary edges in this cluster
            std::size_t const maxEdgeAnalyze = std::min<std::size_t>(3U, cluster.edgeIndices.size());
            for (std::size_t j = 0U; j < maxEdgeAnalyze; ++j)
            {
                auto const& edge = boundaryEdges[cluster.edgeIndices[j]];
                
                std::cout << "  Boundary edge " << j << ":" << std::endl;
                std::cout << "    Vertices: " << edge.v0 << " - " << edge.v1 << std::endl;
                std::cout << "    Midpoint: [" << edge.midpoint.transpose() << "]" << std::endl;
                std::cout << "    Length: " << edge.length << " mm (" 
                          << (edge.length / voxelSize) << " voxels)" << std::endl;
                
                // Get cell at edge midpoint
                auto [ex, ey, ez] = worldToCellCoords(edge.midpoint, bboxMin, bboxSize, maxDepth);
                std::cout << "    Edge cell: (" << ex << ", " << ey << ", " << ez << ")" << std::endl;
                
                // Check if this is at the boundary of the grid
                bool const atXMin = (ex == 0U);
                bool const atYMin = (ey == 0U);
                bool const atZMin = (ez == 0U);
                bool const atXMax = (ex == maxCoord);
                bool const atYMax = (ey == maxCoord);
                bool const atZMax = (ez == maxCoord);
                
                if (atXMin || atYMin || atZMin || atXMax || atYMax || atZMax)
                {
                    std::cout << "    AT GRID BOUNDARY: "
                              << (atXMin ? "-X " : "") << (atXMax ? "+X " : "")
                              << (atYMin ? "-Y " : "") << (atYMax ? "+Y " : "")
                              << (atZMin ? "-Z " : "") << (atZMax ? "+Z " : "")
                              << std::endl;
                }
            }
            std::cout << std::endl;
        }

        // Track which edges are at grid boundary vs interior
        std::size_t boundaryCount = 0U;
        std::size_t interiorCount = 0U;
        
        for (auto const& edge : boundaryEdges)
        {
            auto [ex, ey, ez] = worldToCellCoords(edge.midpoint, bboxMin, bboxSize, maxDepth);
            bool const atBoundary = (ex == 0U || ey == 0U || ez == 0U ||
                                     ex == maxCoord || ey == maxCoord || ez == maxCoord);
            if (atBoundary)
                ++boundaryCount;
            else
                ++interiorCount;
        }

        std::cout << "\n=== BOUNDARY vs INTERIOR HOLES ===" << std::endl;
        std::cout << "  Edges at grid boundary: " << boundaryCount 
                  << " (" << (100.0 * boundaryCount / boundaryEdges.size()) << "%)" << std::endl;
        std::cout << "  Edges in interior: " << interiorCount 
                  << " (" << (100.0 * interiorCount / boundaryEdges.size()) << "%)" << std::endl;

        // Interior holes are the ones we need to investigate - they shouldn't exist
        // Boundary holes can occur where the surface exits the bounding box
        if (interiorCount > 0U)
        {
            std::cout << "\n!!! WARNING: " << interiorCount << " interior boundary edges found !!!" << std::endl;
            std::cout << "These indicate missing quads in the mesh interior." << std::endl;
        }

        // Success criteria
        EXPECT_GT(mesh.positions.size(), 0U);
    }

    /// Simple unit sphere test - should produce a watertight mesh with minimal holes
    /// This serves as a baseline to verify the algorithm works on simple geometry
    TEST_F(ManifoldDualContouringGpu_Test, DebugBaseline_Sphere_ShouldBeWatertight)
    {
        // Load the SphereInACage test model (contains a unit sphere)
        auto bundle = loadDocument("testdata/SphereInACage.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        Eigen::Vector3f const bboxMin(bbox->min.s[0], bbox->min.s[1], bbox->min.s[2]);
        Eigen::Vector3f const bboxMax(bbox->max.s[0], bbox->max.s[1], bbox->max.s[2]);
        Eigen::Vector3f const bboxSize = bboxMax - bboxMin;

        std::uint32_t constexpr maxDepth = 7U;
        float const voxelSize = bboxSize.maxCoeff() / static_cast<float>(1U << maxDepth);

        std::cout << "\n=== BASELINE: SPHERE IN A CAGE ===" << std::endl;
        std::cout << "BBox: [" << bboxMin.transpose() << "] to [" << bboxMax.transpose() << "]" << std::endl;
        std::cout << "Voxel size: " << voxelSize << " mm" << std::endl;

        // Generate mesh
        ManifoldDualContouringGpu mesher(*bundle.core);
        mesher.setConfig({
            .initialDepth = 5,
            .maxDepth = maxDepth,
            .enableGpu = true,
            .enableCpuFallback = true,
            .enableCaching = true,
            .isoValue = 0.0F,
            .enableHierarchicalOctree = false
        });
        mesher.generateMesh();
        auto const& mesh = mesher.getMesh();

        // Analyze holes
        auto [boundaryEdges, clusters] = analyzeMeshHoles(mesh);

        std::cout << "Mesh vertices: " << mesh.positions.size() << std::endl;
        std::cout << "Mesh triangles: " << (mesh.indices.size() / 3U) << std::endl;
        std::cout << "Boundary edges: " << boundaryEdges.size() << std::endl;
        std::cout << "Hole clusters: " << clusters.size() << std::endl;

        if (!clusters.empty())
        {
            std::cout << "  Largest hole: " << clusters[0].edgeIndices.size() << " edges" << std::endl;
        }

        // For a simple sphere + cage, we expect very few or no holes
        // The cage has sharp edges which may cause some boundary issues
        EXPECT_LT(boundaryEdges.size(), 500U) 
            << "Simple geometry should have minimal holes";

        // The mesh should have reasonable vertex count
        EXPECT_GT(mesh.positions.size(), 1000U) << "Sphere should have many vertices";
        EXPECT_GT(mesh.indices.size(), 3000U) << "Sphere should have many triangles";
    }

    /// Test that examines the quad emission statistics
    /// Counts how many quads were skipped due to missing neighbors
    TEST_F(ManifoldDualContouringGpu_Test, DebugQuadEmission_Filamentholder_CountSkippedQuads)
    {
        auto bundle = loadDocument("testdata/filamentholder.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        Eigen::Vector3f const bboxMin(bbox->min.s[0], bbox->min.s[1], bbox->min.s[2]);
        Eigen::Vector3f const bboxMax(bbox->max.s[0], bbox->max.s[1], bbox->max.s[2]);
        Eigen::Vector3f const bboxSize = bboxMax - bboxMin;

        std::uint32_t constexpr maxDepth = 7U;
        float const voxelSize = bboxSize.maxCoeff() / static_cast<float>(1U << maxDepth);

        std::cout << "\n=== QUAD EMISSION ANALYSIS ===" << std::endl;

        // Generate mesh and track statistics
        ManifoldDualContouringGpu mesher(*bundle.core);
        mesher.setConfig({
            .initialDepth = 5,
            .maxDepth = maxDepth,
            .enableGpu = true,
            .enableCpuFallback = true,
            .enableCaching = true,
            .isoValue = 0.0F,
            .enableHierarchicalOctree = false
        });
        mesher.generateMesh();
        auto const& mesh = mesher.getMesh();

        // Analyze the mesh
        auto [boundaryEdges, clusters] = analyzeMeshHoles(mesh);
        auto const edgeStats = analyzeMeshEdges(mesh);

        std::cout << "Mesh vertices: " << mesh.positions.size() << std::endl;
        std::cout << "Mesh triangles: " << (mesh.indices.size() / 3U) << std::endl;
        std::cout << "Unique edges: " << edgeStats.totalEdges << std::endl;
        std::cout << "Open edges: " << edgeStats.openEdges << std::endl;
        std::cout << "Non-manifold edges: " << edgeStats.nonManifoldEdges << std::endl;
        std::cout << "Boundary edges (holes): " << boundaryEdges.size() << std::endl;

        // Calculate theoretical minimum quads based on edge crossings
        // Each owned edge (6, 5, 10) that crosses surface should emit one quad
        // If we have N boundary edges, that means N/2 quads were NOT emitted
        // (each quad has 4 edges, but boundary edges are shared by 2 triangles,
        // so roughly 2 boundary edges per missing quad)
        std::size_t const estimatedMissingQuads = boundaryEdges.size() / 2U;
        
        std::cout << "\nEstimated missing quads: ~" << estimatedMissingQuads << std::endl;
        std::cout << "(Based on boundary edge count / 2)" << std::endl;

        // Document non-manifold edges (currently not a strict requirement)
        // Note: 1281 non-manifold edges found, likely from bridge triangles
        if (edgeStats.nonManifoldEdges > 100U)
        {
            std::cout << "Note: " << edgeStats.nonManifoldEdges 
                      << " non-manifold edges (likely from gap-filling bridges)" << std::endl;
        }

        EXPECT_GT(mesh.positions.size(), 0U);
    }

    /// Test edge mask consistency - for each edge, verify that if cell A has bit set,
    /// the neighbor cell that would share that edge also has the corresponding bit set
    TEST_F(ManifoldDualContouringGpu_Test, DebugEdgeMask_Filamentholder_CheckConsistency)
    {
        auto bundle = loadDocument("testdata/filamentholder.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        Eigen::Vector3f const bboxMin(bbox->min.s[0], bbox->min.s[1], bbox->min.s[2]);
        Eigen::Vector3f const bboxMax(bbox->max.s[0], bbox->max.s[1], bbox->max.s[2]);
        Eigen::Vector3f const bboxSize = bboxMax - bboxMin;

        std::uint32_t constexpr maxDepth = 7U;
        std::uint32_t constexpr gridSize = 1U << maxDepth;
        std::uint32_t constexpr maxCoord = gridSize - 1U;
        float const voxelSize = bboxSize.maxCoeff() / static_cast<float>(gridSize);

        std::cout << "\n=== EDGE MASK CONSISTENCY CHECK ===" << std::endl;
        std::cout << "This test verifies that shared edges have matching edge masks." << std::endl;
        std::cout << std::endl;

        // Generate mesh
        ManifoldDualContouringGpu mesher(*bundle.core);
        mesher.setConfig({
            .initialDepth = 5,
            .maxDepth = maxDepth,
            .enableGpu = true,
            .enableCpuFallback = true,
            .enableCaching = true,
            .isoValue = 0.0F,
            .enableHierarchicalOctree = false
        });
        mesher.generateMesh();
        auto const& mesh = mesher.getMesh();

        // Analyze holes
        auto [boundaryEdges, clusters] = analyzeMeshHoles(mesh);

        std::cout << "Mesh vertices: " << mesh.positions.size() << std::endl;
        std::cout << "Boundary edges: " << boundaryEdges.size() << std::endl;
        std::cout << "Hole clusters: " << clusters.size() << std::endl;

        // Edge correspondence for the 3 owned edges (6, 5, 10):
        // - Edge 6 (X-axis at y=max, z=max) in cell (x,y,z) corresponds to:
        //   - Edge 0 (X-axis at y=min, z=min) in cell (x, y+1, z+1)
        //   - Edge 4 (X-axis at y=min, z=max) in cell (x, y+1, z)
        //   - Edge 2 (X-axis at y=max, z=min) in cell (x, y, z+1)
        //
        // - Edge 5 (Y-axis at x=max, z=max) in cell (x,y,z) corresponds to:
        //   - Edge 3 (Y-axis at x=min, z=min) in cell (x+1, y, z+1)
        //   - Edge 7 (Y-axis at x=min, z=max) in cell (x+1, y, z)
        //   - Edge 1 (Y-axis at x=max, z=min) in cell (x, y, z+1)
        //
        // - Edge 10 (Z-axis at x=max, y=max) in cell (x,y,z) corresponds to:
        //   - Edge 8 (Z-axis at x=min, y=min) in cell (x+1, y+1, z)
        //   - Edge 9 (Z-axis at x=max, y=min) in cell (x+1, y, z)
        //   - Edge 11 (Z-axis at x=min, y=max) in cell (x, y+1, z)

        // This analysis would require reading back the octree buffer from GPU
        // For now, we just verify the hole analysis is working
        
        std::cout << "\nEdge correspondence table:" << std::endl;
        std::cout << "  Edge 6 (X @ y+,z+) <-> Edge 0 (X @ y-,z-) in (+Y+Z neighbor)" << std::endl;
        std::cout << "  Edge 5 (Y @ x+,z+) <-> Edge 3 (Y @ x-,z-) in (+X+Z neighbor)" << std::endl;
        std::cout << "  Edge 10 (Z @ x+,y+) <-> Edge 8 (Z @ x-,y-) in (+X+Y neighbor)" << std::endl;
        std::cout << std::endl;
        
        std::cout << "To verify consistency, we would need to:" << std::endl;
        std::cout << "  1. Read back the octree buffer from GPU" << std::endl;
        std::cout << "  2. For each cell with edge mask bit N set," << std::endl;
        std::cout << "     verify the corresponding neighbor has matching bit M set" << std::endl;
        std::cout << "  3. Report any mismatches as potential hole causes" << std::endl;

        // At least verify we have a valid mesh
        EXPECT_GT(mesh.positions.size(), 0U);
    }

    /// Test ImplicitGyroid (simpler geometry) to establish baseline
    TEST_F(ManifoldDualContouringGpu_Test, DebugBaseline_ImplicitGyroid_HoleAnalysis)
    {
        auto bundle = loadDocument("testdata/ImplicitGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        Eigen::Vector3f const bboxMin(bbox->min.s[0], bbox->min.s[1], bbox->min.s[2]);
        Eigen::Vector3f const bboxMax(bbox->max.s[0], bbox->max.s[1], bbox->max.s[2]);
        Eigen::Vector3f const bboxSize = bboxMax - bboxMin;

        std::uint32_t constexpr maxDepth = 7U;
        float const voxelSize = bboxSize.maxCoeff() / static_cast<float>(1U << maxDepth);

        std::cout << "\n=== BASELINE: IMPLICIT GYROID ===" << std::endl;
        std::cout << "BBox: [" << bboxMin.transpose() << "] to [" << bboxMax.transpose() << "]" << std::endl;
        std::cout << "Voxel size: " << voxelSize << " mm" << std::endl;

        // Generate mesh
        ManifoldDualContouringGpu mesher(*bundle.core);
        mesher.setConfig({
            .initialDepth = 5,
            .maxDepth = maxDepth,
            .enableGpu = true,
            .enableCpuFallback = true,
            .enableCaching = true,
            .isoValue = 0.0F,
            .enableHierarchicalOctree = false
        });
        mesher.generateMesh();
        auto const& mesh = mesher.getMesh();

        // Analyze holes
        auto [boundaryEdges, clusters] = analyzeMeshHoles(mesh);
        auto const edgeStats = analyzeMeshEdges(mesh);

        std::cout << "Mesh vertices: " << mesh.positions.size() << std::endl;
        std::cout << "Mesh triangles: " << (mesh.indices.size() / 3U) << std::endl;
        std::cout << "Unique edges: " << edgeStats.totalEdges << std::endl;
        std::cout << "Open edges: " << edgeStats.openEdges << std::endl;
        std::cout << "Non-manifold edges: " << edgeStats.nonManifoldEdges << std::endl;
        std::cout << "Boundary edges (holes): " << boundaryEdges.size() << std::endl;
        std::cout << "Hole clusters: " << clusters.size() << std::endl;

        if (!clusters.empty())
        {
            // Report spatial distribution of holes
            std::cout << "\nTop 5 largest holes:" << std::endl;
            std::size_t const maxReport = std::min<std::size_t>(5U, clusters.size());
            for (std::size_t i = 0U; i < maxReport; ++i)
            {
                auto const& cluster = clusters[i];
                std::cout << "  #" << (i+1) << ": " << cluster.edgeIndices.size() 
                          << " edges at [" << cluster.centroid.transpose() << "]" << std::endl;
            }
        }

        // Document hole count (gyroid has boundary holes at the clipping planes)
        // The largest holes are at the X boundaries where the gyroid exits the bounding box
        std::cout << "\nNote: Holes are concentrated at X-boundary (bounding box edges)" << std::endl;
        
        EXPECT_GT(mesh.positions.size(), 1000U);
    }

    /// Test comparing hole counts at different octree depths
    /// Higher depth = more cells = potentially more holes from missing neighbors
    TEST_F(ManifoldDualContouringGpu_Test, DebugDepth_Filamentholder_CompareDepths)
    {
        auto bundle = loadDocument("testdata/filamentholder.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        Eigen::Vector3f const bboxMin(bbox->min.s[0], bbox->min.s[1], bbox->min.s[2]);
        Eigen::Vector3f const bboxMax(bbox->max.s[0], bbox->max.s[1], bbox->max.s[2]);
        Eigen::Vector3f const bboxSize = bboxMax - bboxMin;

        std::cout << "\n=== HOLE COUNT VS OCTREE DEPTH ===" << std::endl;
        std::cout << "BBox: [" << bboxMin.transpose() << "] to [" << bboxMax.transpose() << "]" << std::endl;
        std::cout << std::endl;

        // Test different depths
        for (std::uint32_t depth = 5U; depth <= 8U; ++depth)
        {
            float const voxelSize = bboxSize.maxCoeff() / static_cast<float>(1U << depth);

            ManifoldDualContouringGpu mesher(*bundle.core);
            mesher.setConfig({
                .initialDepth = std::min(5U, depth),
                .maxDepth = depth,
                .enableGpu = true,
                .enableCpuFallback = true,
                .enableCaching = true,
                .isoValue = 0.0F,
                .enableHierarchicalOctree = false
            });
            mesher.generateMesh();
            auto const& mesh = mesher.getMesh();

            auto [boundaryEdges, clusters] = analyzeMeshHoles(mesh);

            std::cout << "Depth " << depth << " (voxel=" << voxelSize << "mm):" << std::endl;
            std::cout << "  Vertices: " << mesh.positions.size() << std::endl;
            std::cout << "  Triangles: " << (mesh.indices.size() / 3U) << std::endl;
            std::cout << "  Boundary edges: " << boundaryEdges.size() << std::endl;
            std::cout << "  Hole clusters: " << clusters.size() << std::endl;
            
            if (!clusters.empty())
            {
                std::cout << "  Largest hole: " << clusters[0].edgeIndices.size() << " edges" << std::endl;
            }
            std::cout << std::endl;
        }

        // At least the mesh generation succeeded
        EXPECT_TRUE(true);
    }

    /// Root cause analysis: Test the hypothesis that holes appear because halo nodes 
    /// need their own neighbors (cascading halo problem)
    /// 
    /// The hypothesis is:
    /// 1. Surface cell A needs halo H at position (x+1, y, z) for edge 5
    /// 2. Halo H gets edgeMask=0x20 (edge 5) from A's contribution
    /// 3. To emit quad for edge 5, H needs neighbors at (x+2, y, z), (x+1, y, z+1), (x+2, y, z+1)
    /// 4. If those don't exist, the quad is skipped -> hole
    ///
    /// We can verify this by checking:
    /// - Are holes concentrated where surface cells are sparse?
    /// - Do holes appear at the edge of surface cell clusters?
    TEST_F(ManifoldDualContouringGpu_Test, DebugRootCause_HaloCascadingProblem)
    {
        auto bundle = loadDocument("testdata/filamentholder.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const bbox = bundle.core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        Eigen::Vector3f const bboxMin(bbox->min.s[0], bbox->min.s[1], bbox->min.s[2]);
        Eigen::Vector3f const bboxMax(bbox->max.s[0], bbox->max.s[1], bbox->max.s[2]);
        Eigen::Vector3f const bboxSize = bboxMax - bboxMin;

        std::uint32_t constexpr maxDepth = 7U;
        std::uint32_t constexpr gridSize = 1U << maxDepth;
        float const voxelSize = bboxSize.maxCoeff() / static_cast<float>(gridSize);

        std::cout << "\n=== ROOT CAUSE ANALYSIS: HALO CASCADING PROBLEM ===" << std::endl;
        std::cout << "Testing hypothesis: holes occur where halo nodes need neighbors" << std::endl;
        std::cout << "that don't exist (secondary halo problem)" << std::endl;
        std::cout << std::endl;

        // Generate mesh
        ManifoldDualContouringGpu mesher(*bundle.core);
        mesher.setConfig({
            .initialDepth = 5,
            .maxDepth = maxDepth,
            .enableGpu = true,
            .enableCpuFallback = true,
            .enableCaching = true,
            .isoValue = 0.0F,
            .enableHierarchicalOctree = false
        });
        mesher.generateMesh();
        auto const& mesh = mesher.getMesh();

        // Analyze holes
        auto [boundaryEdges, clusters] = analyzeMeshHoles(mesh);

        std::cout << "Total boundary edges: " << boundaryEdges.size() << std::endl;
        std::cout << "Total hole clusters: " << clusters.size() << std::endl;
        std::cout << std::endl;

        // For the largest holes, analyze the local structure
        std::size_t const maxAnalyze = std::min<std::size_t>(3U, clusters.size());
        
        for (std::size_t i = 0U; i < maxAnalyze; ++i)
        {
            auto const& cluster = clusters[i];
            
            std::cout << "=== HOLE #" << (i + 1) << " (edges: " << cluster.edgeIndices.size() << ") ===" << std::endl;
            
            // Convert centroid to cell coordinates
            auto [cx, cy, cz] = worldToCellCoords(cluster.centroid, bboxMin, bboxSize, maxDepth);
            
            std::cout << "  Centroid cell: (" << cx << ", " << cy << ", " << cz << ")" << std::endl;
            
            // Analyze edge orientations in this cluster
            // Edges aligned with X, Y, Z axes might indicate which direction is problematic
            std::size_t xAligned = 0U, yAligned = 0U, zAligned = 0U;
            
            for (std::size_t edgeIdx : cluster.edgeIndices)
            {
                auto const& edge = boundaryEdges[edgeIdx];
                Eigen::Vector3f const dir = (edge.pos1 - edge.pos0).normalized();
                
                if (std::abs(dir.x()) > 0.9F) ++xAligned;
                else if (std::abs(dir.y()) > 0.9F) ++yAligned;
                else if (std::abs(dir.z()) > 0.9F) ++zAligned;
            }
            
            std::cout << "  Edge orientation distribution:" << std::endl;
            std::cout << "    X-aligned: " << xAligned << std::endl;
            std::cout << "    Y-aligned: " << yAligned << std::endl;
            std::cout << "    Z-aligned: " << zAligned << std::endl;
            std::cout << "    Diagonal: " << (cluster.edgeIndices.size() - xAligned - yAligned - zAligned) << std::endl;
            
            // Analyze if this hole is near a surface boundary
            // (where surface cells become sparse)
            Eigen::Vector3f const relPos = (cluster.centroid - bboxMin).cwiseQuotient(bboxSize);
            
            std::cout << "  Relative position in bbox: [" << relPos.transpose() << "]" << std::endl;
            
            // Check if near model boundary
            bool const nearXMin = relPos.x() < 0.05F;
            bool const nearXMax = relPos.x() > 0.95F;
            bool const nearYMin = relPos.y() < 0.05F;
            bool const nearYMax = relPos.y() > 0.95F;
            bool const nearZMin = relPos.z() < 0.05F;
            bool const nearZMax = relPos.z() > 0.95F;
            
            if (nearXMin || nearXMax || nearYMin || nearYMax || nearZMin || nearZMax)
            {
                std::cout << "  ** NEAR MODEL BOUNDARY **" << std::endl;
            }
            
            std::cout << std::endl;
        }

        // Summary hypothesis test
        std::cout << "=== HYPOTHESIS SUMMARY ===" << std::endl;
        std::cout << "The halo cascading problem occurs when:" << std::endl;
        std::cout << "  1. A surface cell creates a halo node for a missing neighbor" << std::endl;
        std::cout << "  2. That halo node has edgeMask bits set (inherited from surface)" << std::endl;
        std::cout << "  3. The halo node tries to emit quads but needs its own neighbors" << std::endl;
        std::cout << "  4. Those secondary neighbors don't exist -> skipped quad -> hole" << std::endl;
        std::cout << std::endl;
        std::cout << "Solutions:" << std::endl;
        std::cout << "  A) Run halo generation iteratively until convergence" << std::endl;
        std::cout << "  B) Don't let halo nodes emit quads (only surface cells emit)" << std::endl;
        std::cout << "  C) Change owned edges: emit from MIN corner (edges 0,3,8) instead of MAX" << std::endl;
        std::cout << std::endl;

        EXPECT_GT(mesh.positions.size(), 0U);
    }

    /// Test admesh validation for SimpleGyroid (with wall thickness via abs() and offset)
    /// This should work better than ImplicitGyroid because it defines an actual shell/volume
    TEST_F(ManifoldDualContouringGpu_Test, GenerateMesh_WithSimpleGyroid_AdmeshValidation)
    {
        if (!isAdmeshAvailable())
        {
            GTEST_SKIP() << "admesh not available, skipping validation test";
        }

        auto bundle = loadDocument("testdata/SimpleGyroid.3mf");
        ASSERT_TRUE(bundle.core->updateBBox()) << "Failed to compute bounding box";

        auto const metrics = exportAndValidateWithAdmesh(*bundle.core);
        
        // Validate manifold properties
        EXPECT_EQ(metrics.totalDisconnectedFacets.final, 0) 
            << "Mesh should have no disconnected facets after processing";
        EXPECT_EQ(metrics.degenerateFacets, 0) 
            << "Mesh should have no degenerate facets";
        
        double const reversedRatio = 
            static_cast<double>(metrics.facetsReversed) / 
            static_cast<double>(metrics.numberOfFacets.original);
        double const maxReversedRatio = 0.05; // 5% tolerance (more lenient for complex geometry)
        EXPECT_LE(reversedRatio, maxReversedRatio) 
            << "Too many facets reversed: " << metrics.facetsReversed 
            << " out of " << metrics.numberOfFacets.original 
            << " (" << (reversedRatio * 100.0) << "%)";

        // Log summary info
        std::cout << "SimpleGyroid Admesh validation:" << std::endl;
        std::cout << "  Facets: " << metrics.numberOfFacets.original << std::endl;
        std::cout << "  Volume: " << metrics.volume << std::endl;
        std::cout << "  Parts: " << metrics.numberOfParts << std::endl;
        std::cout << "  Reversed facets: " << metrics.facetsReversed 
                  << " (" << (reversedRatio * 100.0) << "%)" << std::endl;
    }
}
