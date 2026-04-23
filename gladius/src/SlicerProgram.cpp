#include "SlicerProgram.h"
#include "ProgramBase.h"
#include "gpgpu.h"

#include <algorithm>
#include <utility>

#include "Mesh.h"
#include "Profiling.h"

namespace gladius
{

#define PAYLOAD_ARGUMENTS                                                                          \
    m_resources->getBuildArea(), lines.primitives.getBuffer(),                                      \
      static_cast<cl_uint>(lines.primitives.getSize()), lines.data.getBuffer(),                    \
      static_cast<cl_uint>(lines.data.getSize()), m_resources->getRenderingSettings(),              \
      m_resources->getPrecompSdfBuffer().getBuffer(), m_resources->getParameterBuffer().getBuffer(), \
      m_resources->getCommandBuffer().getBuffer(),                                                  \
      static_cast<cl_int>(m_resources->getCommandBuffer().getData().size()),                        \
      m_resources->getPreCompSdfBBox()

#define PAYLOAD_ARGUMENTS_CLIPPING_AREA                                                            \
    m_resources->getClippingArea(), lines.primitives.getBuffer(),                                   \
      static_cast<cl_uint>(lines.primitives.getSize()), lines.data.getBuffer(),                    \
      static_cast<cl_uint>(lines.data.getSize()), m_resources->getRenderingSettings(),              \
      m_resources->getPrecompSdfBuffer().getBuffer(), m_resources->getParameterBuffer().getBuffer(), \
      m_resources->getCommandBuffer().getBuffer(),                                                  \
      static_cast<cl_int>(m_resources->getCommandBuffer().getData().size()),                        \
      m_resources->getPreCompSdfBBox()

    gladius::SlicerProgram::SlicerProgram(SharedComputeContext context,
                                          const SharedResources & resources)
        : ProgramBase(context, resources)
    {
        m_sourceFiles = {"types.h",
                         "arguments.h",
                         "sdf.h",
                         "sampler.h",
                         "rendering.h",
                         "sdf_generator.h",
                         "PNanoVDB_OpenCL.h",
                         "PNanoVDB.h",
                         "PNanoVDB_OpenCL_Helpers.h",
                         "mesh_sdf.cl",
                         "sdf.cl",
                         "rendering.cl",
                         "distanceUpDown.cl",
                         "sdf_generator.cl"};
    }

    void SlicerProgram::readBuffer() const
    {

        m_resources->getContourVertexPos().read();
    }

    void SlicerProgram::renderFirstLayer(const Primitives & lines, cl_float isoValue, cl_float z_mm)
    {
        ProfileFunction;
        swapProgramsIfNeeded();
        const auto res = m_resources->getMipMapResolutions().front();
        const cl_float branchThreshold = determineBranchThreshold(res, isoValue);

        const cl::NDRange origin = {0, 0, 0};
        const cl::NDRange globalRange = {m_resources->getDistanceMipMaps().front()->getWidth(),
                                         m_resources->getDistanceMipMaps().front()->getHeight(),
                                         1u};

        m_programFront->run("renderSDFFirstLayer",
                            origin,
                            globalRange,
                            m_resources->getDistanceMipMaps().front()->getBuffer(), // 0
                            branchThreshold,                                       // 1
                            PAYLOAD_ARGUMENTS_CLIPPING_AREA,
                            z_mm); // 12
    }

    void SlicerProgram::renderLayers(const Primitives & lines, cl_float isoValue, cl_float z_mm)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        ProfileFunction;
        m_resources->getRenderingSettings().approximation = AM_HYBRID;
        swapProgramsIfNeeded();
        renderFirstLayer(lines, isoValue, z_mm);

        auto resIt = std::begin(m_resources->getMipMapResolutions());
        auto distMapIt = std::begin(m_resources->getDistanceMipMaps());

