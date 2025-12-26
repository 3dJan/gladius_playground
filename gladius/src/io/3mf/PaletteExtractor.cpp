#include "PaletteExtractor.h"

#include "io/3mf/FaceColorSampler.h"
#include "io/3mf/FaceColors.h"

#include "compute/ManifoldDualContouringGpu.h"
#include "compute/ProgramManager.h"
#include "compute/ComputeCore.h"

#include <DualContouringSamplingProgram.h>

#include <stdexcept>
#include <set>
#include <tuple>
#include <array>

namespace gladius::io
{
    std::vector<Eigen::Vector3f> derivePaletteFromMesh(gladius::ComputeCore & core,
                                                       PaletteExtractionOptions const & options)
    {
        if (!core.updateBBox())
        {
            throw std::runtime_error("Bounding box update failed before palette derivation");
        }

        auto const bbox = core.getBoundingBox();
        if (!bbox.has_value())
        {
            throw std::runtime_error("Mesh generation failed, bounding box is empty");
        }

        // Build GPU config from manifold options
        compute::ManifoldDualContouringConfig config{};
        auto const & opt = options.manifoldOptions;
        config.initialDepth = opt.initialDepth;
        config.maxDepth = opt.maxDepth;
        config.enableGpu = opt.enableGpu;
        config.enableCpuFallback = opt.enableCpuFallback;
        config.enableCaching = opt.enableCaching;
        config.isoValue = opt.isoValue;
        config.minFeatureSize = opt.minFeatureSize;
        config.enableChunking = opt.enableChunking;
        config.enableHierarchicalOctree = opt.enableHierarchicalOctree;
        config.enableSharpFeaturePostProcess = opt.enableSharpFeaturePostProcess;
        config.sharpFeatureAngleThreshold = opt.sharpFeatureAngleThreshold;
        config.subdivisionIterations = opt.subdivisionIterations;
        config.projectToSurface = opt.projectToSurface;
        config.simplificationMethod = (opt.simplificationMethod == SimplificationMethod::QemSdfAware)
                                        ? compute::SimplificationMethod::QemSdfAware
                                        : compute::SimplificationMethod::None;
        config.enableSimplification = opt.enableSimplification;
        config.simplificationMaxSdfError = opt.simplificationMaxSdfError;
        config.simplificationMaxQemError = opt.simplificationMaxQemError;
        config.simplificationMaxNormalDeviation = opt.simplificationMaxNormalDeviation;
        config.simplificationSdfWeight = opt.simplificationSdfWeight;
        config.simplificationQemWeight = opt.simplificationQemWeight;
        config.simplificationNormalWeight = opt.simplificationNormalWeight;
        config.simplificationSharpEdgeThreshold = opt.simplificationSharpEdgeThreshold;
        config.simplificationBatchSize = opt.simplificationBatchSize;
        config.simplificationMaxPasses = opt.simplificationMaxPasses;
        config.simplificationTargetTriangles = opt.simplificationTargetTriangles;
        config.simplificationTargetReduction = opt.simplificationTargetReduction;

        compute::ManifoldDualContouringGpu pipeline(core);
        pipeline.setConfig(config);
        pipeline.generateMesh();
        auto const & mesh = pipeline.getMesh();
        if (mesh.indices.empty() || mesh.positions.empty())
        {
            throw std::runtime_error("Mesh extraction produced no geometry");
        }

        std::vector<std::array<std::uint32_t, 3>> faces;
        faces.reserve(mesh.indices.size() / 3);
        for (std::size_t i = 0; i + 2 < mesh.indices.size(); i += 3)
        {
            faces.push_back({mesh.indices[i + 0], mesh.indices[i + 1], mesh.indices[i + 2]});
        }

        auto * samplingProgram = core.getProgramManager().getDualContouringSamplingProgram();
        if (samplingProgram == nullptr)
        {
            throw std::runtime_error("No sampling program available for palette derivation");
        }
        auto primitives = core.getPrimitives();
        if (primitives == nullptr)
        {
            throw std::runtime_error("No primitives available for palette derivation");
        }

        auto vertexColors = FaceColorSampler::sampleVertexColors(
          mesh.positions, faces, *samplingProgram, *primitives, nullptr, options.convertToSrgb);

        std::set<std::tuple<std::uint8_t, std::uint8_t, std::uint8_t>> unique;
        for (auto const & face : vertexColors.faceVertexColors)
        {
            for (auto const & c : face.colors)
            {
                unique.emplace(c.r, c.g, c.b);
            }
        }

        std::vector<Eigen::Vector3f> palette;
        palette.reserve(unique.size());
        for (auto const & [r, g, b] : unique)
        {
            palette.emplace_back(static_cast<float>(r) / 255.0F,
                                 static_cast<float>(g) / 255.0F,
                                 static_cast<float>(b) / 255.0F);
        }

        if (palette.empty())
        {
            throw std::runtime_error("No colors found in extracted mesh");
        }

        return palette;
    }
}
