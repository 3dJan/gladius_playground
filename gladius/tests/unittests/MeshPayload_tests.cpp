#include "MeshBVH.h"
#include "MeshPayloadSerializer.h"
#include "compute/RenderSceneSnapshot.h"
#include "nodes/DerivedNodes.h"
#include "nodes/Model.h"
#include "nodes/ToWgslVisitor.h"
#include "webgpu/WebGPUFrameShaderComposer.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstring>
#include <sstream>

namespace gladius::nodes::tests
{
    namespace
    {
        /// Build a minimal valid SpatialMeshData: one triangle with a trivial BVH.
        SpatialMeshData makeSingleTriangleData()
        {
            SpatialMeshData data;
            data.originalTriangleCount = 1u;

            float4 const v0{0.0f, 0.0f, 0.0f, 0.0f};
            float4 const v1{1.0f, 0.0f, 0.0f, 0.0f};
            float4 const v2{0.0f, 1.0f, 0.0f, 0.0f};

            MeshBVHNode node;
            node.bboxMin = {0.0f, 0.0f, 0.0f, 0.0f};
            node.bboxMax = {1.0f, 1.0f, 0.0f, 0.0f};
            node.leftChild = -1;
            node.rightChild = -1;
            node.primStart = 0;
            node.primCount = 1;
            data.nodes.push_back(node);

            MeshTriangle tri;
            tri.v0 = v0;
            tri.v1 = v1;
            tri.v2 = v2;
            tri.faceNormal = {0.0f, 0.0f, 1.0f, 0.0f};
            data.triangles.push_back(tri);

            MeshVertexNormal normal;
            normal.normal = {0.0f, 0.0f, 1.0f, 0.0f};
            data.vertexNormals.push_back(normal);

            data.triangleIndices.push_back({0, 0, 0});
            for (int i = 0; i < 3; ++i)
            {
                MeshEdgeNeighborNormal edgeNormal;
                edgeNormal.normal = {0.0f, 0.0f, 1.0f, 0.0f};
                data.edgeNeighborNormals.push_back(edgeNormal);
            }

            data.boundingBox.min = {0.0f, 0.0f, -0.01f, 0.0f};
            data.boundingBox.max = {1.0f, 1.0f, 0.01f, 0.0f};
            return data;
        }
    }

    TEST(MeshPayloadSerializer, SerializeSingleTriangle_ProducesHeaderWithLocalOffsets)
    {
        auto const data = makeSingleTriangleData();
        auto const payload = io::serializeSpatialMeshPayload(data);

        ASSERT_GE(payload.size(), io::MESH_PAYLOAD_HEADER_FLOATS);

        // Counts: nodes=1, triangles=1, vertexNormals=1.
        EXPECT_FLOAT_EQ(payload[8], 1.0f);
        EXPECT_FLOAT_EQ(payload[9], 1.0f);
        EXPECT_FLOAT_EQ(payload[10], 1.0f);

        // BVH offsets are local and non-zero.
        auto const nodesOffset = static_cast<std::size_t>(payload[12]);
        auto const trianglesOffset = static_cast<std::size_t>(payload[13]);
        auto const normalsOffset = static_cast<std::size_t>(payload[14]);
        auto const indicesOffset = static_cast<std::size_t>(payload[15]);
        EXPECT_GT(nodesOffset, io::MESH_PAYLOAD_HEADER_FLOATS);
        EXPECT_GT(trianglesOffset, nodesOffset);
        EXPECT_GT(normalsOffset, trianglesOffset);
        EXPECT_GT(indicesOffset, normalsOffset);

        // Pure-BVH mode: voxel count zero, FWN/NanoVDB slots zero.
        EXPECT_FLOAT_EQ(payload[27], 0.0f);
        EXPECT_FLOAT_EQ(payload[29], 0.0f);
        EXPECT_FLOAT_EQ(payload[33], 0.0f);

        // Edge-neighbor offset points within the payload.
        auto const edgeNeighborsOffset = static_cast<std::size_t>(payload[28]);
        EXPECT_GE(edgeNeighborsOffset, indicesOffset);
        EXPECT_LT(edgeNeighborsOffset, payload.size());

        // Payload size accounts for all sections including 4-float alignment
        // padding between sections (header 34 -> pad to 36).
        EXPECT_EQ(payload.size(),
                  io::MESH_PAYLOAD_HEADER_FLOATS + 2u   // header alignment pad
                    + 12u + 16u + 4u + 4u + 12u);
    }

    TEST(MeshPayloadSerializer, SerializeSingleTriangle_NodeAndTriangleDataMatchSource)
    {
        auto const data = makeSingleTriangleData();
        auto const payload = io::serializeSpatialMeshPayload(data);

        auto const nodesOffset = static_cast<std::size_t>(payload[12]);
        // First node bbox min/max.
        EXPECT_FLOAT_EQ(payload[nodesOffset + 0], 0.0f);
        EXPECT_FLOAT_EQ(payload[nodesOffset + 3], 0.0f);
        EXPECT_FLOAT_EQ(payload[nodesOffset + 4], 1.0f);
        // leftChild == -1 as bit pattern: reinterpret the float bits as int.
        int leftChild = 0;
        std::memcpy(&leftChild, &payload[nodesOffset + 8], sizeof(int));
        EXPECT_EQ(leftChild, -1);

        auto const trianglesOffset = static_cast<std::size_t>(payload[13]);
        EXPECT_FLOAT_EQ(payload[trianglesOffset + 1], 0.0f);          // v0.y
        EXPECT_FLOAT_EQ(payload[trianglesOffset + 4], 1.0f);          // v1.x
        EXPECT_FLOAT_EQ(payload[trianglesOffset + 12], 0.0f);         // faceNormal.x
        EXPECT_FLOAT_EQ(payload[trianglesOffset + 14], 1.0f);         // faceNormal.z
    }

