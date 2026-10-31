#include "io/3mf/PreparedDiscreteColorExport.h"
#include "io/3mf/ColorRegionizer.h"
#include "io/3mf/IndexedTriangleMesh3mfWriter.h"
#include "io/3mf/Lib3mfLoader.h"
#include "io/3mf/MmuSegmentationWriter.h"
#if defined(GLADIUS_ENABLE_OPENCL)
#include "io/3mf/ColorExportDispatcher.h"
#include "io/3mf/MeshWriter3mf.h"
#include "ComputeContext.h"
#include "Mesh.h"
#endif

#include <gtest/gtest.h>
#include <minizip/unzip.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>

namespace gladius::io::tests
{
    namespace
    {
        IndexedTriangleMesh makeColorTestMesh()
        {
            IndexedTriangleMesh mesh;
            mesh.positions = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
            mesh.indices = {0, 2, 4, 4, 2, 1, 1, 2, 5, 5, 2, 0,
                            4, 3, 0, 1, 3, 4, 5, 3, 1, 0, 3, 5};
            for (int level = 0; level < 3; ++level)
            {
                std::map<std::pair<std::uint32_t, std::uint32_t>, std::uint32_t> midpoints;
                auto midpoint = [&](std::uint32_t a, std::uint32_t b)
                {
                    auto const key = std::minmax(a, b);
                    if (auto const found = midpoints.find(key); found != midpoints.end()) return found->second;
                    auto const id = static_cast<std::uint32_t>(mesh.positions.size());
                    Eigen::Vector3f const position = (mesh.positions[a] + mesh.positions[b]).normalized();
                    mesh.positions.push_back(position);
                    midpoints.emplace(key, id);
                    return id;
                };
                std::vector<std::uint32_t> indices;
                for (std::size_t t = 0; t < mesh.indices.size(); t += 3U)
                {
                    auto const a = mesh.indices[t];
                    auto const b = mesh.indices[t + 1U];
                    auto const c = mesh.indices[t + 2U];
                    auto const ab = midpoint(a, b);
                    auto const bc = midpoint(b, c);
                    auto const ca = midpoint(c, a);
                    indices.insert(indices.end(), {a, ab, ca, ab, b, bc, ca, bc, c, ab, bc, ca});
                }
                mesh.indices = std::move(indices);
            }
            return mesh;
        }

        FaceColors sampleFixtureColors(IndexedTriangleMesh const & mesh)
        {
            FaceColors colors(mesh.indices.size() / 3U);
            for (std::size_t t = 0; t < colors.size(); ++t)
            {
                float const y = mesh.positions[mesh.indices[t * 3U]].y() +
                                mesh.positions[mesh.indices[t * 3U + 1U]].y() +
                                mesh.positions[mesh.indices[t * 3U + 2U]].y();
                colors[t] = y > 0.0F ? Color8{255, 0, 0} : Color8{0, 0, 255};
            }
            return colors;
        }

        std::string readModelXml(std::filesystem::path const & file)
        {
            auto archive = unzOpen64(file.string().c_str());
            if (archive == nullptr) throw std::runtime_error("Cannot open test 3MF ZIP");
            auto const closeArchive = [](void * value) { unzClose(value); };
            std::unique_ptr<void, decltype(closeArchive)> cleanup(archive, closeArchive);
            if (unzLocateFile(archive, "3D/3dmodel.model", 0) != UNZ_OK || unzOpenCurrentFile(archive) != UNZ_OK)
            {
                throw std::runtime_error("Cannot locate test model XML");
            }
            std::string xml;
            std::array<char, 4096> buffer;
            int count;
            while ((count = unzReadCurrentFile(archive, buffer.data(), static_cast<unsigned>(buffer.size()))) > 0)
            {
                xml.append(buffer.data(), static_cast<std::size_t>(count));
            }
            unzCloseCurrentFile(archive);
            if (count < 0) throw std::runtime_error("Cannot read test model XML");
            return xml;
        }

        void expectMmuFaceOrder(std::filesystem::path const & file, QuantizedPalette const & palette)
        {
            auto const xml = readModelXml(file);
            std::size_t position = 0U;
            for (auto const material : palette.sourceToPaletteMap)
            {
                auto const start = xml.find("<triangle ", position);
                ASSERT_NE(start, std::string::npos);
                auto const end = xml.find('>', start);
                ASSERT_NE(end, std::string::npos);
                auto const triangle = xml.substr(start, end - start);
                auto const encoded = MmuSegmentationWriter::encodeExtruderState(material + 1U);
                EXPECT_NE(triangle.find("paint_color=\"" + encoded + "\""), std::string::npos);
                EXPECT_NE(triangle.find("slic3rpe:mmu_segmentation=\"" + encoded + "\""), std::string::npos);
                position = end + 1U;
            }
            EXPECT_EQ(xml.find("<triangle ", position), std::string::npos);
        }

