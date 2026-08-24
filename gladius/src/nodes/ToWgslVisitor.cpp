#include "ToWgslVisitor.h"

#include "DerivedNodes.h"
#include "Parameter.h"

#include <fmt/format.h>

#include <array>
#include <cmath>
#include <stdexcept>

namespace gladius::nodes
{
    namespace
    {
        std::string typeIndexToWgsl(std::type_index const typeIndex)
        {
            if (typeIndex == ParameterTypeIndex::Float)
            {
                return "f32";
            }
            if (typeIndex == ParameterTypeIndex::Float3)
            {
                return "vec3<f32>";
            }

            throw std::runtime_error("WGSL evaluator supports only scalar and vec3 graph values");
        }

        std::string formatFloat(float const value)
        {
            if (!std::isfinite(value))
            {
                throw std::runtime_error("WGSL evaluator does not support non-finite static values");
            }

            return fmt::format("{:.9g}", value) + "f";
        }
    }

    void ToWgslVisitor::setModel(Model * const model)
    {
        Visitor::setModel(model);
        m_definition.str({});
        m_definition.clear();
        m_visitedNodes.clear();
        m_referenceAnalysisPerformed = false;
        m_inlineExpressions.clear();
        m_requiredParameterCount = 0u;
        m_requiredParameterIds.clear();
        m_functionClosed = false;
    }

    void ToWgslVisitor::write(std::ostream & out) const
    {
        out << m_definition.str();
    }

    std::size_t ToWgslVisitor::getRequiredParameterCount() const noexcept
    {
        return m_requiredParameterCount;
    }

    bool ToWgslVisitor::usesParameter(ParameterId const parameterId) const noexcept
    {
        return m_requiredParameterIds.contains(parameterId);
    }

    bool ToWgslVisitor::beginNode(NodeBase const & node)
    {
        if (m_functionClosed)
        {
            return false;
        }

        return m_visitedNodes.insert(node.getId()).second;
    }

    bool ToWgslVisitor::shouldInlineOutput(NodeBase const & node, std::string const & portName) const
    {
        if (!m_referenceAnalysisPerformed && m_currentModel)
        {
            m_referenceAnalyzer.setModel(m_currentModel);
            m_referenceAnalyzer.analyze();
            m_referenceAnalysisPerformed = true;
        }

        return m_referenceAnalyzer.shouldInline(node.getId(), portName);
    }

    std::string ToWgslVisitor::resolveStaticValue(IParameter const & parameter) const
    {
        auto const * variantParameter = dynamic_cast<VariantParameter const *>(&parameter);
        if (variantParameter == nullptr)
        {
            throw std::runtime_error("WGSL evaluator received an unsupported parameter type");
        }

        auto const & value = variantParameter->getValue();
        if (parameter.isModifiable())
        {
            auto const lookupIndex = const_cast<IParameter &>(parameter).getLookUpIndex();
            if (lookupIndex < 0)
            {
                throw std::runtime_error("WGSL evaluator received an invalid parameter lookup index");
            }

            auto const index = static_cast<std::size_t>(lookupIndex);
            if (std::holds_alternative<float>(value))
            {
                m_requiredParameterIds.insert(parameter.getId());
                m_requiredParameterCount = std::max(m_requiredParameterCount, index + 1u);
                return fmt::format("parameters.values[{}u]", index);
            }
            if (std::holds_alternative<float3>(value))
            {
                m_requiredParameterIds.insert(parameter.getId());
                m_requiredParameterCount = std::max(m_requiredParameterCount, index + 3u);
                return fmt::format("vec3<f32>(parameters.values[{0}u], parameters.values[{1}u], parameters.values[{2}u])",
                                   index,
                                   index + 1u,
                                   index + 2u);
            }
            if (std::holds_alternative<Matrix4x4>(value))
            {
                m_requiredParameterIds.insert(parameter.getId());
                m_requiredParameterCount = std::max(m_requiredParameterCount, index + 16u);
                std::array<std::string, 16> values;
                for (std::size_t offset = 0u; offset < values.size(); ++offset)
                {
                    values[offset] = fmt::format("parameters.values[{}u]", index + offset);
                }
                return composeMatrixExpression(values);
            }

            throw std::runtime_error("WGSL evaluator supports only scalar, vec3, and Matrix4x4 modifiable parameters");
        }

        if (auto const * scalar = std::get_if<float>(&value))
        {
            return formatFloat(*scalar);
        }
        if (auto const * vector = std::get_if<float3>(&value))
        {
            return fmt::format("vec3<f32>({}, {}, {})",
                               formatFloat(vector->x),
                               formatFloat(vector->y),
                               formatFloat(vector->z));
        }
        if (auto const * matrix = std::get_if<Matrix4x4>(&value))
        {
            std::array<std::string, 16> values;
            for (std::size_t row = 0u; row < 4u; ++row)
            {
                for (std::size_t column = 0u; column < 4u; ++column)
                {
                    values[row * 4u + column] = formatFloat((*matrix)[row][column]);
                }
            }
            return composeMatrixExpression(values);
        }

        throw std::runtime_error("WGSL evaluator supports only static scalar, vec3, and Matrix4x4 parameters");
    }