    TEST(RenderSceneSnapshot, IsValid_WithMeshesRequiresMeshCapability)
    {
        compute::RenderSceneSnapshot snapshot;
        snapshot.sceneGeneration = 1u;
        snapshot.analyticEvaluatorWgsl = "fn evaluateModel(position: vec3<f32>) -> vec4<f32> {}";
        snapshot.requiredCapabilities = compute::RendererCapability::AnalyticRendering;

        // Declares no meshes: valid analytic-only snapshot.
        EXPECT_TRUE(snapshot.isValid());

        // Meshes present without the capability flag: invalid.
        snapshot.meshResources.resize(1u);
        snapshot.meshResources[0].data = io::serializeSpatialMeshPayload(makeSingleTriangleData());
        EXPECT_FALSE(snapshot.isValid());

        // Capability declared: valid again.
        snapshot.requiredCapabilities =
          snapshot.requiredCapabilities | compute::RendererCapability::MeshSdf;
        EXPECT_TRUE(snapshot.isValid());

        // Sparse holes (unused resource ids) are allowed as long as at least one
        // slot carries a real payload.
        snapshot.meshResources.push_back(compute::MeshResourcePayload{});
        EXPECT_TRUE(snapshot.isValid());
    }

    TEST(ToWgslVisitor, VisitSignedDistanceToMesh_EmitsMeshHookCall)
    {
        Model model;
        model.createBeginEndWithDefaultInAndOuts();

        auto * resource = model.create<Resource>();
        auto * meshDistance = model.create<SignedDistanceToMesh>();
        auto * color = model.create<ConstantVector>();
        ASSERT_NE(resource, nullptr);
        ASSERT_NE(meshDistance, nullptr);
        ASSERT_NE(color, nullptr);

        resource->parameter().at(FieldNames::ResourceId).setValue(ResourceId{7u});
        color->parameter().at(FieldNames::X).setValue(0.25f);
        color->parameter().at(FieldNames::Y).setValue(0.5f);
        color->parameter().at(FieldNames::Z).setValue(0.75f);
        color->parameter().at(FieldNames::X).setModifiable(false);
        color->parameter().at(FieldNames::Y).setModifiable(false);
        color->parameter().at(FieldNames::Z).setModifiable(false);

        ASSERT_TRUE(model.addLink(resource->getOutputValue().getId(),
                                  meshDistance->parameter().at(FieldNames::Mesh).getId()));
        ASSERT_TRUE(model.addLink(color->getVectorOutputPort().getId(),
                                  meshDistance->parameter().at(FieldNames::Pos).getId()));
        ASSERT_TRUE(model.addLink(meshDistance->getOutputs().at(FieldNames::Distance).getId(),
                                  model.getEndNode()->parameter().at(FieldNames::Shape).getId()));

        ToWgslVisitor visitor;
        ASSERT_NO_THROW(model.visitNodes(visitor));

        std::ostringstream source;
        visitor.write(source);
        auto const shader = source.str();

        EXPECT_NE(shader.find("gladiusSignedDistanceToMesh("), std::string::npos);
        EXPECT_NE(shader.find("7u"), std::string::npos);
    }

    TEST(WebGPUFrameShaderComposer, ComposeWithMeshSupport_IncludesMeshModuleAndBindings)
    {
        auto const shader = webgpu::WebGPUFrameShaderComposer::composeWithMeshSupport(
          "fn evaluateModel(position: vec3<f32>) -> vec4<f32> {\n"
          "    return vec4<f32>(vec3<f32>(1.0), gladiusSignedDistanceToMesh(position, 0u));\n"
          "}");

        // Mesh module content is present.
        EXPECT_NE(shader.find("fn gladiusSignedDistanceToMesh("), std::string::npos);
        EXPECT_NE(shader.find("mesh_payload_data"), std::string::npos);
        EXPECT_NE(shader.find("mesh_offset_table: array<vec2<u32>>"), std::string::npos);
        // Bindings 0-3 are the frame uniforms/output/parameters; the mesh module
        // occupies bindings 4 (payload) and 5 (offset table).
        EXPECT_NE(shader.find("@group(0) @binding(4)"), std::string::npos);
        EXPECT_NE(shader.find("@group(0) @binding(5)"), std::string::npos);
        EXPECT_EQ(shader.find("payload_base, payload_base +"), std::string::npos);

        // Evaluator injected after the mesh module; markers replaced.
        EXPECT_EQ(shader.find("// GLADIUS_MODEL_EVALUATOR"), std::string::npos);
        EXPECT_EQ(shader.find("// GLADIUS_MESH_SDF_MODULE"), std::string::npos);
        EXPECT_NE(shader.find("gladiusSignedDistanceToMesh(position, 0u)"), std::string::npos);
    }

    TEST(WebGPUFrameShaderComposer, ComposeWithoutMeshSupport_OmitsMeshModule)
    {
        auto const shader = webgpu::WebGPUFrameShaderComposer::compose(
          "fn evaluateModel(position: vec3<f32>) -> vec4<f32> { "
          "return vec4<f32>(vec3<f32>(1.0), 0.0); }");

        EXPECT_EQ(shader.find("gladiusSignedDistanceToMesh"), std::string::npos);
        EXPECT_EQ(shader.find("@group(0) @binding(3)"), std::string::npos);
    }
}