        void expectPortableRegions(std::filesystem::path const & file, QuantizedPalette const & palette)
        {
            auto const wrapper = loadLib3mfScoped();
            auto const model = wrapper->CreateModel();
            model->QueryReader("3mf")->ReadFromFile(file.string());
            auto const regions = ColorRegionizer::regionize(palette, PrintableRegionKind::Component);
            EXPECT_EQ(model->GetBuildItems()->Count(), regions.size());
            auto const objects = model->GetMeshObjects();
            for (auto const & region : regions)
            {
                ASSERT_TRUE(objects->MoveNext());
                auto const object = objects->GetCurrentMeshObject();
                EXPECT_EQ(object->GetTriangleCount(), region.triangleIndices.size());
                std::vector<Lib3MF::sTriangleProperties> properties;
                object->GetAllTriangleProperties(properties);
                auto const materials = model->GetBaseMaterialGroups();
                ASSERT_TRUE(materials->MoveNext());
                auto const group = materials->GetCurrentBaseMaterialGroup();
                for (auto const & property : properties)
                {
                    auto const color = group->GetDisplayColor(property.m_PropertyIDs[0]);
                    auto const expected = palette.colors[region.paletteIndex];
                    EXPECT_EQ(color.m_Red, expected.r);
                    EXPECT_EQ(color.m_Green, expected.g);
                    EXPECT_EQ(color.m_Blue, expected.b);
                }
            }
            EXPECT_FALSE(objects->MoveNext());
            EXPECT_EQ(readModelXml(file).find("paint_color="), std::string::npos);
        }
    }

    TEST(PreparedDiscreteColorExport, Prepare_OversampledFace_VotesAgainstFixedPalette)
    {
        MeshColorExportSettings settings;
        settings.targetApplication = TargetApplication::PrusaSlicer;
        FaceColors centroid(std::vector<Color8>{{255, 0, 0}, {0, 0, 255}});
        FaceColors edges(std::vector<Color8>{{0, 0, 255}, {0, 0, 255}});

        auto const prepared = prepareDiscreteColorExport(settings, centroid, {centroid, edges, edges, edges});

        ASSERT_EQ(prepared.palette.colors.size(), 2U);
        EXPECT_EQ(prepared.palette.sourceToPaletteMap[0], prepared.palette.sourceToPaletteMap[1]);
        EXPECT_EQ(prepared.palette.colors[prepared.palette.sourceToPaletteMap[0]], (Color8{0, 0, 255}));
    }

    TEST(PreparedDiscreteColorExport, Prepare_PaletteCapacity_ClampsOverridesAndWarns)
    {
        MeshColorExportSettings settings;
        settings.targetApplication = TargetApplication::Orca;
        settings.maxPaletteSize = 1000U;
        FaceColors colors;
        for (std::uint8_t r = 0; r < 20; ++r) colors.colors.emplace_back(r, 0, 0);

        auto const prepared = prepareDiscreteColorExport(settings, colors);

        EXPECT_LE(prepared.palette.colors.size(), 16U);
        EXPECT_TRUE(prepared.decision.needsQuantization);
        EXPECT_FALSE(prepared.decision.warnings.empty());
        settings.maxPaletteSize = 0U;
        EXPECT_EQ(resolveDiscretePaletteLimit(settings, 20U, true), 1U);
        settings.maxPaletteSize = 1000U;
        EXPECT_EQ(resolveDiscretePaletteLimit(settings, 500U, false), 256U);
    }

    TEST(PreparedDiscreteColorExport, Prepare_InvalidSampleCounts_Throws)
    {
        FaceColors colors(2U);
        EXPECT_THROW((void) prepareDiscreteColorExport({}, colors, {FaceColors(1U)}), std::invalid_argument);
        EXPECT_THROW((void) prepareDiscreteColorExport({}, FaceColors{}), std::invalid_argument);
        QuantizedPalette palette;
        palette.colors = {{255, 0, 0}};
        palette.sourceToPaletteMap = {1U};
        EXPECT_THROW(validateDiscretePalette(palette, 1U), std::invalid_argument);
        EXPECT_THROW(validateDiscretePalette(palette, 2U), std::invalid_argument);
    }

