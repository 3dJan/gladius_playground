#include "opencl_test_helper.h"

#include <kernel/types.h>
#include <nodes/Model.h>
#include <nodes/ToCommandStreamVisitor.h>

#include "gtest/gtest.h"

#include <sstream>
#include <stdexcept>
#include <string>

namespace gladius_tests
{
    namespace
    {
        class ToCommandStreamVisitorTest : public ::testing::Test
        {
          protected:
            void SetUp() override
            {
                m_computeContext = std::make_unique<gladius::ComputeContext>();
            }
            std::unique_ptr<gladius::ComputeContext> m_computeContext;
        };

        template <typename NodeType>
        Command visitSingleNode(gladius::ComputeContext & computeContext,
                                gladius::nodes::Assembly & assembly,
                                NodeType & node)
        {
            gladius::CommandBuffer cmds(computeContext);
            gladius::nodes::ToCommandStreamVisitor visitor(&cmds, &assembly);
            visitor.setModel(assembly.assemblyModel().get());

            visitor.visit(node);

            EXPECT_EQ(cmds.getData().size(), 1u);
            return cmds.getData().front();
        }

    } // namespace

    TEST_F(ToCommandStreamVisitorTest,
           Visit_ModelWithBeginAndEndNode_CommandStreamFilledWithCorrectCommands)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        gladius::nodes::Assembly assembly;
        assembly.assemblyModel()->createBeginEndWithDefaultInAndOuts();
        gladius::CommandBuffer cmds(*m_computeContext);
        gladius::nodes::ToCommandStreamVisitor dynamicOclVisitor(&cmds, &assembly);
        assembly.visitAssemblyNodes(dynamicOclVisitor);