    std::string ToWgslVisitor::resolveParameter(IParameter const & parameter) const
    {
        if (auto const & source = parameter.getConstSource(); source.has_value())
        {
            auto const key = std::make_pair(source->nodeId, std::string(source->shortName));
            if (auto const iterator = m_inlineExpressions.find(key); iterator != m_inlineExpressions.end())
            {
                return iterator->second;
            }
            return source->uniqueName;
        }

        return resolveStaticValue(parameter);
    }

    void ToWgslVisitor::emitValue(NodeBase const & node,
                                  std::string const & portName,
                                  std::string const & typeName,
                                  std::string expression)
    {
        auto const & output = node.getOutputs().at(portName);
        if (shouldInlineOutput(node, portName))
        {
            m_inlineExpressions.emplace(std::make_pair(node.getId(), portName), std::move(expression));
            return;
        }

        m_definition << fmt::format("let {}: {} = {};\n", output.getUniqueName(), typeName, expression);
    }

    std::string ToWgslVisitor::composeMatrixExpression(std::array<std::string, 16> const & values)
    {
        return fmt::format("mat4x4<f32>(vec4<f32>({0}, {4}, {8}, {12}), "
                           "vec4<f32>({1}, {5}, {9}, {13}), "
                           "vec4<f32>({2}, {6}, {10}, {14}), "
                           "vec4<f32>({3}, {7}, {11}, {15}))",
                           values[0], values[1], values[2], values[3],
                           values[4], values[5], values[6], values[7],
                           values[8], values[9], values[10], values[11],
                           values[12], values[13], values[14], values[15]);
    }

    void ToWgslVisitor::emitUnaryOperation(NodeBase & node, std::string const & operation)
    {
        if (!beginNode(node))
        {
            return;
        }

        auto const & output = node.getOutputs().at(FieldNames::Result);
        auto const typeName = typeIndexToWgsl(output.getTypeIndex());
        emitValue(node,
                  FieldNames::Result,
                  typeName,
                  fmt::format("{}({})", operation, resolveParameter(node.parameter().at(FieldNames::A))));
    }

    void ToWgslVisitor::emitBinaryOperation(NodeBase & node, std::string const & operation)
    {
        if (!beginNode(node))
        {
            return;
        }

        auto const & output = node.getOutputs().at(FieldNames::Result);
        auto const typeName = typeIndexToWgsl(output.getTypeIndex());
        emitValue(node,
                  FieldNames::Result,
                  typeName,
                  fmt::format("{}({}, {})",
                              operation,
                              resolveParameter(node.parameter().at(FieldNames::A)),
                              resolveParameter(node.parameter().at(FieldNames::B))));
    }

    void ToWgslVisitor::visit(Begin & beginning)
    {
        if (!beginNode(beginning))
        {
            return;
        }

        auto const & position = beginning.getOutputs().at(FieldNames::Pos);
        m_definition << fmt::format("fn evaluateModel(position: vec3<f32>) -> vec4<f32> {{\n");
        m_definition << fmt::format("let {}: vec3<f32> = position;\n", position.getUniqueName());
    }

    void ToWgslVisitor::visit(End & ending)
    {
        if (!beginNode(ending))
        {
            return;
        }

        m_definition << fmt::format("return vec4<f32>({}, {});\n}}\n",
                                    resolveParameter(ending.parameter().at(FieldNames::Color)),
                                    resolveParameter(ending.parameter().at(FieldNames::Shape)));
        m_functionClosed = true;
    }