    TEST(PreparedDiscreteColorExport, Extract_FastAndLegacySettings_DisablesBothFlagsWithoutChangingRequest)
    {
        for (auto const method : {SimplificationMethod::None, SimplificationMethod::QemFast, SimplificationMethod::QemSdfAware})
        {
            ManifoldDualContouringOptions requested;
            requested.simplificationMethod = method;
            requested.enableSimplification = true;
            requested.projectToSurface = true;
            requested.simplificationTargetTriangles = 100U;

            auto const extraction = makeUnsimplifiedColorExtractionOptions(requested);

            EXPECT_EQ(extraction.simplificationMethod, SimplificationMethod::None);
            EXPECT_FALSE(extraction.enableSimplification);
            EXPECT_TRUE(extraction.projectToSurface);
            EXPECT_EQ(extraction.simplificationTargetTriangles, requested.simplificationTargetTriangles);
            EXPECT_EQ(requested.simplificationMethod, method);
            EXPECT_TRUE(requested.enableSimplification);
        }
    }

    TEST(PreparedDiscreteColorExport, Simplify_SdfAwareAndLegacy_WarnsWithoutChangingColorsOrGeometry)
    {
        for (auto const method : {SimplificationMethod::None, SimplificationMethod::QemSdfAware})
        {
            auto mesh = makeColorTestMesh();
            auto const indices = mesh.indices;
            auto prepared = prepareDiscreteColorExport({}, sampleFixtureColors(mesh));
            auto const map = prepared.palette.sourceToPaletteMap;
            ManifoldDualContouringOptions options;
            options.simplificationMethod = method;
            options.enableSimplification = true;

            EXPECT_TRUE(simplifyColoredMesh(mesh, prepared.palette, options).has_value());

            EXPECT_EQ(mesh.indices, indices);
            EXPECT_EQ(prepared.palette.sourceToPaletteMap, map);
        }
    }

    TEST(PreparedDiscreteColorExport, Simplify_FastQem_KeepsPaletteAndClearsStaleNormals)
    {
        auto mesh = makeColorTestMesh();
        mesh.normals.resize(mesh.positions.size(), Eigen::Vector3f{0, 1, 0});
        auto prepared = prepareDiscreteColorExport({}, sampleFixtureColors(mesh));
        auto const palette = prepared.palette.colors;
        auto const count = mesh.indices.size();
        ManifoldDualContouringOptions options;
        options.simplificationMethod = SimplificationMethod::QemFast;

        (void) simplifyColoredMesh(mesh, prepared.palette, options);

        EXPECT_EQ(prepared.palette.colors, palette);
        EXPECT_LT(mesh.indices.size(), count);
        EXPECT_TRUE(mesh.normals.empty());
        EXPECT_FALSE(mesh.closedOrientedTopologyError().has_value());
        EXPECT_EQ(prepared.palette.sourceToPaletteMap.size(), mesh.indices.size() / 3U);
    }

    TEST(PreparedDiscreteColorExport, Simplify_ImpossibleTarget_WarnsInsteadOfCrossingBoundaries)
    {
        auto mesh = makeColorTestMesh();
        auto prepared = prepareDiscreteColorExport({}, sampleFixtureColors(mesh));
        ManifoldDualContouringOptions options;
        options.simplificationMethod = SimplificationMethod::QemFast;
        options.simplificationTerminationMode = SimplificationTerminationMode::TargetTriangleCount;
        options.simplificationTargetTriangles = 1U;

        auto const warning = simplifyColoredMesh(mesh, prepared.palette, options);

        ASSERT_TRUE(warning.has_value());
        EXPECT_NE(warning->find("material boundaries"), std::string::npos);
        EXPECT_GT(mesh.indices.size() / 3U, 1U);
        EXPECT_FALSE(mesh.closedOrientedTopologyError().has_value());
    }

    class PreparedColorWriterTest : public ::testing::TestWithParam<TargetApplication>
    {
      protected:
        void SetUp() override
        {
            auto const unique = std::chrono::steady_clock::now().time_since_epoch().count();
            m_directory = std::filesystem::temp_directory_path() / ("gladius_prepared_color_" + std::to_string(unique));
            std::filesystem::create_directories(m_directory);
        }
        void TearDown() override { std::filesystem::remove_all(m_directory); }
        std::filesystem::path m_directory;
    };