        EXPECT_EQ(cmds.getSize(), 1u); // 1 command for the end node
    }

    TEST_F(
      ToCommandStreamVisitorTest,
      Visit_ModelWithBeginAndEndNodeAndOneNodeInBetween_UnusedCommandsAreNotAddedToCommandStream)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        using namespace gladius::nodes;
        gladius::nodes::Assembly assembly;
        auto & model = assembly.assemblyModel();
        model->createBeginEndWithDefaultInAndOuts();
        auto * addition = model->create<Addition>();

        auto inA = addition->getParameter(FieldNames::A);
        inA->setInputFromPort(model->getInputs().at(FieldNames::Pos));

        gladius::CommandBuffer cmds(*m_computeContext);
        gladius::nodes::ToCommandStreamVisitor dynamicOclVisitor(&cmds, &assembly);
        assembly.visitAssemblyNodes(dynamicOclVisitor);

        EXPECT_EQ(cmds.getSize(), 1u); // 1 command for the end node
    }

    TEST_F(ToCommandStreamVisitorTest, Visit_BoxMinMax_UsesPositionMinMaxArguments)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        using namespace gladius::nodes;
        Assembly assembly;
        auto model = assembly.assemblyModel();
        auto * boxMinMax = model->create<BoxMinMax>();

        boxMinMax->parameter().at(FieldNames::Pos).setLookUpIndex(10);
        boxMinMax->parameter().at(FieldNames::Min).setLookUpIndex(20);
        boxMinMax->parameter().at(FieldNames::Max).setLookUpIndex(30);

        auto const cmd = visitSingleNode(*m_computeContext, assembly, *boxMinMax);

        EXPECT_EQ(cmd.type, CT_BOX_MIN_MAX);
        EXPECT_EQ(cmd.args[0], 10);
        EXPECT_EQ(cmd.args[1], 20);
        EXPECT_EQ(cmd.args[2], 30);
    }

    TEST_F(ToCommandStreamVisitorTest,
           Visit_ComposeMatrixFromColumnsAndRows_UsesResultOutputAndVectorInputs)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        using namespace gladius::nodes;

        Assembly columnAssembly;
        auto columnModel = columnAssembly.assemblyModel();
        auto * columns = columnModel->create<ComposeMatrixFromColumns>();
        columns->parameter().at(FieldNames::Col0).setLookUpIndex(11);
        columns->parameter().at(FieldNames::Col1).setLookUpIndex(22);
        columns->parameter().at(FieldNames::Col2).setLookUpIndex(33);
        columns->parameter().at(FieldNames::Col3).setLookUpIndex(44);

        auto const columnCmd = visitSingleNode(*m_computeContext, columnAssembly, *columns);

        EXPECT_EQ(columnCmd.type, CT_COMPOSE_MATRIX_FROM_COLUMNS);
        EXPECT_EQ(columnCmd.args[0], 11);
        EXPECT_EQ(columnCmd.args[1], 22);
        EXPECT_EQ(columnCmd.args[2], 33);
        EXPECT_EQ(columnCmd.args[3], 44);
        EXPECT_GT(columnCmd.output[0], 0);

        Assembly rowAssembly;
        auto rowModel = rowAssembly.assemblyModel();
        auto * rows = rowModel->create<ComposeMatrixFromRows>();
        rows->parameter().at(FieldNames::Row0).setLookUpIndex(55);
        rows->parameter().at(FieldNames::Row1).setLookUpIndex(66);
        rows->parameter().at(FieldNames::Row2).setLookUpIndex(77);
        rows->parameter().at(FieldNames::Row3).setLookUpIndex(88);

        auto const rowCmd = visitSingleNode(*m_computeContext, rowAssembly, *rows);

        EXPECT_EQ(rowCmd.type, CT_COMPOSE_MATRIX_FROM_ROWS);
        EXPECT_EQ(rowCmd.args[0], 55);
        EXPECT_EQ(rowCmd.args[1], 66);
        EXPECT_EQ(rowCmd.args[2], 77);
        EXPECT_EQ(rowCmd.args[3], 88);
        EXPECT_GT(rowCmd.output[0], 0);
    }

    TEST_F(ToCommandStreamVisitorTest, Visit_Transpose_EmitsMatrixOutputCommand)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        using namespace gladius::nodes;
        Assembly assembly;
        auto model = assembly.assemblyModel();
        auto * transpose = model->create<Transpose>();
        transpose->parameter().at(FieldNames::A).setLookUpIndex(42);

        auto const cmd = visitSingleNode(*m_computeContext, assembly, *transpose);

        EXPECT_EQ(cmd.type, CT_TRANSPOSE);
        EXPECT_EQ(cmd.args[0], 42);
        EXPECT_GT(cmd.output[0], 0);
    }

    TEST_F(ToCommandStreamVisitorTest, Visit_DecomposeMatrix_EmitsAllScalarOutputs)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        using namespace gladius::nodes;
        Assembly assembly;
        auto model = assembly.assemblyModel();
        auto * decomposeMatrix = model->create<DecomposeMatrix>();
        decomposeMatrix->parameter().at(FieldNames::Matrix).setLookUpIndex(101);

        auto const cmd = visitSingleNode(*m_computeContext, assembly, *decomposeMatrix);

        EXPECT_EQ(cmd.type, CT_DECOMPOSE_MATRIX);
        EXPECT_EQ(cmd.args[0], 101);
        for (int component = 0; component < 16; ++component)
        {
            EXPECT_EQ(cmd.output[component], cmd.output[0] + component);
        }
    }

    TEST_F(ToCommandStreamVisitorTest,
           Visit_SignedDistanceToBeamLattice_EmitsMeshDistancePayloadCommand)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        using namespace gladius::nodes;
        Assembly assembly;
        auto model = assembly.assemblyModel();
        auto * beamLattice = model->create<SignedDistanceToBeamLattice>();
        beamLattice->parameter().at(FieldNames::Pos).setLookUpIndex(12);
        beamLattice->parameter().at(FieldNames::Start).setLookUpIndex(34);
        beamLattice->parameter().at(FieldNames::End).setLookUpIndex(56);

        auto const cmd = visitSingleNode(*m_computeContext, assembly, *beamLattice);

        EXPECT_EQ(cmd.type, CT_SIGNED_DISTANCE_TO_MESH);
        EXPECT_EQ(cmd.args[0], 12);
        EXPECT_EQ(cmd.args[1], 34);
        EXPECT_EQ(cmd.args[2], 56);
    }

    TEST_F(ToCommandStreamVisitorTest, Visit_UnloweredFunctionNodes_ThrowsHelpfulError)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        using namespace gladius::nodes;
        Assembly assembly;
        auto model = assembly.assemblyModel();
        auto * functionCall = model->create<FunctionCall>();
        auto * functionGradient = model->create<FunctionGradient>();

        gladius::CommandBuffer cmds(*m_computeContext);
        ToCommandStreamVisitor visitor(&cmds, &assembly);
        visitor.setModel(model.get());

        EXPECT_THROW(visitor.visit(*functionCall), std::runtime_error);
        EXPECT_THROW(visitor.visit(*functionGradient), std::runtime_error);
    }

    TEST_F(ToCommandStreamVisitorTest, Write_GeneratedSourceContainsFallbackAndDynamicImageSampler)
    {
        SKIP_IF_OPENCL_UNAVAILABLE();

        gladius::nodes::Assembly assembly;
        assembly.setFallbackValueLevelSet(123.0);
        assembly.assemblyModel()->createBeginEndWithDefaultInAndOuts();

        gladius::CommandBuffer cmds(*m_computeContext);
        gladius::nodes::ToCommandStreamVisitor visitor(&cmds, &assembly);
        assembly.visitAssemblyNodes(visitor);

        std::stringstream source;
        visitor.write(source);
        auto const generatedSource = source.str();

        EXPECT_NE(generatedSource.find("isnan(shape) || isinf(shape)"), std::string::npos);
        EXPECT_NE(generatedSource.find("if (cmds[i].type == CT_COMPOSE_MATRIX)"),
                  std::string::npos);
        EXPECT_NE(generatedSource.find("if (cmds[i].type == CT_MIX_SCALAR)"), std::string::npos);

        auto const vdbBranch = generatedSource.find("if (isVdbGrid)");
        auto const nearestBranch = generatedSource.find("else if (filter == 0)");
        EXPECT_NE(vdbBranch, std::string::npos);
        EXPECT_NE(nearestBranch, std::string::npos);
        EXPECT_LT(vdbBranch, nearestBranch);
    }
} // namespace gladius_tests