    void ToWgslVisitor::visit(ConstantScalar & constantScalar)
    {
        if (beginNode(constantScalar))
        {
            emitValue(constantScalar,
                      FieldNames::Value,
                      "f32",
                      resolveParameter(constantScalar.parameter().at(FieldNames::Value)));
        }
    }

    void ToWgslVisitor::visit(ConstantVector & constantVector)
    {
        if (!beginNode(constantVector))
        {
            return;
        }

        emitValue(constantVector,
                  FieldNames::Vector,
                  "vec3<f32>",
                  fmt::format("vec3<f32>({}, {}, {})",
                              resolveParameter(constantVector.parameter().at(FieldNames::X)),
                              resolveParameter(constantVector.parameter().at(FieldNames::Y)),
                              resolveParameter(constantVector.parameter().at(FieldNames::Z))));
    }

    void ToWgslVisitor::visit(ConstantMatrix & constantMatrix)
    {
        if (!beginNode(constantMatrix))
        {
            return;
        }

        std::array<std::string, 16> values;
        for (std::size_t row = 0u; row < 4u; ++row)
        {
            for (std::size_t column = 0u; column < 4u; ++column)
            {
                values[row * 4u + column] =
                  resolveParameter(constantMatrix.parameter().at(fmt::format("m{}{}", row, column)));
            }
        }

        emitValue(constantMatrix,
                  FieldNames::Matrix,
                  "mat4x4<f32>",
                  composeMatrixExpression(values));
    }

    void ToWgslVisitor::visit(ComposeVector & composeVector)
    {
        if (!beginNode(composeVector))
        {
            return;
        }

        emitValue(composeVector,
                  FieldNames::Result,
                  "vec3<f32>",
                  fmt::format("vec3<f32>({}, {}, {})",
                              resolveParameter(composeVector.parameter().at(FieldNames::X)),
                              resolveParameter(composeVector.parameter().at(FieldNames::Y)),
                              resolveParameter(composeVector.parameter().at(FieldNames::Z))));
    }

    void ToWgslVisitor::visit(ComposeMatrix & composeMatrix)
    {
        if (!beginNode(composeMatrix))
        {
            return;
        }

        std::array<std::string, 16> values;
        for (std::size_t row = 0u; row < 4u; ++row)
        {
            for (std::size_t column = 0u; column < 4u; ++column)
            {
                values[row * 4u + column] =
                  resolveParameter(composeMatrix.parameter().at(fmt::format("m{}{}", row, column)));
            }
        }

        emitValue(composeMatrix,
                  FieldNames::Result,
                  "mat4x4<f32>",
                  composeMatrixExpression(values));
    }

    void ToWgslVisitor::visit(ComposeMatrixFromColumns & composeMatrixFromColumns)
    {
        if (!beginNode(composeMatrixFromColumns))
        {
            return;
        }

        auto const col0 = resolveParameter(composeMatrixFromColumns.parameter().at(FieldNames::Col0));
        auto const col1 = resolveParameter(composeMatrixFromColumns.parameter().at(FieldNames::Col1));
        auto const col2 = resolveParameter(composeMatrixFromColumns.parameter().at(FieldNames::Col2));
        auto const col3 = resolveParameter(composeMatrixFromColumns.parameter().at(FieldNames::Col3));
        emitValue(composeMatrixFromColumns,
                  FieldNames::Result,
                  "mat4x4<f32>",
                  fmt::format("mat4x4<f32>(vec4<f32>({}, 0.0f), vec4<f32>({}, 0.0f), "
                              "vec4<f32>({}, 0.0f), vec4<f32>({}, 1.0f))",
                              col0,
                              col1,
                              col2,
                              col3));
    }

    void ToWgslVisitor::visit(ComposeMatrixFromRows & composeMatrixFromRows)
    {
        if (!beginNode(composeMatrixFromRows))
        {
            return;
        }

        auto const row0 = resolveParameter(composeMatrixFromRows.parameter().at(FieldNames::Row0));
        auto const row1 = resolveParameter(composeMatrixFromRows.parameter().at(FieldNames::Row1));
        auto const row2 = resolveParameter(composeMatrixFromRows.parameter().at(FieldNames::Row2));
        auto const row3 = resolveParameter(composeMatrixFromRows.parameter().at(FieldNames::Row3));
        emitValue(composeMatrixFromRows,
                  FieldNames::Result,
                  "mat4x4<f32>",
                  fmt::format("mat4x4<f32>(vec4<f32>(({0}).x, ({1}).x, ({2}).x, ({3}).x), "
                              "vec4<f32>(({0}).y, ({1}).y, ({2}).y, ({3}).y), "
                              "vec4<f32>(({0}).z, ({1}).z, ({2}).z, ({3}).z), "
                              "vec4<f32>(0.0f, 0.0f, 0.0f, 1.0f))",
                              row0,
                              row1,
                              row2,
                              row3));
    }

