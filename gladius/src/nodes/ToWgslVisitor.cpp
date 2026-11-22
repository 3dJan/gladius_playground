#include "ToWgslVisitor.h"

#include "Assembly.h"
#include "DerivedNodes.h"
#include "Model.h"
#include "Parameter.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace gladius::nodes
{
    namespace
    {
        /// WGSL type the generated function-signature entry point returns.
        auto constexpr FUNCTION_RESULT_TYPE_NAME = "GladiusFunctionResult";

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
            if (typeIndex == ParameterTypeIndex::Matrix4)
            {
                return "mat4x4<f32>";
            }

            throw std::runtime_error("WGSL evaluator supports only scalar, vec3, and mat4x4 graph values");
        }


        /// Number of consecutive floats one graph value occupies in the flat evaluator storage bindings.
        std::size_t typeIndexToFloatCount(std::type_index const typeIndex)
        {
            if (typeIndex == ParameterTypeIndex::Float)
            {
                return 1u;
            }
            if (typeIndex == ParameterTypeIndex::Float3)
            {
                return 3u;
            }
            if (typeIndex == ParameterTypeIndex::Matrix4)
            {
                return 16u;
            }

            throw std::runtime_error("WGSL evaluator supports only scalar, vec3 and matrix graph values");
        }

        /// WGSL expression that rebuilds one graph value from a flat f32 storage array.
        /// `base` is a u32 record offset and `offset` the component offset inside the record.
        std::string recordValueExpression(std::string const & wgslType,
                                          std::string const & bindingName,
                                          std::string const & base,
                                          std::size_t const offset)
        {
            auto const at = [&](std::size_t const component)
            {
                return fmt::format("{}.values[{} + {}u]", bindingName, base, offset + component);
            };

            if (wgslType == "f32")
            {
                return at(0u);
            }
            if (wgslType == "vec3<f32>")
            {
                return fmt::format("vec3<f32>({}, {}, {})", at(0u), at(1u), at(2u));
            }
            if (wgslType == "mat4x4<f32>")
            {
                std::array<std::string, 16> cells;
                for (std::size_t cell = 0u; cell < cells.size(); ++cell)
                {
                    cells[cell] = at(cell);
                }
                return fmt::format("mat4x4<f32>(vec4<f32>({0}, {1}, {2}, {3}), "
                                   "vec4<f32>({4}, {5}, {6}, {7}), "
                                   "vec4<f32>({8}, {9}, {10}, {11}), "
                                   "vec4<f32>({12}, {13}, {14}, {15}))",
                                   cells[0], cells[1], cells[2], cells[3],
                                   cells[4], cells[5], cells[6], cells[7],
                                   cells[8], cells[9], cells[10], cells[11],
                                   cells[12], cells[13], cells[14], cells[15]);
            }

            throw std::runtime_error("WGSL evaluator supports only scalar, vec3 and matrix graph values");
        }

        /// WGSL statements storing one graph value into a flat f32 storage array.
        std::string recordStoreStatements(std::string const & wgslType,
                                          std::string const & bindingName,
                                          std::string const & base,
                                          std::size_t const offset,
                                          std::string const & valueExpression,
                                          std::size_t const temporaryIndex)
        {
            auto const store = [&](std::size_t const component, std::string const & expression)
            {
                return fmt::format("    {}.values[{} + {}u] = {};\n", bindingName, base, offset + component, expression);
            };

            if (wgslType == "f32")
            {
                return store(0u, valueExpression);
            }

            std::string statements =
                fmt::format("    let rec{}: {} = {};\n", temporaryIndex, wgslType, valueExpression);
            if (wgslType == "vec3<f32>")
            {
                statements += store(0u, fmt::format("rec{}.x", temporaryIndex));
                statements += store(1u, fmt::format("rec{}.y", temporaryIndex));
                statements += store(2u, fmt::format("rec{}.z", temporaryIndex));
                return statements;
            }

            // mat4x4<f32>: elements are column-major, matching the render-side matrix lowering.
            for (std::size_t column = 0u; column < 4u; ++column)
            {
                for (std::size_t row = 0u; row < 4u; ++row)
                {
                    statements += store(column * 4u + row,
                                        fmt::format("rec{}[{}u][{}u]", temporaryIndex, column, row));
                }
            }
            return statements;
        }

        /// Maps a graph port name onto a WGSL identifier usable as a function parameter or local.
        std::string toWgslIdentifier(std::string const & name)
        {
            std::string result;
            result.reserve(name.size() + 2u);
            for (auto const character : name)
            {
                auto const isAlpha = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
                auto const isDigit = character >= '0' && character <= '9';
                result += (isAlpha || isDigit) ? character : '_';
            }
            if (result.empty() || !(result.front() >= 'a' && result.front() <= 'z')
                && !(result.front() >= 'A' && result.front() <= 'Z'))
            {
                result.insert(result.begin(), 'v');
            }
            return result;
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
        m_helperDefinitions.str({});
        m_helperDefinitions.clear();
        m_definition.str({});
        m_definition.clear();
        m_visitedNodes.clear();
        m_emittedHelpers.clear();
        m_referenceAnalysisPerformed = false;
        m_inlineExpressions.clear();
        m_requiredParameterCount = 0u;
        m_requiredParameterIds.clear();
        m_functionClosed = false;
        m_functionSignature = {};
        m_channelSelector.setModel(model);
    }

    void ToWgslVisitor::setAssembly(Assembly const * const assembly) noexcept
    {
        m_assembly = assembly;
    }

    void ToWgslVisitor::setHelperMode(std::string functionName,
                                      HelperSignature const * const signature) noexcept
    {
        m_helperFunctionName = std::move(functionName);
        m_helperSignature = signature;
    }

    void ToWgslVisitor::setEvaluatorChannel(EvaluatorChannel const channel)
    {
        m_channelSelector.setChannel(channel);
        if (channel == EvaluatorChannel::Translucency)
        {
            m_channelSelector.setEmittedChannels({std::string{FieldNames::Translucency}});
        }
    }

    EvaluatorChannel ToWgslVisitor::getEvaluatorChannel() const noexcept
    {
        return m_channelSelector.getChannel();
    }

    void ToWgslVisitor::write(std::ostream & out) const
    {
        out << m_helperDefinitions.str() << m_definition.str();
    }

    std::size_t ToWgslVisitor::getRequiredParameterCount() const noexcept
    {
        return m_requiredParameterCount;
    }

    bool ToWgslVisitor::usesParameter(ParameterId const parameterId) const noexcept
    {
        return m_requiredParameterIds.contains(parameterId);
    }

    void ToWgslVisitor::setBufferNumericLiterals(bool const enabled)
    {
        m_bufferNumericLiterals = enabled;
    }

    void ToWgslVisitor::setParameterLookupIndices(ParameterLookupIndices const * const indices) noexcept
    {
        m_parameterLookupIndices = indices;
    }

    void ToWgslVisitor::emitFunctionSignatureEntry(Begin & beginning)
    {
        if (m_currentModel == nullptr)
        {
            throw std::runtime_error("Function signature lowering requires a model");
        }

        m_functionSignature = {};
        auto const localizedName = toWgslIdentifier(fmt::format("graphFunction_{}", m_currentModel->getResourceId()));
        std::string argumentList;
        // Sort arguments by definition order (sort index), the same order the
        // graph and the evaluation tool use for positional binding.
        auto const & inputPorts = m_currentModel->getInputs();
        std::vector<std::pair<int, std::string>> sortableInputs;
        sortableInputs.reserve(inputPorts.size());
        for (auto const & [name, input] : inputPorts)
        {
            sortableInputs.emplace_back(input.getSortIndex(), name);
        }
        std::sort(sortableInputs.begin(), sortableInputs.end());

        auto const & beginOutputs = beginning.getOutputs();
        for (auto const & entry : sortableInputs)
        {
            auto const & name = entry.second;
            if (beginOutputs.find(name) == beginOutputs.end())
            {
                continue;
            }

            auto const & input = inputPorts.at(name);
            auto const wgslType = typeIndexToWgsl(input.getTypeIndex());

            if (!argumentList.empty())
            {
                argumentList += ",\n    ";
            }
            argumentList += input.getUniqueName() + ": " + wgslType;

            m_functionSignature.arguments.push_back(
                {input.getShortName(), wgslType, typeIndexToFloatCount(input.getTypeIndex())});
        }

        for (auto const & argument : m_functionSignature.arguments)
        {
            m_functionSignature.argumentComponents += argument.components;
        }

        if (argumentList.empty())
        {
            argumentList = "()";
        }
        else
        {
            argumentList = "(\n    " + argumentList + "\n)";
        }

        m_functionSignature.outputs = functionSignatureOutputDeclarations();
        m_functionSignature.entryPointName = localizedName;
        m_functionSignature.resultTypeName = FUNCTION_RESULT_TYPE_NAME;

        m_definition << "struct " << FUNCTION_RESULT_TYPE_NAME << "\n{\n";
        for (auto const & output : m_functionSignature.outputs)
        {
            m_functionSignature.outputComponents += output.components;
            m_definition << "    " << output.wgslName << ": " << output.wgslType << ",\n";
        }
        m_definition << "}\n\n";

        m_definition << "fn " << localizedName << argumentList << " -> " << FUNCTION_RESULT_TYPE_NAME << "\n{\n";
        for (auto const & output : m_functionSignature.outputs)
        {
            m_definition << "    var " << output.wgslName << ": " << output.wgslType << ";\n";
        }
    }

    void ToWgslVisitor::emitFunctionSignatureOutputs(End & ending)
    {
        auto const & endParameters = ending.parameter();

        for (auto const & declaration : m_functionSignature.outputs)
        {
            auto const parameterIterator = endParameters.find(declaration.graphName);
            if (parameterIterator == endParameters.end())
            {
                throw std::runtime_error("Function output '" + declaration.graphName + "' has no end parameter");
            }

            m_definition << "    " << declaration.wgslName << " = "
                         << resolveParameter(parameterIterator->second) << ";\n";
        }

        std::string resultArguments;
        for (auto const & output : m_functionSignature.outputs)
        {
            if (!resultArguments.empty())
            {
                resultArguments += ", ";
            }
            resultArguments += output.wgslName;
        }
        m_definition << "    return " << FUNCTION_RESULT_TYPE_NAME << "(" << resultArguments << ");\n}\n";

        emitFunctionRecordAdapter();
        m_functionClosed = true;
    }

    void ToWgslVisitor::emitFunctionRecordAdapter()
    {
        // Adapter between the flat per-sample evaluator bindings and the graph function signature:
        // one record of argument components in, one record of output components out. The storage
        // binding names are the contract of src/webgpu/shaders/sdf_evaluate_function.wgsl.
        m_definition << "fn gladiusCallRecord(argsBase: u32, resultsBase: u32)\n{\n";

        std::string callArguments;
        std::size_t argumentOffset = 0u;
        for (std::size_t index = 0u; index < m_functionSignature.arguments.size(); ++index)
        {
            auto const & argument = m_functionSignature.arguments[index];
            m_definition << "    let in" << index << ": " << argument.wgslType << " = "
                         << recordValueExpression(argument.wgslType, "function_arguments", "argsBase", argumentOffset)
                         << ";\n";
            if (!callArguments.empty())
            {
                callArguments += ", ";
            }
            callArguments += "in" + std::to_string(index);
            argumentOffset += argument.components;
        }

        m_definition << "    let gladiusRecord: " << m_functionSignature.resultTypeName << " = "
                     << m_functionSignature.entryPointName << "(" << callArguments << ");\n";

        std::size_t outputOffset = 0u;
        for (std::size_t index = 0u; index < m_functionSignature.outputs.size(); ++index)
        {
            auto const & output = m_functionSignature.outputs[index];
            m_definition << recordStoreStatements(output.wgslType,
                                                  "function_results",
                                                  "resultsBase",
                                                  outputOffset,
                                                  "gladiusRecord." + output.wgslName,
                                                  index);
            outputOffset += output.components;
        }
        m_definition << "}\n";
    }

    std::vector<ToWgslVisitor::FunctionOutputDescriptor> ToWgslVisitor::functionSignatureOutputDeclarations()
    {
        std::vector<FunctionOutputDescriptor> declarations;

        // Map order, the same order the OpenCL lowering uses for out parameters.
        for (auto const & [name, parameter] : m_currentModel->getOutputs())
        {
            auto const typeIndex = parameter.getTypeIndex();
            declarations.push_back(
                {name,
                 toWgslIdentifier(fmt::format("out{}", declarations.size())),
                 typeIndexToWgsl(typeIndex),
                 typeIndexToFloatCount(typeIndex)});
        }

        return declarations;
    }

    ToWgslVisitor::FunctionSignatureDescriptor ToWgslVisitor::getFunctionSignature() const
    {
        return m_functionSignature;
    }

    bool ToWgslVisitor::beginNode(NodeBase const & node)
    {
        if (m_functionClosed)
        {
            return false;
        }

        if (m_channelSelector.getChannel() != EvaluatorChannel::ShapeAndColor
            && !m_channelSelector.isNeededByChannel(node.getId()))
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
        if (parameter.isModifiable() || m_bufferNumericLiterals)
        {
            int lookupIndex = -1;
            if (m_parameterLookupIndices != nullptr)
            {
                auto const index = m_parameterLookupIndices->find(parameter.getId());
                if (index != m_parameterLookupIndices->end())
                {
                    lookupIndex = index->second;
                }
            }
            else
            {
                lookupIndex = const_cast<IParameter &>(parameter).getLookUpIndex();
            }
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
        return fmt::format("mat4x4<f32>(vec4<f32>({0}, {1}, {2}, {3}), "
                           "vec4<f32>({4}, {5}, {6}, {7}), "
                           "vec4<f32>({8}, {9}, {10}, {11}), "
                           "vec4<f32>({12}, {13}, {14}, {15}))",
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

        if (m_helperSignature != nullptr)
        {
            m_definition << fmt::format("fn {}(", m_helperFunctionName);
            for (std::size_t i = 0u; i < m_helperSignature->inputNames.size(); ++i)
            {
                auto const & inputName = m_helperSignature->inputNames[i];
                auto const & port = beginning.getOutputs().at(inputName);
                if (i > 0u)
                {
                    m_definition << ", ";
                }
                m_definition << fmt::format(
                  "arg_{}: {}", i, typeIndexToWgsl(port.getTypeIndex()));
            }
            auto const returnType = m_helperSignature->singleOutput
                                      ? m_helperSignature->outputWgslTypes.front()
                                      : m_helperSignature->structName;
            m_definition << fmt::format(") -> {} {{\n", returnType);
            for (std::size_t i = 0u; i < m_helperSignature->inputNames.size(); ++i)
            {
                auto const & inputName = m_helperSignature->inputNames[i];
                auto const & port = beginning.getOutputs().at(inputName);
                m_definition << fmt::format("let {}: {} = arg_{};\n",
                                            port.getUniqueName(),
                                            typeIndexToWgsl(port.getTypeIndex()),
                                            i);
            }
            return;
        }

        if (m_channelSelector.getChannel() == EvaluatorChannel::FunctionSignature)
        {
            emitFunctionSignatureEntry(beginning);
            return;
        }

        if (!m_channelSelector.providesSelectedChannel())
        {
            // The model does not provide the selected channel - emit no evaluator at all.
            m_functionClosed = true;
            return;
        }

        auto const & position = beginning.getOutputs().at(FieldNames::Pos);
        if (m_channelSelector.getChannel() == EvaluatorChannel::Translucency)
        {
            m_definition << fmt::format("fn evaluateTranslucency(position: vec3<f32>) -> f32 {{\n");
        }
        else
        {
            m_definition << fmt::format("fn evaluateModel(position: vec3<f32>) -> vec4<f32> {{\n");
        }
        m_definition << fmt::format("let {}: vec3<f32> = position;\n", position.getUniqueName());
    }

    void ToWgslVisitor::visit(End & ending)
    {
        if (!beginNode(ending))
        {
            return;
        }

        if (m_helperSignature != nullptr)
        {
            if (m_helperSignature->singleOutput)
            {
                m_definition << fmt::format(
                  "return {};\n}}\n",
                  resolveParameter(ending.parameter().at(m_helperSignature->outputNames.front())));
            }
            else
            {
                m_definition << fmt::format("return {}(", m_helperSignature->structName);
                for (std::size_t i = 0u; i < m_helperSignature->outputNames.size(); ++i)
                {
                    if (i > 0u)
                    {
                        m_definition << ", ";
                    }
                    m_definition << resolveParameter(
                      ending.parameter().at(m_helperSignature->outputNames[i]));
                }
                m_definition << ");\n}\n";
            }
            m_functionClosed = true;
            return;
        }

        if (m_channelSelector.getChannel() == EvaluatorChannel::FunctionSignature)
        {
            emitFunctionSignatureOutputs(ending);
            m_functionClosed = true;
            return;
        }

        if (m_channelSelector.getChannel() == EvaluatorChannel::Translucency)
        {
            // Clamp to the ISO/TS 19307 translucency alpha range instead of trusting the graph.
            m_definition << fmt::format("return clamp({}, 0.0f, 1.0f);\n}}\n",
                                        resolveParameter(ending.parameter().at(FieldNames::Translucency)));
            m_functionClosed = true;
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

    void ToWgslVisitor::visit(ImageSampler & imageSampler)
    {
        if (!beginNode(imageSampler))
        {
            return;
        }

        auto const resourceId = imageSampler.getImageResourceId();
        auto const & colorOutput = imageSampler.getOutputs().at(FieldNames::Color);
        auto const rgbaName = colorOutput.getUniqueName() + "_rgba";
        m_definition << fmt::format(
          "let {}: vec4<f32> = gladiusSampleImage({}, {}u, vec3<u32>({}u, {}u, {}u), {}u);\n",
          rgbaName,
          resolveParameter(imageSampler.parameter().at(FieldNames::UVW)),
          resourceId,
          static_cast<unsigned int>(imageSampler.getTileStyleU()),
          static_cast<unsigned int>(imageSampler.getTileStyleV()),
          static_cast<unsigned int>(imageSampler.getTileStyleW()),
          static_cast<unsigned int>(imageSampler.getFilter()));
        emitValue(imageSampler, FieldNames::Color, "vec3<f32>", rgbaName + ".xyz");
        emitValue(imageSampler, FieldNames::Alpha, "f32", rgbaName + ".w");
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

    void ToWgslVisitor::emitHelperFunction(ResourceId const functionId, Model & callee)
    {
        if (m_emittedHelpers.contains(functionId))
        {
            return;
        }

        auto * const beginNode = callee.getBeginNode();
        auto * const endNode = callee.getEndNode();
        if (beginNode == nullptr || endNode == nullptr)
        {
            throw std::runtime_error(
              fmt::format("WGSL helper function {} is missing Begin/End nodes", functionId));
        }

        HelperSignature signature;
        signature.functionName = fmt::format("gladius_fn_{}", functionId);
        signature.structName = fmt::format("GladiusFnOut_{}", functionId);

        for (auto const & [portName, port] : beginNode->getOutputs())
        {
            if (port.getTypeIndex() == ParameterTypeIndex::Float ||
                port.getTypeIndex() == ParameterTypeIndex::Float3 ||
                port.getTypeIndex() == ParameterTypeIndex::Matrix4)
            {
                signature.inputNames.push_back(portName);
            }
        }

        for (auto const & [paramName, param] : endNode->constParameter())
        {
            if (param.getTypeIndex() == ParameterTypeIndex::Float ||
                param.getTypeIndex() == ParameterTypeIndex::Float3 ||
                param.getTypeIndex() == ParameterTypeIndex::Matrix4)
            {
                signature.outputNames.push_back(paramName);
                signature.outputWgslTypes.push_back(typeIndexToWgsl(param.getTypeIndex()));
            }
        }

        if (signature.outputNames.empty())
        {
            throw std::runtime_error(
              fmt::format("WGSL helper function {} has no supported outputs", functionId));
        }
        signature.singleOutput = (signature.outputNames.size() == 1u);

        auto const [it, inserted] = m_emittedHelpers.emplace(functionId, std::move(signature));
        (void) inserted;
        auto const & storedSig = it->second;

        if (!storedSig.singleOutput)
        {
            m_helperDefinitions << fmt::format("struct {} {{\n", storedSig.structName);
            for (std::size_t i = 0u; i < storedSig.outputNames.size(); ++i)
            {
                m_helperDefinitions << fmt::format(
                  "    out_{}: {},\n", i, storedSig.outputWgslTypes[i]);
            }
            m_helperDefinitions << "};\n";
        }

        ToWgslVisitor nested;
        nested.setAssembly(m_assembly);
        nested.setBufferNumericLiterals(m_bufferNumericLiterals);
        nested.setParameterLookupIndices(m_parameterLookupIndices);
        nested.setHelperMode(storedSig.functionName, &storedSig);
        callee.visitNodes(nested);

        std::ostringstream nestedCode;
        nested.write(nestedCode);
        m_helperDefinitions << nestedCode.str() << "\n";
        m_requiredParameterCount =
          std::max(m_requiredParameterCount, nested.getRequiredParameterCount());
        m_requiredParameterIds.insert(nested.m_requiredParameterIds.begin(),
                                      nested.m_requiredParameterIds.end());
    }

    void ToWgslVisitor::visit(FunctionCall & functionCall)
    {
        if (!beginNode(functionCall))
        {
            return;
        }

        if (m_assembly == nullptr)
        {
            throw std::runtime_error(
              "WGSL evaluator encountered FunctionCall without an attached Assembly");
        }

        auto const functionId = functionCall.getFunctionId();
        auto const callee = const_cast<Assembly *>(m_assembly)->findModel(functionId);
        if (!callee)
        {
            throw std::runtime_error(
              fmt::format("WGSL evaluator could not resolve FunctionCall target {}", functionId));
        }

        emitHelperFunction(functionId, *callee);
        auto const & signature = m_emittedHelpers.at(functionId);

        std::ostringstream callExpr;
        callExpr << signature.functionName << "(";
        for (std::size_t i = 0u; i < signature.inputNames.size(); ++i)
        {
            if (i > 0u)
            {
                callExpr << ", ";
            }
            auto const & inputName = signature.inputNames[i];
            callExpr << resolveParameter(functionCall.parameter().at(inputName));
        }
        callExpr << ")";

        if (signature.singleOutput)
        {
            auto const & outName = signature.outputNames.front();
            emitValue(functionCall, outName, signature.outputWgslTypes.front(), callExpr.str());
            return;
        }

        auto const resultVar = fmt::format("call_res_{}", functionCall.getId());
        m_definition << fmt::format(
          "let {}: {} = {};\n", resultVar, signature.structName, callExpr.str());
        for (std::size_t i = 0u; i < signature.outputNames.size(); ++i)
        {
            auto const & outName = signature.outputNames[i];
            if (functionCall.getOutputs().contains(outName))
            {
                emitValue(functionCall,
                          outName,
                          signature.outputWgslTypes[i],
                          fmt::format("{}.out_{}", resultVar, i));
            }
        }
    }
}