        for (++resIt, ++distMapIt; resIt != std::end(m_resources->getMipMapResolutions()) &&
                                   distMapIt != std::end(m_resources->getDistanceMipMaps());
             ++resIt, ++distMapIt)
        {
            const auto res = *resIt;
            const cl_float branchThreshold = determineBranchThreshold(res, isoValue);

            const auto previousLayer = (distMapIt - 1)->get();
            const cl::NDRange origin = {0, 0, 0};
            const cl::NDRange globalRange = {
              (*distMapIt)->getWidth(), (*distMapIt)->getHeight(), 1u};

            m_programFront->run("renderSDFLayer",
                                origin,
                                globalRange,
                                distMapIt->get()->getBuffer(), // 0
                                previousLayer->getBuffer(),    // 1
                                branchThreshold,               // 2
                                PAYLOAD_ARGUMENTS_CLIPPING_AREA,
                                z_mm); // 13
        }

        auto & lastLayer = *m_resources->getDistanceMipMaps().back();
        lastLayer.read();
    }

    cl_float SlicerProgram::determineBranchThreshold(const cl_int2 & res, cl_float isoValue) const
    {
        ProfileFunction;
        const auto buildAreaWidth =
          m_resources->getClippingArea().z - m_resources->getClippingArea().x;
        const auto buildAreaHeight =
          m_resources->getClippingArea().w - m_resources->getClippingArea().y;
        const auto maxSize = std::max(buildAreaWidth / static_cast<float>(res.x),
                                      buildAreaHeight / static_cast<float>(res.y));
        const auto maxSizeGrid =
          std::max(buildAreaWidth / static_cast<float>(m_resources->getGridSize().x),
                   buildAreaHeight / static_cast<float>(m_resources->getGridSize().y));
        return fabs(isoValue) + std::max(maxSize, maxSizeGrid) * 2.0f;
    }

    void SlicerProgram::renderResultImageReadPixel(DistanceMap & sourceImage,
                                                   GLImageBuffer & targetImage)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        ProfileFunction;
        swapProgramsIfNeeded();
        const cl::NDRange origin = {0, 0, 0};
        const cl::NDRange range = {targetImage.getWidth(), targetImage.getHeight(), 1};
        m_programFront->run(
          "render", origin, range, targetImage.getBuffer(), sourceImage.getBuffer());
    }

    void SlicerProgram::precomputeSdf(const Primitives & lines, BoundingBox boundingBox)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        ProfileFunction;
        swapProgramsIfNeeded();
        auto & target = m_resources->getPrecompSdfBuffer();
        m_resources->getRenderingSettings().approximation = AM_FULL_MODEL;
        const cl::NDRange origin = {0, 0, 0};
        const cl::NDRange range = {target.getWidth(), target.getHeight(), target.getDepth()};
        m_programFront->run(
          "preComputeSdf", origin, range, target.getBuffer(), boundingBox, PAYLOAD_ARGUMENTS);
    }

    cl::Event SlicerProgram::precomputeSdfAsync(const Primitives & lines,
                                                BoundingBox boundingBox,
                                                cl::CommandQueue const & queue)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        ProfileFunction;
        swapProgramsIfNeeded();

        auto & target = m_resources->getPrecompSdfBuffer();
        m_resources->getRenderingSettings().approximation = AM_FULL_MODEL;

        const cl::NDRange origin = {0, 0, 0};
        const cl::NDRange range = {target.getWidth(), target.getHeight(), target.getDepth()};

        // Use runNonBlocking to get the event without blocking
        cl::Event sdfEvent = m_programFront->runNonBlocking(queue,
                                                            "preComputeSdf",
                                                            origin,
                                                            range,
                                                            target.getBuffer(),
                                                            boundingBox,
                                                            PAYLOAD_ARGUMENTS);

        // Flush to ensure the kernel is submitted, but don't wait
        if (sdfEvent())
        {
            queue.flush();
        }

        return sdfEvent;
    }

    void SlicerProgram::calculateNormals(const Primitives & lines, const Mesh & mesh)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        ProfileFunction;
        swapProgramsIfNeeded();
        m_resources->getRenderingSettings().approximation = AM_FULL_MODEL;
        const cl::NDRange origin = {0, 0, 0};
        const cl::NDRange range = {mesh.getNumberOfVertices(), 1, 1};
        m_programFront->run("calculateVertexNormals",
                            origin,
                            range,
                            mesh.getVertices().getBuffer(),
                            mesh.getVertexNormals().getBuffer(),
                            PAYLOAD_ARGUMENTS);
    }

    void SlicerProgram::renderDownSkinDistance(DepthBuffer & targetImage,
                                               Primitives const & lines,
                                               cl_float z_mm)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        ProfileFunction;
        swapProgramsIfNeeded();
        m_resources->getRenderingSettings().approximation = AM_FULL_MODEL;
        const cl::NDRange origin = {0, 0, 0};
        const cl::NDRange range = {targetImage.getWidth(), targetImage.getHeight(), 1};

        m_programFront->run("distanceToBottom",
                            origin,
                            range,
                            targetImage.getBuffer(),
                            PAYLOAD_ARGUMENTS_CLIPPING_AREA);

        targetImage.read();
    }

    void SlicerProgram::renderUpSkinDistance(DepthBuffer & targetImage,
                                             Primitives const & lines,
                                             cl_float z_mm)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        ProfileFunction;
        swapProgramsIfNeeded();
        m_resources->getRenderingSettings().approximation = AM_FULL_MODEL;
        const cl::NDRange origin = {0, 0, 0};
        const cl::NDRange range = {targetImage.getWidth(), targetImage.getHeight(), 1};
        m_programFront->run(
          "distanceToTop", origin, range, targetImage.getBuffer(), PAYLOAD_ARGUMENTS_CLIPPING_AREA);

        targetImage.read();
    }

    void SlicerProgram::movePointsToSurface(Primitives const & lines,
                                            VertexBuffer & input,
                                            VertexBuffer & output)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        ProfileFunction;
        swapProgramsIfNeeded();
        if (!isValid())
        {
            throw std::runtime_error("Internal error (movePointsToSurface): Program is not valid");
        }
        if (input.getSize() != output.getSize())
        {
            throw std::runtime_error("Internal error (movePointsToSurface): Input and Output "
                                     "buffer need to have the same size");
        }

        input.write();
        output.write();

        m_resources->getRenderingSettings().approximation = AM_FULL_MODEL;
        const cl::NDRange origin = {0, 0, 0};
        const cl::NDRange range = {output.getSize(), 1, 1};
        m_programFront->run("movePointsToSurface",
                            origin,
                            range,
                            input.getBuffer(),
                            output.getBuffer(),
                            PAYLOAD_ARGUMENTS);
        output.read();
    }

    void SlicerProgram::adoptVertexOfMeshToSurface(Primitives const & lines,
                                                   VertexBuffer & input,
                                                   VertexBuffer & output)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        ProfileFunction;
        swapProgramsIfNeeded();

        if (input.getSize() != output.getSize())
        {
            throw std::runtime_error("Internal error (movePointsToSurface): Input and Output "
                                     "buffer need to have the same size");
        }

        input.write();
        output.write();

        m_resources->getRenderingSettings().approximation = AM_FULL_MODEL;
        const cl::NDRange origin = {0, 0, 0};
        const cl::NDRange range = {output.getSize(), 1, 1};
        m_programFront->run("adoptVertexOfMeshToSurface",
                            origin,
                            range,
                            input.getBuffer(),
                            output.getBuffer(),
                            PAYLOAD_ARGUMENTS);
        output.read();
    }

    void SlicerProgram::computeMarchingSquareState(const Primitives & lines, cl_float z_mm)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        ProfileFunction;
        swapProgramsIfNeeded();
        const cl::NDRange origin = {0, 0, 0};
        const cl::NDRange globalRange = {m_resources->getMarchingSquareStates().getWidth(),
                                         m_resources->getMarchingSquareStates().getHeight(),
                                         1};
        /*   m_resources->getRenderingSettings().approximation =
             static_cast<ApproximationMode>(AM_HYBRID | AM_DISABLE_INTERPOLATION);*/
        m_resources->getRenderingSettings().approximation = AM_FULL_MODEL;
        m_programFront->run("computeMarchingSquareStates",
                            origin,
                            globalRange,
                            m_resources->getMarchingSquareStates().getBuffer(), // 0
                            z_mm,
                            PAYLOAD_ARGUMENTS_CLIPPING_AREA);

        m_resources->getMarchingSquareStates().read();
    }

    void SlicerProgram::adoptVertexPositions2d(const Primitives & lines,
                                               Vertex2dBuffer & input,
                                               Vertex2dBuffer & output,
                                               cl_float z_mm)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        ProfileFunction;

        swapProgramsIfNeeded();

        if (input.getSize() != output.getSize())
        {
            throw std::runtime_error("Internal error (movePointsToSurface): Input and Output "
                                     "buffer need to have the same size");
        }

        input.write();
        output.write();

        m_resources->getRenderingSettings().approximation = AM_FULL_MODEL;
        const cl::NDRange origin = {0, 0, 0};
        const cl::NDRange range = {output.getSize(), 1, 1};

        for (int i = 0; i < 3; ++i)
        {
            cl_int const numIterations = 1 + i * 5;
            m_programFront->run("adoptVertexPositions2d",
                                origin,
                                range,
                                input.getBuffer(),
                                output.getBuffer(),
                                static_cast<cl_int>(output.getSize()),
                                numIterations,
                                z_mm,
                                PAYLOAD_ARGUMENTS);

            m_programFront->run("adoptVertexPositions2d",
                                origin,
                                range,
                                output.getBuffer(),
                                input.getBuffer(),
                                static_cast<cl_int>(output.getSize()),
                                numIterations,
                                z_mm,
                                PAYLOAD_ARGUMENTS);
        }
        m_programFront->run("adoptVertexPositions2d",
                            origin,
                            range,
                            input.getBuffer(),
                            output.getBuffer(),
                            static_cast<cl_int>(output.getSize()),
                            5,
                            z_mm,
                            PAYLOAD_ARGUMENTS);
        output.read();
    }

    void SlicerProgram::setKernelReplacements(SharedKernelReplacements replacements)
    {
        m_programFront->setKernelReplacements(replacements);
    }
    
    bool SlicerProgram::buildMeshVoxelGrid(Primitives & primitives, 
                                           MeshVoxelGridBuildParams const & params)
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        ProfileFunction;
        
        swapProgramsIfNeeded();
        
        if (params.voxelCount <= 0)
        {
            return false;  // Nothing to build
        }
        
        cl::NDRange const origin = {0, 0, 0};
        cl::NDRange const globalRange = {static_cast<size_t>(params.voxelCount), 1, 1};
        
        m_programFront->run("buildMeshVoxelGrid",
                            origin,
                            globalRange,
                            primitives.data.getBuffer(),               // 0: primitiveData
                            static_cast<cl_int>(params.headerStart),   // 1: headerStart
                            static_cast<cl_int>(params.voxelDataOffset), // 2: voxelDataOffset
                            static_cast<cl_int>(params.nodesOffset),   // 3: nodesOffset
                            static_cast<cl_int>(params.trianglesOffset), // 4: trianglesOffset
                            static_cast<cl_int>(params.normalsOffset), // 5: normalsOffset
                            static_cast<cl_int>(params.indicesOffset), // 6: indicesOffset
                            static_cast<cl_int>(params.edgeNeighborsOffset), // 7: edgeNeighborsOffset
                            static_cast<cl_int>(params.nodeCount),     // 8: nodeCount
                            static_cast<cl_int>(params.triCount),      // 9: triCount
                            static_cast<cl_int>(params.vertexNormalCount)); // 10: vertexNormalCount
        
        return true;
    }
}