    void ToWgslVisitor::visit(Addition & addition)
    {
        if (beginNode(addition))
        {
            auto const & output = addition.getOutputs().at(FieldNames::Result);
            emitValue(addition,
                      FieldNames::Result,
                      typeIndexToWgsl(output.getTypeIndex()),
                      fmt::format("({} + {})",
                                  resolveParameter(addition.parameter().at(FieldNames::A)),
                                  resolveParameter(addition.parameter().at(FieldNames::B))));
        }
    }

    void ToWgslVisitor::visit(Multiplication & multiplication)
    {
        if (beginNode(multiplication))
        {
            auto const & output = multiplication.getOutputs().at(FieldNames::Result);
            emitValue(multiplication,
                      FieldNames::Result,
                      typeIndexToWgsl(output.getTypeIndex()),
                      fmt::format("({} * {})",
                                  resolveParameter(multiplication.parameter().at(FieldNames::A)),
                                  resolveParameter(multiplication.parameter().at(FieldNames::B))));
        }
    }

    void ToWgslVisitor::visit(Subtraction & subtraction)
    {
        if (beginNode(subtraction))
        {
            auto const & output = subtraction.getOutputs().at(FieldNames::Result);
            emitValue(subtraction,
                      FieldNames::Result,
                      typeIndexToWgsl(output.getTypeIndex()),
                      fmt::format("({} - {})",
                                  resolveParameter(subtraction.parameter().at(FieldNames::A)),
                                  resolveParameter(subtraction.parameter().at(FieldNames::B))));
        }
    }

    void ToWgslVisitor::visit(Division & division)
    {
        if (beginNode(division))
        {
            auto const & output = division.getOutputs().at(FieldNames::Result);
            emitValue(division,
                      FieldNames::Result,
                      typeIndexToWgsl(output.getTypeIndex()),
                      fmt::format("({} / {})",
                                  resolveParameter(division.parameter().at(FieldNames::A)),
                                  resolveParameter(division.parameter().at(FieldNames::B))));
        }
    }

    void ToWgslVisitor::visit(DotProduct & dotProduct)
    {
        if (beginNode(dotProduct))
        {
            emitValue(dotProduct,
                      FieldNames::Result,
                      "f32",
                      fmt::format("dot({}, {})",
                                  resolveParameter(dotProduct.parameter().at(FieldNames::A)),
                                  resolveParameter(dotProduct.parameter().at(FieldNames::B))));
        }
    }

    void ToWgslVisitor::visit(CrossProduct & crossProduct)
    {
        if (beginNode(crossProduct))
        {
            emitValue(crossProduct,
                      FieldNames::Result,
                      "vec3<f32>",
                      fmt::format("cross({}, {})",
                                  resolveParameter(crossProduct.parameter().at(FieldNames::A)),
                                  resolveParameter(crossProduct.parameter().at(FieldNames::B))));
        }
    }

    void ToWgslVisitor::visit(Length & length)
    {
        if (beginNode(length))
        {
            emitValue(length,
                      FieldNames::Result,
                      "f32",
                      fmt::format("length({})", resolveParameter(length.parameter().at(FieldNames::A))));
        }
    }

    void ToWgslVisitor::visit(Min & min)
    {
        emitBinaryOperation(min, "min");
    }

    void ToWgslVisitor::visit(Max & max)
    {
        emitBinaryOperation(max, "max");
    }

    void ToWgslVisitor::visit(Abs & abs)
    {
        emitUnaryOperation(abs, "abs");
    }

    void ToWgslVisitor::visit(Sqrt & sqrt)
    {
        emitUnaryOperation(sqrt, "sqrt");
    }

    void ToWgslVisitor::visit(Pow & pow)
    {
        if (!beginNode(pow))
        {
            return;
        }

        emitValue(pow,
                  FieldNames::Value,
                  typeIndexToWgsl(pow.getOutputs().at(FieldNames::Value).getTypeIndex()),
                  fmt::format("pow({}, {})",
                              resolveParameter(pow.parameter().at(FieldNames::Base)),
                              resolveParameter(pow.parameter().at(FieldNames::Exponent))));
    }