    TEST_P(PreparedColorWriterTest, Export_SimplifiedIndexedMesh_PreservesEveryMaterialAssignment)
    {
        auto mesh = makeColorTestMesh();
        MeshColorExportSettings settings;
        settings.targetApplication = GetParam();
        auto prepared = prepareDiscreteColorExport(settings, sampleFixtureColors(mesh));
        auto const palette = prepared.palette.colors;
        auto const initialCount = mesh.indices.size();
        ManifoldDualContouringOptions options;
        options.simplificationMethod = SimplificationMethod::QemFast;
        (void) simplifyColoredMesh(mesh, prepared.palette, options);
        ASSERT_LT(mesh.indices.size(), initialCount);
        auto const file = m_directory / "indexed.3mf";
        IndexedTriangleMesh3mfWriter writer;

        if (GetParam() == TargetApplication::None)
        {
            auto const regions = ColorRegionizer::regionize(prepared.palette, PrintableRegionKind::Component);
            writer.exportMeshWithRegions(file, mesh, "Prepared", prepared.palette, regions);
            expectPortableRegions(file, prepared.palette);
        }
        else
        {
            writer.exportMeshWithMmuSegmentation(file, mesh, "Prepared", prepared.palette);
            expectMmuFaceOrder(file, prepared.palette);
        }
        EXPECT_EQ(prepared.palette.colors, palette);
    }

#if defined(GLADIUS_ENABLE_OPENCL)
    TEST_P(PreparedColorWriterTest, Export_SimplifiedSoup_DispatchesFrozenPaletteWithoutSampling)
    {
        auto indexed = makeColorTestMesh();
        MeshColorExportSettings settings;
        settings.targetApplication = GetParam();
        auto prepared = prepareDiscreteColorExport(settings, sampleFixtureColors(indexed));
        auto const colors = prepared.palette.colors;
        ManifoldDualContouringOptions options;
        options.simplificationMethod = SimplificationMethod::QemFast;
        (void) simplifyColoredMesh(indexed, prepared.palette, options);
        ComputeContext context;
        Mesh soup(context);
        for (std::size_t t = 0; t < indexed.indices.size(); t += 3U)
        {
            soup.addFace(indexed.positions[indexed.indices[t]], indexed.positions[indexed.indices[t + 1U]],
                         indexed.positions[indexed.indices[t + 2U]]);
        }
        auto const file = m_directory / "soup.3mf";
        MeshWriter3mf writer(nullptr);

        dispatchPreparedColorExport(writer, file, soup, "Prepared", prepared, nullptr, false);

        if (GetParam() == TargetApplication::None) expectPortableRegions(file, prepared.palette);
        else expectMmuFaceOrder(file, prepared.palette);
        EXPECT_EQ(prepared.palette.colors, colors);
    }

    TEST(PreparedDiscreteColorExport, Export_FilteredSoupTriangles_RejectsMissingPortableFacesBeforeWriting)
    {
        ComputeContext context;
        Mesh soup(context);
        soup.addFace(Vector3{0, 0, 0}, Vector3{1, 0, 0}, Vector3{0, 1, 0});
        soup.addFace(Vector3{0, 0, 0}, Vector3{1e-8F, 0, 0}, Vector3{0, 1e-8F, 0});
        auto const prepared = prepareDiscreteColorExport({}, FaceColors(std::vector<Color8>{{255, 0, 0}, {0, 0, 255}}));
        auto const file = std::filesystem::temp_directory_path() /
                          ("gladius_filtered_portable_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".3mf");
        MeshWriter3mf writer(nullptr);

        EXPECT_THROW(dispatchPreparedColorExport(writer, file, soup, "Filtered", prepared, nullptr, false), std::runtime_error);

        EXPECT_FALSE(std::filesystem::exists(file));
    }

    TEST(PreparedDiscreteColorExport, Export_FilteredSoupTriangles_RejectsMisalignedMmuBeforeWriting)
    {
        ComputeContext context;
        Mesh soup(context);
        soup.addFace(Vector3{0, 0, 0}, Vector3{1, 0, 0}, Vector3{0, 1, 0});
        soup.addFace(Vector3{0, 0, 0}, Vector3{1e-8F, 0, 0}, Vector3{0, 1e-8F, 0});
        QuantizedPalette palette;
        palette.colors = {{255, 0, 0}, {0, 0, 255}};
        palette.sourceToPaletteMap = {0, 1};
        auto const file = std::filesystem::temp_directory_path() /
                          ("gladius_filtered_mmu_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".3mf");
        MeshWriter3mf writer(nullptr);

        EXPECT_THROW(writer.exportMeshWithMmuSegmentation(file, soup, "Filtered", palette), std::runtime_error);

        EXPECT_FALSE(std::filesystem::exists(file));
    }
#endif

    INSTANTIATE_TEST_SUITE_P(SlicerTargets, PreparedColorWriterTest,
                            ::testing::Values(TargetApplication::None, TargetApplication::PrusaSlicer, TargetApplication::Orca));
}