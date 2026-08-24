#pragma once

#include "NodeBase.h"
#include "OutputPortReferenceAnalyzer.h"
#include "Visitor.h"
#include "nodesfwd.h"

#include <array>
#include <map>
#include <set>
#include <sstream>
#include <string>

namespace gladius::nodes
{
    /**
     * @brief Emits a pure analytic Gladius model evaluator in WGSL.
     *
      * Resource-backed nodes are deliberately rejected until their WebGPU binding ABI is implemented.
      * Modifiable scalar, vec3, and Matrix4x4 values use the generated parameter storage binding.
     */
    class ToWgslVisitor final : public Visitor
    {
      public:
        void setModel(Model * model) override;
        void write(std::ostream & out) const;
        [[nodiscard]] std::size_t getRequiredParameterCount() const noexcept;
        [[nodiscard]] bool usesParameter(ParameterId parameterId) const noexcept;

        void visit(Begin & beginning) override;
        void visit(End & ending) override;
        void visit(ConstantScalar & constantScalar) override;
        void visit(ConstantVector & constantVector) override;
        void visit(ConstantMatrix & constantMatrix) override;
        void visit(ComposeVector & composeVector) override;
        void visit(ComposeMatrix & composeMatrix) override;
        void visit(ComposeMatrixFromColumns & composeMatrixFromColumns) override;
        void visit(ComposeMatrixFromRows & composeMatrixFromRows) override;
        void visit(DecomposeMatrix & decomposeMatrix) override;
        void visit(Addition & addition) override;
        void visit(Multiplication & multiplication) override;
        void visit(Subtraction & subtraction) override;
        void visit(Division & division) override;
        void visit(DotProduct & dotProduct) override;
        void visit(CrossProduct & crossProduct) override;
        void visit(Length & length) override;
        void visit(Min & min) override;
        void visit(Max & max) override;
        void visit(Abs & abs) override;
        void visit(Sqrt & sqrt) override;
        void visit(Pow & pow) override;
        void visit(Mix & mix) override;
        void visit(Clamp & clamp) override;
        void visit(Sine & sine) override;
        void visit(Cosine & cosine) override;
        void visit(Tangent & tangent) override;
        void visit(ArcSin & arcSin) override;
        void visit(ArcCos & arcCos) override;
        void visit(ArcTan & arcTan) override;
        void visit(Fmod & fmod) override;
        void visit(Mod & mod) override;
        void visit(DecomposeVector & decomposeVector) override;
        void visit(ArcTan2 & arcTan2) override;
        void visit(Exp & exp) override;
        void visit(Log & log) override;
        void visit(Log2 & log2) override;
        void visit(Log10 & log10) override;
        void visit(Select & select) override;
        void visit(SinH & sinh) override;
        void visit(CosH & cosh) override;
        void visit(TanH & tanh) override;
        void visit(Round & round) override;
        void visit(Ceil & ceil) override;
        void visit(Floor & floor) override;
        void visit(Sign & sign) override;
        void visit(Fract & fract) override;
        void visit(VectorFromScalar & vectorFromScalar) override;
        void visit(BoxMinMax & boxMinMax) override;
        void visit(MatrixVectorMultiplication & matrixVectorMultiplication) override;
        void visit(Transformation & transformation) override;
        void visit(Transpose & transpose) override;
        void visit(Resource & resource) override;
        void visit(SignedDistanceToMesh & signedDistanceToMesh) override;
        void visit(UnsignedDistanceToMesh & unsignedDistanceToMesh) override;
        void visit(SignedDistanceToBeamLattice & signedDistanceToBeamLattice) override;

      private:
        [[nodiscard]] bool beginNode(NodeBase const & node);
        [[nodiscard]] bool shouldInlineOutput(NodeBase const & node, std::string const & portName) const;
        [[nodiscard]] std::string resolveParameter(IParameter const & parameter) const;
        [[nodiscard]] std::string resolveStaticValue(IParameter const & parameter) const;
        void emitUnaryOperation(NodeBase & node, std::string const & operation);
        void emitBinaryOperation(NodeBase & node, std::string const & operation);
        void emitValue(NodeBase const & node,
                       std::string const & portName,
                       std::string const & typeName,
                       std::string expression);
        [[nodiscard]] static std::string composeMatrixExpression(std::array<std::string, 16> const & values);

        std::stringstream m_definition;
        std::set<NodeId> m_visitedNodes;
        mutable OutputPortReferenceAnalyzer m_referenceAnalyzer;
        mutable bool m_referenceAnalysisPerformed{};
        mutable std::map<std::pair<NodeId, std::string>, std::string> m_inlineExpressions;
        mutable std::size_t m_requiredParameterCount{};
        mutable std::set<ParameterId> m_requiredParameterIds;
        bool m_functionClosed{};
    };
}