    void ToWgslVisitor::visit(Mix & mix)
    {
        if (beginNode(mix))
        {
            emitValue(mix,
                      FieldNames::Result,
                      typeIndexToWgsl(mix.getOutputs().at(FieldNames::Result).getTypeIndex()),
                      fmt::format("mix({}, {}, {})",
                                  resolveParameter(mix.parameter().at(FieldNames::A)),
                                  resolveParameter(mix.parameter().at(FieldNames::B)),
                                  resolveParameter(mix.parameter().at(FieldNames::Ratio))));
        }
    }

    void ToWgslVisitor::visit(Clamp & clamp)
    {
        if (beginNode(clamp))
        {
            emitValue(clamp,
                      FieldNames::Result,
                      typeIndexToWgsl(clamp.getOutputs().at(FieldNames::Result).getTypeIndex()),
                      fmt::format("clamp({}, {}, {})",
                                  resolveParameter(clamp.parameter().at(FieldNames::A)),
                                  resolveParameter(clamp.parameter().at(FieldNames::Min)),
                                  resolveParameter(clamp.parameter().at(FieldNames::Max))));
        }
    }

    void ToWgslVisitor::visit(Sine & sine)
    {
        emitUnaryOperation(sine, "sin");
    }

    void ToWgslVisitor::visit(Cosine & cosine)
    {
        emitUnaryOperation(cosine, "cos");
    }

    void ToWgslVisitor::visit(Tangent & tangent)
    {
        emitUnaryOperation(tangent, "tan");
    }

    void ToWgslVisitor::visit(ArcSin & arcSin)
    {
        emitUnaryOperation(arcSin, "asin");
    }

    void ToWgslVisitor::visit(ArcCos & arcCos)
    {
        emitUnaryOperation(arcCos, "acos");
    }

    void ToWgslVisitor::visit(ArcTan & arcTan)
    {
        emitUnaryOperation(arcTan, "atan");
    }

    void ToWgslVisitor::visit(Fmod & fmod)
    {
        if (!beginNode(fmod))
        {
            return;
        }

        auto const & output = fmod.getOutputs().at(FieldNames::Result);
        auto const a = resolveParameter(fmod.parameter().at(FieldNames::A));
        auto const b = resolveParameter(fmod.parameter().at(FieldNames::B));
        emitValue(fmod,
                  FieldNames::Result,
                  typeIndexToWgsl(output.getTypeIndex()),
                  fmt::format("({0} - {1} * trunc({0} / {1}))", a, b));
    }

    void ToWgslVisitor::visit(Mod & mod)
    {
        if (!beginNode(mod))
        {
            return;
        }

        auto const & output = mod.getOutputs().at(FieldNames::Result);
        auto const a = resolveParameter(mod.parameter().at(FieldNames::A));
        auto const b = resolveParameter(mod.parameter().at(FieldNames::B));
        emitValue(mod,
                  FieldNames::Result,
                  typeIndexToWgsl(output.getTypeIndex()),
                  fmt::format("({0} - {1} * floor({0} / {1}))", a, b));
    }

    void ToWgslVisitor::visit(DecomposeVector & decomposeVector)
    {
        if (!beginNode(decomposeVector))
        {
            return;
        }

        auto const vector = resolveParameter(decomposeVector.parameter().at(FieldNames::A));
        emitValue(decomposeVector, FieldNames::X, "f32", fmt::format("({}).x", vector));
        emitValue(decomposeVector, FieldNames::Y, "f32", fmt::format("({}).y", vector));
        emitValue(decomposeVector, FieldNames::Z, "f32", fmt::format("({}).z", vector));
    }

    void ToWgslVisitor::visit(DecomposeMatrix & decomposeMatrix)
    {
        if (!beginNode(decomposeMatrix))
        {
            return;
        }

        auto const matrix = resolveParameter(decomposeMatrix.parameter().at(FieldNames::Matrix));
        for (std::size_t row = 0u; row < 4u; ++row)
        {
            for (std::size_t column = 0u; column < 4u; ++column)
            {
                auto const portName = fmt::format("m{}{}", row, column);
                emitValue(decomposeMatrix,
                          portName,
                          "f32",
                          fmt::format("({})[{}][{}]", matrix, column, row));
            }
        }
    }

