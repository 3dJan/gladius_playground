#if defined(GLADIUS_ENABLE_WEBGPU)

#include "Document.h"
#include "EventLogger.h"
#include "compute/ApplicationComputeRuntime.h"
#include "io/IndexedTriangleMesh.h"
#include "io/MeshExportJob.h"
#include "io/3mf/FaceColorSampler.h"
#include "io/3mf/Lib3mfLoader.h"
#include "io/3mf/MmuSegmentationWriter.h"

#include <gtest/gtest.h>
#include <minizip/unzip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <sstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace gladius::tests
{
    namespace
    {
        class Temporary3mf
        {
          public:
            Temporary3mf()
            {
                auto const unique = std::chrono::steady_clock::now().time_since_epoch().count();
                m_path = std::filesystem::temp_directory_path() /
                         ("gladius_webgpu_export_" + std::to_string(unique) + ".3mf");
            }

            ~Temporary3mf()
            {
                if (std::getenv("GLADIUS_KEEP_WEBGPU_EXPORT") != nullptr)
                {
                    return;
                }
                std::error_code error;
                std::filesystem::remove(m_path, error);
            }

            Temporary3mf(Temporary3mf const &) = delete;
            Temporary3mf & operator=(Temporary3mf const &) = delete;

            [[nodiscard]] std::filesystem::path const & path() const noexcept
            {
                return m_path;
            }

          private:
            std::filesystem::path m_path;
        };

        struct ArchiveCloser
        {
            void operator()(void * archive) const noexcept
            {
                if (archive != nullptr)
                {
                    unzClose(archive);
                }
            }
        };

        [[nodiscard]] io::MeshExportRequest makeUiEquivalentRequest(bool const exportWithColors)
        {
            io::MeshExportRequest request;
            request.format = io::MeshExportFormat::ThreeMf;
            request.extraction.method = io::SurfaceExtractionMethod::ManifoldDualContouring;
            auto & options = request.extraction.manifoldDualContouring;
            options.qualityPreset = io::ManifoldDualContouringQuality::UltraFine;
            options.applyPreset();
            options.enableGpu = true;
            options.enableCpuFallback = true;
            options.enableCaching = true;
            options.minFeatureSize = 0.0F;
            options.enableHierarchicalOctree = true;
            options.enableSharpFeaturePostProcess = false;
            options.projectToSurface = true;
            options.simplificationMethod = io::SimplificationMethod::QemFast;
            options.enableSimplification = true;
            options.simplificationTerminationMode = io::SimplificationTerminationMode::ErrorBounded;
            options.simplificationMaxError = 0.05F * 0.05F;

            request.color.exportWithColors = exportWithColors;
            request.color.convertToSrgb = true;
            request.color.preferredColorMode = io::ColorMode::PerVertex;
            request.color.quantizationMode = io::QuantizationMode::Adaptive;
            request.color.targetApplication = exportWithColors ? io::TargetApplication::Orca
                                                               : io::TargetApplication::None;
            return request;
        }

        [[nodiscard]] bool hasVolumetricColor(Document const & document)
        {
            auto const assembly = document.getFlatAssembly();
            return assembly != nullptr && assembly->assemblyModel() != nullptr &&
                   io::FaceColorSampler::hasVolumetricColor(*assembly->assemblyModel());
        }

        [[nodiscard]] io::IndexedTriangleMesh readIndexedMesh(std::filesystem::path const & filePath)
        {
            auto const wrapper = io::loadLib3mfScoped();
            auto const model = wrapper->CreateModel();
            auto const reader = model->QueryReader("3mf");
            reader->SetStrictModeActive(false);
            reader->ReadFromFile(filePath.string());

            auto const objects = model->GetMeshObjects();
            if (!objects->MoveNext())
            {
                throw std::runtime_error("Exported 3MF contains no mesh object");
            }
            auto const object = objects->GetCurrentMeshObject();
            if (objects->MoveNext())
            {
                throw std::runtime_error("Expected a single mesh object in the Orca export");
            }

            io::IndexedTriangleMesh mesh;
            mesh.positions.reserve(object->GetVertexCount());
            mesh.indices.reserve(static_cast<std::size_t>(object->GetTriangleCount()) * 3U);
            for (Lib3MF_uint32 index = 0U; index < object->GetVertexCount(); ++index)
            {
                auto const vertex = object->GetVertex(index);
                mesh.positions.emplace_back(vertex.m_Coordinates[0],
                                            vertex.m_Coordinates[1],
                                            vertex.m_Coordinates[2]);
            }
            for (Lib3MF_uint32 index = 0U; index < object->GetTriangleCount(); ++index)
            {
                auto const triangle = object->GetTriangle(index);
                mesh.indices.insert(mesh.indices.end(),
                                    {triangle.m_Indices[0],
                                     triangle.m_Indices[1],
                                     triangle.m_Indices[2]});
            }
            return mesh;
        }

        [[nodiscard]] std::string readModelXml(std::filesystem::path const & filePath)
        {
            auto const archive = unzOpen64(filePath.string().c_str());
            if (archive == nullptr)
            {
                throw std::runtime_error("Cannot open exported 3MF ZIP");
            }
            std::unique_ptr<void, ArchiveCloser> archiveGuard{archive};
            if (unzLocateFile(archive, "3D/3dmodel.model", 0) != UNZ_OK ||
                unzOpenCurrentFile(archive) != UNZ_OK)
            {
                throw std::runtime_error("Cannot locate exported 3MF model XML");
            }

            std::string xml;
            std::array<char, 4096> buffer{};
            int count = 0;
            while ((count = unzReadCurrentFile(
                      archive, buffer.data(), static_cast<unsigned>(buffer.size()))) > 0)
            {
                xml.append(buffer.data(), static_cast<std::size_t>(count));
            }
            unzCloseCurrentFile(archive);
            if (count < 0)
            {
                throw std::runtime_error("Cannot read exported 3MF model XML");
            }
            return xml;
        }

        [[nodiscard]] std::optional<std::string> findAttribute(std::string_view const tag,
                                                               std::string_view const name)
        {
            std::string key{name};
            key += "=\"";
            auto const valueStart = tag.find(key);
            if (valueStart == std::string_view::npos)
            {
                return std::nullopt;
            }
            auto const start = valueStart + key.size();
            auto const end = tag.find('"', start);
            if (end == std::string_view::npos)
            {
                return std::nullopt;
            }
            return std::string{tag.substr(start, end - start)};
        }

        void expectOrcaAttributes(std::filesystem::path const & filePath,
                                  std::size_t const expectedTriangleCount)
        {
            auto const xml = readModelXml(filePath);
            std::unordered_set<std::string> validAssignments;
            for (int extruder = 1; extruder <= io::MmuSegmentationWriter::MAX_EXTRUDERS; ++extruder)
            {
                validAssignments.insert(io::MmuSegmentationWriter::encodeExtruderState(extruder));
            }

            std::size_t triangleCount = 0U;
            std::size_t position = 0U;
            while ((position = xml.find("<triangle ", position)) != std::string::npos)
            {
                auto const end = xml.find('>', position);
                ASSERT_NE(end, std::string::npos) << "Malformed triangle element in exported 3MF";
                auto const tag = std::string_view{xml}.substr(position, end - position);
                auto const paintColor = findAttribute(tag, "paint_color");
                auto const segmentation = findAttribute(tag, "slic3rpe:mmu_segmentation");
                ASSERT_TRUE(paintColor.has_value()) << "Triangle " << triangleCount
                                                    << " is missing Orca paint_color";
                ASSERT_TRUE(segmentation.has_value()) << "Triangle " << triangleCount
                                                      << " is missing MMU segmentation";
                EXPECT_EQ(*paintColor, *segmentation);
                EXPECT_TRUE(validAssignments.contains(*paintColor))
                  << "Unexpected MMU assignment on triangle " << triangleCount;
                ++triangleCount;
                position = end + 1U;
            }
            EXPECT_EQ(triangleCount, expectedTriangleCount);
        }

        [[nodiscard]] bool hasManifoldVertexLinks(io::IndexedTriangleMesh const & mesh)
        {
            struct LinkEdge
            {
                std::uint32_t center;
                std::uint32_t first;
                std::uint32_t second;
            };
            std::vector<LinkEdge> linkEdges;
            linkEdges.reserve(mesh.indices.size());
            for (std::size_t offset = 0U; offset < mesh.indices.size(); offset += 3U)
            {
                auto const first = mesh.indices[offset];
                auto const second = mesh.indices[offset + 1U];
                auto const third = mesh.indices[offset + 2U];
                linkEdges.push_back({first, second, third});
                linkEdges.push_back({second, third, first});
                linkEdges.push_back({third, first, second});
            }
            std::sort(linkEdges.begin(), linkEdges.end(), [](LinkEdge const & first, LinkEdge const & second)
            {
                if (first.center != second.center)
                {
                    return first.center < second.center;
                }
                if (first.first != second.first)
                {
                    return first.first < second.first;
                }
                return first.second < second.second;
            });

            std::vector<std::uint32_t> degrees(mesh.positions.size(), 0U);
            std::vector<std::uint32_t> parents(mesh.positions.size(), 0U);
            std::vector<std::uint32_t> ranks(mesh.positions.size(), 0U);
            std::vector<std::uint32_t> neighbors;
            auto findRoot = [&parents](std::uint32_t vertex)
            {
                auto root = vertex;
                while (parents[root] != root)
                {
                    root = parents[root];
                }
                while (parents[vertex] != vertex)
                {
                    auto const next = parents[vertex];
                    parents[vertex] = root;
                    vertex = next;
                }
                return root;
            };
            std::size_t edgeStart = 0U;
            while (edgeStart < linkEdges.size())
            {
                std::size_t edgeEnd = edgeStart + 1U;
                while (edgeEnd < linkEdges.size() &&
                       linkEdges[edgeEnd].center == linkEdges[edgeStart].center)
                {
                    ++edgeEnd;
                }

                neighbors.clear();
                auto addLinkNeighbor = [&degrees, &parents, &ranks, &neighbors](std::uint32_t vertex)
                {
                    if (degrees[vertex] == 0U)
                    {
                        parents[vertex] = vertex;
                        ranks[vertex] = 0U;
                        neighbors.push_back(vertex);
                    }
                    ++degrees[vertex];
                };
                for (std::size_t edge = edgeStart; edge < edgeEnd; ++edge)
                {
                    auto const first = linkEdges[edge].first;
                    auto const second = linkEdges[edge].second;
                    addLinkNeighbor(first);
                    addLinkNeighbor(second);
                    auto firstRoot = findRoot(first);
                    auto secondRoot = findRoot(second);
                    if (firstRoot != secondRoot)
                    {
                        if (ranks[firstRoot] < ranks[secondRoot])
                        {
                            std::swap(firstRoot, secondRoot);
                        }
                        parents[secondRoot] = firstRoot;
                        if (ranks[firstRoot] == ranks[secondRoot])
                        {
                            ++ranks[firstRoot];
                        }
                    }
                }

                auto const root = findRoot(neighbors.front());
                for (auto const neighbor : neighbors)
                {
                    if (degrees[neighbor] != 2U || findRoot(neighbor) != root)
                    {
                        return false;
                    }
                    degrees[neighbor] = 0U;
                }
                edgeStart = edgeEnd;
            }
            return true;
        }

        [[nodiscard]] io::IndexedTriangleMesh makePinchedTetrahedra()
        {
            io::IndexedTriangleMesh mesh;
            mesh.positions = {{0.0F, 0.0F, 0.0F},
                              {1.0F, 0.0F, 0.0F},
                              {0.0F, 1.0F, 0.0F},
                              {0.0F, 0.0F, 1.0F},
                              {-1.0F, 0.0F, 0.0F},
                              {0.0F, -1.0F, 0.0F},
                              {0.0F, 0.0F, -1.0F}};
            mesh.indices = {0U, 2U, 1U, 0U, 1U, 3U, 0U, 3U, 2U, 1U, 2U, 3U,
                            0U, 5U, 4U, 0U, 4U, 6U, 0U, 6U, 5U, 4U, 5U, 6U};
            return mesh;
        }

        [[nodiscard]] std::string firstDegenerateTriangleDetails(io::IndexedTriangleMesh const & mesh)
        {
            double const floatEpsilon = std::numeric_limits<float>::epsilon();
            double const relativeAreaTolerance = floatEpsilon * floatEpsilon;
            for (std::size_t offset = 0U; offset + 2U < mesh.indices.size(); offset += 3U)
            {
                auto const first = mesh.indices[offset];
                auto const second = mesh.indices[offset + 1U];
                auto const third = mesh.indices[offset + 2U];
                auto const & a = mesh.positions[first];
                auto const & b = mesh.positions[second];
                auto const & c = mesh.positions[third];
                double const abx = static_cast<double>(b.x()) - a.x();
                double const aby = static_cast<double>(b.y()) - a.y();
                double const abz = static_cast<double>(b.z()) - a.z();
                double const acx = static_cast<double>(c.x()) - a.x();
                double const acy = static_cast<double>(c.y()) - a.y();
                double const acz = static_cast<double>(c.z()) - a.z();
                double const bcx = static_cast<double>(c.x()) - b.x();
                double const bcy = static_cast<double>(c.y()) - b.y();
                double const bcz = static_cast<double>(c.z()) - b.z();
                double const abSquared = abx * abx + aby * aby + abz * abz;
                double const acSquared = acx * acx + acy * acy + acz * acz;
                double const bcSquared = bcx * bcx + bcy * bcy + bcz * bcz;
                double const maxEdgeSquared = std::max({abSquared, acSquared, bcSquared});
                double const crossX = aby * acz - abz * acy;
                double const crossY = abz * acx - abx * acz;
                double const crossZ = abx * acy - aby * acx;
                double const crossSquared = crossX * crossX + crossY * crossY + crossZ * crossZ;
                if (maxEdgeSquared != 0.0 &&
                    crossSquared > maxEdgeSquared * maxEdgeSquared * relativeAreaTolerance)
                {
                    continue;
                }

                std::ostringstream detail;
                detail << std::scientific << std::setprecision(9)
                       << "triangle=" << offset / 3U << " indices=(" << first << ',' << second << ',' << third
                       << ") positions=(" << a.transpose() << "; " << b.transpose() << "; " << c.transpose()
                       << ") relativeArea=" << (maxEdgeSquared == 0.0 ? 0.0 : crossSquared / (maxEdgeSquared * maxEdgeSquared));
                return detail.str();
            }
            return "No degenerate triangle found while collecting diagnostic details";
        }

        void exportAndCheckTopology(Document & document,
                                    io::MeshExportRequest request,
                                    std::filesystem::path const & outputPath,
                                    bool const hasColors)
        {
            request.filename = outputPath;
            std::string lastPhase{"request preparation"};
            try
            {
                document.exportMesh(
                  request,
                  {},
                  [&lastPhase](double, std::string_view const phase) { lastPhase.assign(phase); });
            }
            catch (std::exception const & error)
            {
                FAIL() << "WebGPU export failed during '" << lastPhase << "': " << error.what();
                return;
            }

            ASSERT_TRUE(std::filesystem::exists(outputPath));
            ASSERT_GT(std::filesystem::file_size(outputPath), 0U);
            io::IndexedTriangleMesh mesh;
            ASSERT_NO_THROW(mesh = readIndexedMesh(outputPath));
            if (auto const validationError = mesh.validationError())
            {
                FAIL() << *validationError << "; " << firstDegenerateTriangleDetails(mesh);
                return;
            }
            auto const topologyError = mesh.closedOrientedTopologyError();
            ASSERT_FALSE(topologyError.has_value()) << topologyError.value_or("");
            ASSERT_TRUE(hasManifoldVertexLinks(mesh))
              << "Exported mesh has a non-manifold or disconnected vertex link";

            if (hasColors)
            {
                expectOrcaAttributes(outputPath, mesh.indices.size() / 3U);
            }
        }

        [[nodiscard]] io::MeshExportRequest makeLampshadeRequest(bool const exportWithColors)
        {
            auto request = makeUiEquivalentRequest(exportWithColors);
            auto & options = request.extraction.manifoldDualContouring;
            options.qualityPreset = io::ManifoldDualContouringQuality::Balanced;
            options.applyPreset();
            if (!exportWithColors)
            {
                options.simplificationMethod = io::SimplificationMethod::None;
                options.enableSimplification = false;
            }
            return request;
        }

        void checkLampshadeBalancedExport(bool const exportWithColors)
        {
            if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
            {
                GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
            }

            auto runtime = compute::ApplicationComputeRuntime::createWebGPU();
            ASSERT_TRUE(runtime->isAvailable()) << runtime->getErrorMessage();

            auto const logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
            Document document{logger};
            document.setBackendRuntime(runtime.get());
            std::filesystem::path const fixture{"testdata/lampshade.3mf"};
            ASSERT_TRUE(std::filesystem::exists(fixture)) << fixture;
            ASSERT_NO_THROW(document.load(fixture));
            if (exportWithColors)
            {
                ASSERT_TRUE(hasVolumetricColor(document));
            }

            Temporary3mf output;
            auto const request = makeLampshadeRequest(exportWithColors);
            ASSERT_EQ(request.extraction.manifoldDualContouring.initialDepth, 5U);
            ASSERT_EQ(request.extraction.manifoldDualContouring.maxDepth, 7U);

            SCOPED_TRACE("fixture=" + fixture.string());
            ASSERT_NO_FATAL_FAILURE(exportAndCheckTopology(document, request, output.path(), exportWithColors));
            EXPECT_TRUE(std::any_of(logger->cbegin(), logger->cend(), [](events::Event const & event)
            {
                return event.getSeverity() == events::Severity::Warning &&
                       event.getMessage().find("Retrying WebGPU manifold extraction at depth 8") != std::string::npos;
            })) << "The Balanced fixture must exercise the finer-depth recovery path";
        }
    } // namespace

    TEST(WebGPUMeshExportTopology, ClosedTetrahedron_HasManifoldVertexLinks)
    {
        auto mesh = makePinchedTetrahedra();
        mesh.positions.resize(4U);
        mesh.indices.resize(12U);

        ASSERT_FALSE(mesh.closedOrientedTopologyError().has_value());
        EXPECT_TRUE(hasManifoldVertexLinks(mesh));
    }

    TEST(WebGPUMeshExportTopology, PinchedClosedTetrahedra_HasDisconnectedVertexLink)
    {
        auto const mesh = makePinchedTetrahedra();

        ASSERT_FALSE(mesh.closedOrientedTopologyError().has_value());
        EXPECT_FALSE(hasManifoldVertexLinks(mesh));
    }

    TEST(WebGPUMeshExport, Lampshade_ManifoldBalanced_ProducesValid3mfTopology)
    {
        checkLampshadeBalancedExport(false);
    }

    TEST(WebGPUMeshExport, Lampshade_ManifoldBalancedOrcaFastQem_ProducesValid3mfTopology)
    {
        checkLampshadeBalancedExport(true);
    }

    TEST(WebGPUMeshExport, Lampshade_CancelledBeforeFinerRetry_ProducesNoFile)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        auto runtime = compute::ApplicationComputeRuntime::createWebGPU();
        ASSERT_TRUE(runtime->isAvailable()) << runtime->getErrorMessage();

        auto const logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
        Document document{logger};
        document.setBackendRuntime(runtime.get());
        std::filesystem::path const fixture{"testdata/lampshade.3mf"};
        ASSERT_TRUE(std::filesystem::exists(fixture)) << fixture;
        ASSERT_NO_THROW(document.load(fixture));

        Temporary3mf output;
        auto request = makeLampshadeRequest(false);
        request.filename = output.path();
        auto const shouldCancel = [&logger]
        {
            return std::any_of(logger->cbegin(), logger->cend(), [](events::Event const & event)
            {
                return event.getMessage().find("Retrying WebGPU manifold extraction") != std::string::npos;
            });
        };

        EXPECT_THROW(document.exportMesh(request, shouldCancel), io::MeshExportCancelled);
        EXPECT_FALSE(std::filesystem::exists(output.path()));
    }

    TEST(WebGPUMeshExport, Lampbase_OrcaPerVertexFastQem_ProducesValid3mfTopology)
    {
        if (std::getenv("GLADIUS_RUN_WEBGPU_TESTS") == nullptr)
        {
            GTEST_SKIP() << "WebGPU tests disabled; set GLADIUS_RUN_WEBGPU_TESTS=1 to enable";
        }

        std::unique_ptr<compute::IBackendRuntime> runtime;
        try
        {
            runtime = compute::ApplicationComputeRuntime::createWebGPU();
        }
        catch (std::exception const & error)
        {
            GTEST_SKIP() << "WebGPU runtime unavailable: " << error.what();
        }
        if (!runtime->isAvailable())
        {
            GTEST_SKIP() << "WebGPU runtime unavailable: " << runtime->getErrorMessage();
        }

        auto const logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
        Document document{logger};
        document.setBackendRuntime(runtime.get());
        std::filesystem::path fixture{"testdata/lampbase.3mf"};
        ASSERT_TRUE(std::filesystem::exists(fixture)) << fixture;
        ASSERT_NO_THROW(document.load(fixture));

        if (!hasVolumetricColor(document))
        {
            GTEST_SKIP() << "lampbase.3mf has no volumetric color output; the Orca color path "
                            "requires a colored fixture";
        }

        Temporary3mf output;
        auto const request = makeUiEquivalentRequest(true);
        SCOPED_TRACE("fixture=" + fixture.string());
        exportAndCheckTopology(document, request, output.path(), true);
    }
} // namespace gladius::tests

#endif