    void ToWgslVisitor::visit(ArcTan2 & arcTan2)
    {
        emitBinaryOperation(arcTan2, "atan2");
    }

    void ToWgslVisitor::visit(Exp & exp)
    {
        emitUnaryOperation(exp, "exp");
    }

    void ToWgslVisitor::visit(Log & log)
    {
        emitUnaryOperation(log, "log");
    }

    void ToWgslVisitor::visit(Log2 & log2)
    {
        emitUnaryOperation(log2, "log2");
    }

    void ToWgslVisitor::visit(Log10 & log10)
    {
        emitUnaryOperation(log10, "log10");
    }

    void ToWgslVisitor::visit(Select & select)
    {
        if (!beginNode(select))
        {
            return;
        }

        auto const & output = select.getOutputs().at(FieldNames::Result);
        emitValue(select,
                  FieldNames::Result,
                  typeIndexToWgsl(output.getTypeIndex()),
                  fmt::format("select({}, {}, {} < {})",
                              resolveParameter(select.parameter().at(FieldNames::D)),
                              resolveParameter(select.parameter().at(FieldNames::C)),
                              resolveParameter(select.parameter().at(FieldNames::A)),
                              resolveParameter(select.parameter().at(FieldNames::B))));
    }

    void ToWgslVisitor::visit(SinH & sinh)
    {
        emitUnaryOperation(sinh, "sinh");
    }

    void ToWgslVisitor::visit(CosH & cosh)
    {
        emitUnaryOperation(cosh, "cosh");
    }

    void ToWgslVisitor::visit(TanH & tanh)
    {
        emitUnaryOperation(tanh, "tanh");
    }

    void ToWgslVisitor::visit(Round & round)
    {
        emitUnaryOperation(round, "round");
    }

    void ToWgslVisitor::visit(Ceil & ceil)
    {
        emitUnaryOperation(ceil, "ceil");
    }

    void ToWgslVisitor::visit(Floor & floor)
    {
        emitUnaryOperation(floor, "floor");
    }

    void ToWgslVisitor::visit(Sign & sign)
    {
        emitUnaryOperation(sign, "sign");
    }

    void ToWgslVisitor::visit(Fract & fract)
    {
        emitUnaryOperation(fract, "fract");
    }

    void ToWgslVisitor::visit(VectorFromScalar & vectorFromScalar)
    {
        if (!beginNode(vectorFromScalar))
        {
            return;
        }

        emitValue(vectorFromScalar,
                  FieldNames::Result,
                  "vec3<f32>",
                  fmt::format("vec3<f32>({})", resolveParameter(vectorFromScalar.parameter().at(FieldNames::A))));
    }

    void ToWgslVisitor::visit(BoxMinMax & boxMinMax)
    {
        if (!beginNode(boxMinMax))
        {
            return;
        }

        auto const position = resolveParameter(boxMinMax.parameter().at(FieldNames::Pos));
        auto const minimum = resolveParameter(boxMinMax.parameter().at(FieldNames::Min));
        auto const maximum = resolveParameter(boxMinMax.parameter().at(FieldNames::Max));
        auto const dimensions = fmt::format("({} - {})", maximum, minimum);
        auto const centeredPosition = fmt::format("({} - ({} * 0.5f + {}))", position, dimensions, minimum);
        auto const distance = fmt::format("(abs({}) - {} * 0.5f)", centeredPosition, dimensions);
        emitValue(boxMinMax,
                  FieldNames::Shape,
                  "f32",
                  fmt::format("min(max(({0}).x, max(({0}).y, ({0}).z)), 0.0f) + length(max({0}, vec3<f32>(0.0f)))",
                              distance));
    }

    void ToWgslVisitor::visit(MatrixVectorMultiplication & matrixVectorMultiplication)
    {
        if (!beginNode(matrixVectorMultiplication))
        {
            return;
        }

        emitValue(matrixVectorMultiplication,
                  FieldNames::Result,
                  "vec3<f32>",
                  fmt::format("({} * vec4<f32>({}, 1.0f)).xyz",
                              resolveParameter(matrixVectorMultiplication.parameter().at(FieldNames::A)),
                              resolveParameter(matrixVectorMultiplication.parameter().at(FieldNames::B))));
    }

    void ToWgslVisitor::visit(Transformation & transformation)
    {
        if (!beginNode(transformation))
        {
            return;
        }

        emitValue(transformation,
                  FieldNames::Pos,
                  "vec3<f32>",
                  fmt::format("({} * vec4<f32>({}, 1.0f)).xyz",
                              resolveParameter(transformation.parameter().at(FieldNames::Transformation)),
                              resolveParameter(transformation.parameter().at(FieldNames::Pos))));
    }

    void ToWgslVisitor::visit(Transpose & transpose)
    {
        if (!beginNode(transpose))
        {
            return;
        }

        emitValue(transpose,
                  FieldNames::Matrix,
                  "mat4x4<f32>",
                  fmt::format("transpose({})", resolveParameter(transpose.parameter().at(FieldNames::A))));
    }

    void ToWgslVisitor::visit(Resource & resource)
    {
        // Resource nodes carry compile-time IDs used while flattening function calls. They do
        // not have a runtime analytic value and should not emit WGSL declarations. A resource
        // consumed by an analytic node is rejected when that parameter is resolved instead of
        // producing a shader that references an undefined identifier.
        if (!beginNode(resource))
        {
            return;
        }
    }

    namespace
    {
        /// Resolve the resource id referenced by a mesh-distance node's Mesh input.
        [[nodiscard]] ResourceId resolveMeshResourceId(NodeBase & meshNode)
        {
            auto & meshParameter = meshNode.parameter().at(FieldNames::Mesh);
            auto const source = meshParameter.getSource();
            if (!source.has_value() || source->port == nullptr || source->port->getParent() == nullptr)
            {
                throw std::runtime_error("WGSL evaluator: mesh node has no connected mesh resource");
            }

            auto * resourceNode = dynamic_cast<Resource *>(source->port->getParent());
            if (resourceNode == nullptr)
            {
                throw std::runtime_error("WGSL evaluator: mesh input is not connected to a resource node");
            }
            return resourceNode->getResourceId();
        }

        /// Resolve the resource id referenced by a beam-lattice distance node's input.
        [[nodiscard]] ResourceId resolveBeamLatticeResourceId(NodeBase & beamNode)
        {
            auto & beamParameter = beamNode.parameter().at(FieldNames::BeamLattice);
            auto const source = beamParameter.getSource();
            if (!source.has_value() || source->port == nullptr || source->port->getParent() == nullptr)
            {
                throw std::runtime_error(
                  "WGSL evaluator: beam lattice node has no connected beam lattice resource");
            }

            auto * resourceNode = dynamic_cast<Resource *>(source->port->getParent());
            if (resourceNode == nullptr)
            {
                throw std::runtime_error(
                  "WGSL evaluator: beam lattice input is not connected to a resource node");
            }
            return resourceNode->getResourceId();
        }
    }

    void ToWgslVisitor::visit(SignedDistanceToMesh & signedDistanceToMesh)
    {
        if (!beginNode(signedDistanceToMesh))
        {
            return;
        }

        auto const resourceId = resolveMeshResourceId(signedDistanceToMesh);
        emitValue(signedDistanceToMesh,
                  FieldNames::Distance,
                  "f32",
                  fmt::format("gladiusSignedDistanceToMesh({}, {}u)",
                              resolveParameter(signedDistanceToMesh.parameter().at(FieldNames::Pos)),
                              resourceId));
    }

    void ToWgslVisitor::visit(UnsignedDistanceToMesh & unsignedDistanceToMesh)
    {
        if (!beginNode(unsignedDistanceToMesh))
        {
            return;
        }

        auto const resourceId = resolveMeshResourceId(unsignedDistanceToMesh);
        emitValue(unsignedDistanceToMesh,
                  FieldNames::Distance,
                  "f32",
                  fmt::format("gladiusUnsignedDistanceToMesh({}, {}u)",
                              resolveParameter(unsignedDistanceToMesh.parameter().at(FieldNames::Pos)),
                              resourceId));
    }

    void ToWgslVisitor::visit(SignedDistanceToBeamLattice & signedDistanceToBeamLattice)
    {
        if (!beginNode(signedDistanceToBeamLattice))
        {
            return;
        }

        auto const resourceId = resolveBeamLatticeResourceId(signedDistanceToBeamLattice);
        emitValue(signedDistanceToBeamLattice,
                  FieldNames::Distance,
                  "f32",
                  fmt::format("gladiusSignedDistanceToBeamLattice({}, {}u)",
                              resolveParameter(signedDistanceToBeamLattice.parameter().at(FieldNames::Pos)),
                              resourceId));
    }
}
