#include "ExpressionToGraphConverter.h"
#include "ExpressionParser.h"
#include "FunctionArgument.h"
#include "nodes/DerivedNodes.h"
#include "nodes/Model.h"
#include "nodes/NodeBase.h"
#include "nodes/nodesfwd.h"
#include "nodes/types.h"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <regex>
#include <set>
#include <sstream>
#include <stack>
#include <stdexcept>
#include <typeindex>

namespace gladius
{
    // Static member definitions
    std::map<nodes::NodeId, std::string> ExpressionToGraphConverter::s_componentMap;
    std::map<std::string, nodes::NodeId> ExpressionToGraphConverter::s_vectorDecomposeNodes;
    std::map<std::string, ArgumentType> ExpressionToGraphConverter::s_beginNodeArguments;

    // Track the current variable context for Begin node port resolution
    thread_local std::vector<std::string> ExpressionToGraphConverter::s_variableContextStack;

    nodes::NodeId ExpressionToGraphConverter::convertSnippetToGraph(
      std::string const & snippet,
      nodes::Model & model,
      ExpressionParser & parser,
      std::vector<FunctionArgument> const & arguments,
      FunctionOutput const & output)
    {
        // @note if-else preprocessing only handles single-line bodies of the form:
        //   if (A op B) { var = C; } else { var = D; }
        // where op is one of <, <=, >, >=, ==, !=.  Multi-line blocks and complex
        // conditions (e.g. logical &&/||) are not matched and will cause the
        // containing statement to fall through to the assignment parser unchanged.
        //
        // Similarly, statement splitting is done on ';' without string/comment
        // awareness; snippets must not contain semicolons inside string literals.
        if (snippet.empty())
        {
            return 0;
        }

        // Clear any previous static state (matching convertExpressionToGraph)
        s_componentMap.clear();
        s_vectorDecomposeNodes.clear();
        s_beginNodeArguments.clear();
        s_variableContextStack.clear();

        // Preprocess if blocks into select function calls
        std::regex if_regex(R"(if\s*\(\s*(.+?)\s*(<|>|<=|>=|==|!=)\s*(.+?)\s*\)\s*\{\s*(?:(?:float|vec2|vec3|vec4|int|bool)\s+)?([a-zA-Z_][a-zA-Z0-9_]*)\s*=\s*(.+?)\s*;\s*\}\s*else\s*\{\s*(?:(?:float|vec2|vec3|vec4|int|bool)\s+)?\4\s*=\s*(.+?)\s*;\s*\})");
        
        std::string processedSnippet = snippet;
        std::smatch match;
        while (std::regex_search(processedSnippet, match, if_regex))
        {
            std::string A = match[1];
            std::string op = match[2];
            std::string B = match[3];
            std::string var = match[4];
            std::string C = match[5];
            std::string D = match[6];
            
            std::string replacement;
            if (op == "<" || op == "<=")
            {
                replacement = var + " = select(" + A + ", " + B + ", " + C + ", " + D + ");";
            }
            else if (op == ">" || op == ">=")
            {
                replacement = var + " = select(" + B + ", " + A + ", " + C + ", " + D + ");";
            }
            else if (op == "==")
            {
                replacement = var + " = select(abs(" + A + " - " + B + "), 1e-6, " + C + ", " + D + ");";
            }
            else if (op == "!=")
            {
                replacement = var + " = select(abs(" + A + " - " + B + "), 1e-6, " + D + ", " + C + ");";
            }
            
            processedSnippet = match.prefix().str() + replacement + match.suffix().str();
        }

        // Always ensure "pos" is available as an implicit vec3 argument (spatial coordinate)
        std::vector<FunctionArgument> effectiveArguments = arguments;
        bool const hasPosArg = std::any_of(arguments.begin(), arguments.end(),
          [](auto const & arg) { return arg.name == "pos"; });
        if (!hasPosArg)
        {
            effectiveArguments.insert(
              effectiveArguments.begin(), FunctionArgument{"pos", ArgumentType::Vector});
        }

        std::map<std::string, nodes::NodeId> variableNodes =
          createArgumentNodes(effectiveArguments, model);
        if (variableNodes.empty())
        {
            return 0;
        }

        // Split snippet by semicolons
        std::vector<std::string> statements;
        size_t start = 0;
        size_t end = processedSnippet.find(';');
        while (end != std::string::npos)
        {
            statements.push_back(processedSnippet.substr(start, end - start));
            start = end + 1;
            end = processedSnippet.find(';', start);
        }
        if (start < processedSnippet.length())
        {
            std::string lastStmt = processedSnippet.substr(start);
            if (!removeWhitespace(lastStmt).empty())
            {
                statements.push_back(lastStmt);
            }
        }

        std::regex assign_regex(R"(^\s*(?:(?:float|vec2|vec3|vec4|int|bool)\s+)?([a-zA-Z_][a-zA-Z0-9_]*)\s*=\s*(.+?)\s*$)");
        std::regex return_regex(R"(^\s*return\s+(.+?)\s*$)");

        nodes::NodeId lastResult = 0;

        for (std::string const & stmt : statements)
        {
            std::string cleanStmt = stmt;
            // Remove leading/trailing whitespace
            cleanStmt.erase(0, cleanStmt.find_first_not_of(" \t\r\n"));
            cleanStmt.erase(cleanStmt.find_last_not_of(" \t\r\n") + 1);

            if (cleanStmt.empty())
            {
                continue;
            }

            std::smatch stmtMatch;
            if (std::regex_match(cleanStmt, stmtMatch, assign_regex))
            {
                std::string varName = stmtMatch[1];
                std::string expr = stmtMatch[2];

                nodes::NodeId exprNodeId = parseAndBuildGraph(expr, model, variableNodes);
                if (exprNodeId == 0)
                {
                    return 0; // Failed to parse expression
                }

                variableNodes[varName] = exprNodeId;
                lastResult = exprNodeId;
            }
            else if (std::regex_match(cleanStmt, stmtMatch, return_regex))
            {
                std::string expr = stmtMatch[1];

                nodes::NodeId exprNodeId = parseAndBuildGraph(expr, model, variableNodes);
                if (exprNodeId == 0)
                {
                    return 0; // Failed to parse return expression
                }

                // Validate output type and connect to End node
                if (!validateOutputType(model, exprNodeId, output.type))
                {
                    return 0; // Type validation failed
                }

                if (!connectToEndNode(model, exprNodeId, output))
                {
                    return 0; // Failed to connect to End node
                }

                return exprNodeId;
            }
            else
            {
                // Try to parse as a single expression if it's the only statement
                if (statements.size() == 1)
                {
                    return convertExpressionToGraph(cleanStmt, model, parser, arguments, output);
                }
                return 0; // Unrecognized statement format
            }
        }

        return lastResult;
    }

    nodes::NodeId ExpressionToGraphConverter::convertExpressionToGraph(
      std::string const & expression,
      nodes::Model & model,
      ExpressionParser & parser,
      std::vector<FunctionArgument> const & arguments,
      FunctionOutput const & output)
    {
        // Clear any previous component mappings
        s_componentMap.clear();
        s_vectorDecomposeNodes.clear();
        s_beginNodeArguments.clear();
        s_variableContextStack.clear();

        // Check for function call with component access BEFORE parser validation
        // because muParser doesn't understand this syntax
        if (isFunctionCallWithComponentAccess(expression) ||
            containsFunctionCallWithComponentAccess(expression))
        {
            // We need to create variableNodes first to use the existing parsing function
            std::map<std::string, nodes::NodeId> variableNodes;
            if (!arguments.empty())
            {
                variableNodes = createArgumentNodes(arguments, model);
            }
            else
            {
                // Get variables from the expression
                std::vector<std::string> variables = parser.getVariables();
                variableNodes = createVariableNodes(variables, model, arguments);
            }

            if (variableNodes.empty())
            {
                return 0;
            }

            // For nested cases, we need different handling
            nodes::NodeId result = 0;
            if (isFunctionCallWithComponentAccess(expression))
            {
                result = parseFunctionCallWithComponentAccess(expression, model, variableNodes);
            }
            else if (containsFunctionCallWithComponentAccess(expression))
            {
                // For nested expressions, we need to parse them step by step
                result =
                  parseNestedFunctionCallWithComponentAccess(expression, model, variableNodes);
            }

            if (result != 0)
            {
                // Validate output type and connect to End node
                if (!validateOutputType(model, result, output.type))
                {
                    return 0; // Type validation failed
                }

                if (!connectToEndNode(model, result, output))
                {
                    return 0; // Failed to connect to End node
                }

                return result;
            }
            else
            {
                return 0;
            }
        }

        // Validate expression with the parser before building graph nodes
        bool parseResult = parser.parseExpression(expression);
        bool hasValid = parser.hasValidExpression();

        if (!parseResult)
        {
            return 0; // Invalid expression
        }

        if (!hasValid)
        {
            return 0; // Invalid expression
        }

        // Get variables from the expression (these are in original form like "pos.x")
        std::vector<std::string> variables = parser.getVariables();

        // Create input nodes based on arguments or variables
        std::map<std::string, nodes::NodeId> variableNodes;

        if (!arguments.empty())
        {
            // Use function arguments to create properly typed input nodes
            variableNodes = createArgumentNodes(arguments, model);

            // Ensure argument nodes were created successfully
            if (variableNodes.empty())
            {
                return 0;
            }

            // Note: Component access will be handled later in parseAndBuildGraph
            // when it encounters expressions like "pos.x"
        }
        else
        {
            // Legacy mode: create variable nodes without type information
            variableNodes = createVariableNodes(variables, model, arguments);
        }

        // Parse the expression and build the graph
        nodes::NodeId result = parseAndBuildGraph(expression, model, variableNodes);

        // Ensure parseAndBuildGraph produced a valid node
        if (result == 0)
        {
            // parseAndBuildGraph failed
            return 0;
        }

        // Validate output type and connect to End node
        if (!validateOutputType(model, result, output.type))
        {
            return 0; // Type validation failed
        }

        if (!connectToEndNode(model, result, output))
        {
            return 0; // Failed to connect to End node
        }

        return result;
    }

    bool ExpressionToGraphConverter::canConvertToGraph(std::string const & expression,
                                                       ExpressionParser & parser)
    {
        if (expression.empty())
        {
            return false;
        }

        // Check if it's a function call with component access first
        // This doesn't require parser validation since we handle it specially
        if (isFunctionCallWithComponentAccess(expression))
        {
            return true;
        }

        // Check if the expression contains function calls with component access
        // These expressions need special handling even though muParser can't parse them
        if (containsFunctionCallWithComponentAccess(expression))
        {
            return true;
        }

        // Otherwise, use standard parser validation
        return parser.parseExpression(expression) && parser.hasValidExpression();
    }

    std::map<std::string, nodes::NodeId>
    ExpressionToGraphConverter::createVariableNodes(std::vector<std::string> const & variables,
                                                    nodes::Model & model,
                                                    std::vector<FunctionArgument> const & arguments)
    {
        std::map<std::string, nodes::NodeId> variableNodes;

        for (std::string const & varName : variables)
        {
            // Create a parameter input node for each variable
            // We'll use ConstantScalar nodes that can be used as inputs
            nodes::NodeBase * node = nodes::createNodeFromName("ConstantScalar", model);
            if (node)
            {
                variableNodes[varName] = node->getId();

                // Set the display name to the variable name
                node->setDisplayName(varName);

                // TODO: Set the default value if needed
                // This would require access to the node's parameters
            }
        }

        return variableNodes;
    }

    nodes::NodeId ExpressionToGraphConverter::createMathOperationNode(std::string const & operation,
                                                                      nodes::Model & model)
    {
        nodes::NodeBase * node = nodes::createNodeFromName(operation, model);
        return node ? node->getId() : 0;
    }

    nodes::NodeId ExpressionToGraphConverter::createConstantNode(double value, nodes::Model & model)
    {
        nodes::NodeBase * node = nodes::createNodeFromName("ConstantScalar", model);
        if (node)
        {
            // Set the constant's numeric value on its parameter rather than abusing the display
            // name
            if (auto * param = node->getParameter(nodes::FieldNames::Value))
            {
                // ConstantScalar expects a float value
                param->setValue(static_cast<float>(value));
            }
            // Keep the default display name (e.g., unique node name) for clarity
            return node->getId();
        }
        return 0;
    }

    bool ExpressionToGraphConverter::connectNodes(nodes::Model & model,
                                                  nodes::NodeId fromNodeId,
                                                  std::string const & fromPortName,
                                                  nodes::NodeId toNodeId,
                                                  std::string const & toPortName)
    {
        // Find the source node and get its output port
        auto fromNodeOpt = model.getNode(fromNodeId);
        if (!fromNodeOpt.has_value())
        {
            return false;
        }
        nodes::NodeBase * fromNode = fromNodeOpt.value();

        nodes::Port * outputPort = fromNode->findOutputPort(fromPortName);
        if (!outputPort)
        {
            return false;
        }

        // Find the target node and get its input parameter
        auto toNodeOpt = model.getNode(toNodeId);
        if (!toNodeOpt.has_value())
        {
            return false;
        }
        nodes::NodeBase * toNode = toNodeOpt.value();

        nodes::VariantParameter * inputParam = toNode->getParameter(toPortName);
        if (!inputParam)
        {
            return false;
        }

        // Create the link using the model's addLink method
        return model.addLink(outputPort->getId(), inputParam->getId());
    }

    nodes::NodeId ExpressionToGraphConverter::parseAndBuildGraph(
      std::string const & expression,
      nodes::Model & model,
      std::map<std::string, nodes::NodeId> const & variableNodes)
    {
        std::string cleanExpr = removeWhitespace(expression);

        if (cleanExpr.empty())
        {
            return 0;
        }

        // Handle parentheses - find outermost expression
        if (cleanExpr.front() == '(' && cleanExpr.back() == ')')
        {
            // Check if these parentheses wrap the entire expression
            int depth = 0;
            bool wrapsEntire = true;
            for (size_t i = 0; i < cleanExpr.length() - 1; ++i)
            {
                if (cleanExpr[i] == '(')
                    depth++;
                else if (cleanExpr[i] == ')')
                    depth--;

                if (depth == 0)
                {
                    wrapsEntire = false;
                    break;
                }
            }

            if (wrapsEntire)
            {
                return parseAndBuildGraph(
                  cleanExpr.substr(1, cleanExpr.length() - 2), model, variableNodes);
            }
        }

        // If the entire expression is a number (including a leading negative sign),
        // create a constant node directly. This must happen BEFORE generic unary minus handling
        // so that "-3" becomes a single ConstantScalar with value -3 rather than (-1 * 3).
        try
        {
            // std::stod throws if the whole string is not a valid number
            double value = std::stod(cleanExpr);
            return createConstantNode(value, model);
        }
        catch (...)
        {
            // Not a pure number; continue parsing
        }

        // Handle unary minus operator as a fallback (e.g., "-x", "-(x+y)")
        if (cleanExpr.length() > 1 && cleanExpr[0] == '-')
        {
            // Only treat as unary minus if it is not immediately followed by an operator
            // (numeric literals like "-3" were already handled above).
            std::string innerExpr = cleanExpr.substr(1);
            nodes::NodeId innerNode = parseAndBuildGraph(innerExpr, model, variableNodes);
            if (innerNode == 0)
            {
                return 0; // Failed to parse inner expression
            }

            // Create a multiplication by -1 to represent unary minus
            nodes::NodeId negativeOne = createConstantNode(-1.0, model);
            if (negativeOne == 0)
            {
                return 0; // Failed to create constant
            }

            nodes::NodeId multiplyNode = createMathOperationNode("Multiplication", model);
            if (multiplyNode == 0)
            {
                return 0; // Failed to create multiplication node
            }

            // Connect -1 to first input and inner expression to second input
            std::string negativeOneOutput = getOutputPortName(model, negativeOne);
            std::string innerOutput = getOutputPortName(model, innerNode);

            if (!connectNodes(
                  model, negativeOne, negativeOneOutput, multiplyNode, nodes::FieldNames::A) ||
                !connectNodes(model, innerNode, innerOutput, multiplyNode, nodes::FieldNames::B))
            {
                return 0; // Failed to connect nodes
            }

            return multiplyNode;
        }

        // Check if it's a component access (e.g., "A.x", "pos.y")
        if (isComponentAccess(cleanExpr))
        {
            return parseComponentAccess(cleanExpr, variableNodes, model);
        }

        // Check if it's a function call with component access (e.g., "sin(pos).x")
        if (isFunctionCallWithComponentAccess(cleanExpr))
        {
            return parseFunctionCallWithComponentAccess(cleanExpr, model, variableNodes);
        }

        // Check if it's a preprocessed component access (e.g., "pos_x" -> "pos.x")
        if (isPreprocessedComponentAccess(cleanExpr))
        {
            std::string originalForm = convertPreprocessedToOriginal(cleanExpr);
            return parseComponentAccess(originalForm, variableNodes, model);
        }

        if (cleanExpr == "pi")
        {
            return createConstantNode(3.14159265358979323846, model);
        }
        if (cleanExpr == "e")
        {
            return createConstantNode(2.71828182845904523536, model);
        }

        // Check if it's a variable
        auto varIt = variableNodes.find(cleanExpr);
        if (varIt != variableNodes.end())
        {
            // Set the current variable context for Begin node port resolution
            s_variableContextStack.push_back(cleanExpr);
            return varIt->second;
        }

        // Note: numeric literal case already handled earlier

        // Find the main operator
        auto [operatorPos, operatorChar] = findMainOperator(cleanExpr);

        if (operatorPos == std::string::npos)
        {
            // No operator found, could be a function call
            return parseFunctionCall(cleanExpr, model, variableNodes);
        }

        // Split the expression at the operator
        std::string leftExpr = cleanExpr.substr(0, operatorPos);
        std::string rightExpr = cleanExpr.substr(operatorPos + 1);

        // Recursively build left and right nodes
        nodes::NodeId leftNodeId = parseAndBuildGraph(leftExpr, model, variableNodes);
        // Capture the output port name immediately, before the right sub-expression
        // can overwrite s_componentMap for the same DecomposeVector node
        std::string leftPortName = (leftNodeId != 0) ? getOutputPortName(model, leftNodeId) : "";
        nodes::NodeId rightNodeId = parseAndBuildGraph(rightExpr, model, variableNodes);

        if (leftNodeId == 0 || rightNodeId == 0)
        {
            return 0; // Failed to build sub-expressions
        }

        // Create the operation node
        std::string operationName;
        switch (operatorChar)
        {
        case '+':
            operationName = "Addition";
            break;
        case '-':
            operationName = "Subtraction";
            break;
        case '*':
            operationName = "Multiplication";
            break;
        case '/':
            operationName = "Division";
            break;
        default:
            return 0; // Unsupported operator
        }

        nodes::NodeId operationNodeId = createMathOperationNode(operationName, model);
        if (operationNodeId == 0)
        {
            return 0;
        }

        // Connect the input nodes to the operation node
        std::string rightPortName = getOutputPortName(model, rightNodeId);

        bool leftConnected =
          connectNodes(model, leftNodeId, leftPortName, operationNodeId, nodes::FieldNames::A);
        bool rightConnected =
          connectNodes(model, rightNodeId, rightPortName, operationNodeId, nodes::FieldNames::B);

        if (!leftConnected || !rightConnected)
        {
            // If connection failed, we should still return the operation node
            // as it was created successfully, even if not properly connected
        }

        return operationNodeId;
    }

    nodes::NodeId ExpressionToGraphConverter::parseFunctionCall(
      std::string const & expression,
      nodes::Model & model,
      std::map<std::string, nodes::NodeId> const & variableNodes)
    {
        // Look for function call pattern: functionName(arguments)
        size_t openParen = expression.find('(');
        if (openParen == std::string::npos)
        {
            return 0; // No opening parenthesis found
        }

        if (expression.back() != ')')
        {
            return 0; // No closing parenthesis at the end
        }

        std::string functionName = expression.substr(0, openParen);
        std::string argumentsStr =
          expression.substr(openParen + 1, expression.length() - openParen - 2);

        // Map function names to node types
        std::string nodeTypeName;
        if (functionName == "sin")
            nodeTypeName = "Sine";
        else if (functionName == "cos")
            nodeTypeName = "Cosine";
        else if (functionName == "tan")
            nodeTypeName = "Tangent";
        else if (functionName == "asin")
            nodeTypeName = "ArcSin";
        else if (functionName == "acos")
            nodeTypeName = "ArcCos";
        else if (functionName == "atan")
            nodeTypeName = "ArcTan";
        else if (functionName == "sinh")
            nodeTypeName = "SinH";
        else if (functionName == "cosh")
            nodeTypeName = "CosH";
        else if (functionName == "tanh")
            nodeTypeName = "TanH";
        else if (functionName == "exp")
            nodeTypeName = "Exp";
        else if (functionName == "log")
            nodeTypeName = "Log";
        else if (functionName == "log2")
            nodeTypeName = "Log2";
        else if (functionName == "log10")
            nodeTypeName = "Log10";
        else if (functionName == "sqrt")
            nodeTypeName = "Sqrt";
        else if (functionName == "abs")
            nodeTypeName = "Abs";
        else if (functionName == "sign")
            nodeTypeName = "Sign";
        else if (functionName == "floor")
            nodeTypeName = "Floor";
        else if (functionName == "ceil")
            nodeTypeName = "Ceil";
        else if (functionName == "round")
            nodeTypeName = "Round";
        else if (functionName == "fract")
            nodeTypeName = "Fract";
        else if (functionName == "length")
            nodeTypeName = "Length";
        else if (functionName == "clamp")
            nodeTypeName = "Clamp";
        else if (functionName == "select")
            nodeTypeName = "Select";
        else
        {
            // Handle binary functions
            if (functionName == "pow")
                nodeTypeName = "Pow";
            else if (functionName == "atan2")
                nodeTypeName = "ArcTan2";
            else if (functionName == "fmod")
                nodeTypeName = "Fmod";
            else if (functionName == "mod")
                nodeTypeName = "Mod";
            else if (functionName == "min")
                nodeTypeName = "Min";
            else if (functionName == "max")
                nodeTypeName = "Max";
            else
            {
                return 0; // Unsupported function
            }
        }

        // Create the function node
        nodes::NodeId functionNodeId = createMathOperationNode(nodeTypeName, model);
        if (functionNodeId == 0)
        {
            return 0;
        }

        // Parse arguments
        std::vector<std::string> arguments = parseArgumentList(argumentsStr);

        // Connect arguments based on function type
        if (isSingleArgumentFunction(functionName))
        {
            if (arguments.size() != 1)
            {
                return 0; // Wrong number of arguments
            }

            nodes::NodeId argNodeId = parseAndBuildGraph(arguments[0], model, variableNodes);
            if (argNodeId == 0)
            {
                return 0;
            }

            std::string argPortName = getOutputPortName(model, argNodeId);
            connectNodes(model, argNodeId, argPortName, functionNodeId, nodes::FieldNames::A);
        }
        else if (isBinaryFunction(functionName))
        {
            if (arguments.size() != 2)
            {
                return 0; // Wrong number of arguments
            }

            nodes::NodeId arg1NodeId = parseAndBuildGraph(arguments[0], model, variableNodes);
            // Capture port name before parsing the next argument: parseAndBuildGraph may
            // overwrite s_componentMap entries, making a deferred getOutputPortName call
            // return the wrong component for a DecomposeVector node.
            std::string arg1PortName = (arg1NodeId != 0) ? getOutputPortName(model, arg1NodeId) : "";
            nodes::NodeId arg2NodeId = parseAndBuildGraph(arguments[1], model, variableNodes);

            if (arg1NodeId == 0 || arg2NodeId == 0)
            {
                return 0;
            }

            std::string arg2PortName = getOutputPortName(model, arg2NodeId);

            connectNodes(model, arg1NodeId, arg1PortName, functionNodeId, nodes::FieldNames::A);
            connectNodes(model, arg2NodeId, arg2PortName, functionNodeId, nodes::FieldNames::B);
        }
        else if (isTernaryFunction(functionName))
        {
            if (arguments.size() != 3)
            {
                return 0; // Wrong number of arguments
            }

            nodes::NodeId arg1NodeId = parseAndBuildGraph(arguments[0], model, variableNodes);
            // Same ordering constraint as the binary case: capture each port name
            // immediately after parsing its argument, before the next parseAndBuildGraph
            // call can mutate s_componentMap.
            std::string arg1PortName = (arg1NodeId != 0) ? getOutputPortName(model, arg1NodeId) : "";
            nodes::NodeId arg2NodeId = parseAndBuildGraph(arguments[1], model, variableNodes);
            std::string arg2PortName = (arg2NodeId != 0) ? getOutputPortName(model, arg2NodeId) : "";
            nodes::NodeId arg3NodeId = parseAndBuildGraph(arguments[2], model, variableNodes);

            if (arg1NodeId == 0 || arg2NodeId == 0 || arg3NodeId == 0)
            {
                return 0;
            }

            std::string arg3PortName = getOutputPortName(model, arg3NodeId);

            // For clamp: clamp(value, min, max) -> A=value, Min=min, Max=max
            connectNodes(model, arg1NodeId, arg1PortName, functionNodeId, nodes::FieldNames::A);
            connectNodes(model, arg2NodeId, arg2PortName, functionNodeId, nodes::FieldNames::Min);
            connectNodes(model, arg3NodeId, arg3PortName, functionNodeId, nodes::FieldNames::Max);
        }
        else if (functionName == "select")
        {
            if (arguments.size() != 4)
            {
                return 0; // Wrong number of arguments
            }

            nodes::NodeId arg1NodeId = parseAndBuildGraph(arguments[0], model, variableNodes);
            // Same ordering constraint: capture each port name before the next argument
            // is parsed so s_componentMap is not yet overwritten.
            std::string arg1PortName = (arg1NodeId != 0) ? getOutputPortName(model, arg1NodeId) : "";
            nodes::NodeId arg2NodeId = parseAndBuildGraph(arguments[1], model, variableNodes);
            std::string arg2PortName = (arg2NodeId != 0) ? getOutputPortName(model, arg2NodeId) : "";
            nodes::NodeId arg3NodeId = parseAndBuildGraph(arguments[2], model, variableNodes);
            std::string arg3PortName = (arg3NodeId != 0) ? getOutputPortName(model, arg3NodeId) : "";
            nodes::NodeId arg4NodeId = parseAndBuildGraph(arguments[3], model, variableNodes);

            if (arg1NodeId == 0 || arg2NodeId == 0 || arg3NodeId == 0 || arg4NodeId == 0)
            {
                return 0;
            }

            std::string arg4PortName = getOutputPortName(model, arg4NodeId);

            connectNodes(model, arg1NodeId, arg1PortName, functionNodeId, nodes::FieldNames::A);
            connectNodes(model, arg2NodeId, arg2PortName, functionNodeId, nodes::FieldNames::B);
            connectNodes(model, arg3NodeId, arg3PortName, functionNodeId, nodes::FieldNames::C);
            connectNodes(model, arg4NodeId, arg4PortName, functionNodeId, nodes::FieldNames::D);
        }
        else
        {
            return 0; // Unsupported function type
        }

        return functionNodeId;
    }

    std::vector<std::string>
    ExpressionToGraphConverter::parseArgumentList(std::string const & argumentsStr)
    {
        std::vector<std::string> arguments;

        if (argumentsStr.empty())
        {
            return arguments;
        }

        std::string cleanArgs = removeWhitespace(argumentsStr);
        std::string currentArg;
        int depth = 0;

        for (char c : cleanArgs)
        {
            if (c == ',' && depth == 0)
            {
                if (!currentArg.empty())
                {
                    arguments.push_back(currentArg);
                    currentArg.clear();
                }
            }
            else
            {
                currentArg += c;
                if (c == '(')
                    depth++;
                else if (c == ')')
                    depth--;
            }
        }

        if (!currentArg.empty())
        {
            arguments.push_back(currentArg);
        }

        return arguments;
    }

    bool ExpressionToGraphConverter::isSingleArgumentFunction(std::string const & functionName)
    {
        static const std::set<std::string> singleArgFunctions = {
          "sin",  "cos",  "tan",   "asin", "acos",   "atan", "sinh",  "cosh",  "tanh",   "exp",
          "log",  "log2", "log10", "sqrt", "abs",    "sign", "floor", "ceil",  "round",  "fract",
          "length"};

        return singleArgFunctions.find(functionName) != singleArgFunctions.end();
    }

    bool ExpressionToGraphConverter::isBinaryFunction(std::string const & functionName)
    {
        static const std::set<std::string> binaryFunctions = {
          "pow", "atan2", "fmod", "mod", "min", "max"};

        return binaryFunctions.find(functionName) != binaryFunctions.end();
    }

    bool ExpressionToGraphConverter::isTernaryFunction(std::string const & functionName)
    {
        static const std::set<std::string> ternaryFunctions = {"clamp"};

        return ternaryFunctions.find(functionName) != ternaryFunctions.end();
    }

    std::string ExpressionToGraphConverter::removeWhitespace(std::string const & expr)
    {
        std::string result;
        result.reserve(expr.length());

        for (char c : expr)
        {
            if (!std::isspace(c))
            {
                result += c;
            }
        }

        return result;
    }

    std::pair<size_t, char> ExpressionToGraphConverter::findMainOperator(std::string const & expr)
    {
        int depth = 0;
        int minPrecedence = INT_MAX;
        size_t operatorPos = std::string::npos;
        char operatorChar = 0;

        // Scan from right to left to handle left-associative operators correctly
        for (int i = static_cast<int>(expr.length()) - 1; i >= 0; --i)
        {
            char c = expr[i];

            if (c == ')')
                depth++;
            else if (c == '(')
                depth--;
            else if (depth == 0 && isOperator(c))
            {
                // Skip unary minus occurrences (e.g., at the beginning or after another
                // operator/paren)
                if (c == '-')
                {
                    if (i == 0)
                    {
                        continue; // leading '-' is unary
                    }
                    char prev = expr[i - 1];
                    if (prev == '(' || isOperator(prev))
                    {
                        continue; // unary minus; not a binary operator to split on
                    }
                }
                int precedence = getOperatorPrecedence(c);
                if (precedence <= minPrecedence)
                {
                    minPrecedence = precedence;
                    operatorPos = i;
                    operatorChar = c;
                }
            }
        }

        return {operatorPos, operatorChar};
    }

    std::string ExpressionToGraphConverter::getOutputPortName(nodes::Model & model,
                                                              nodes::NodeId nodeId)
    {
        auto nodeOpt = model.getNode(nodeId);
        if (!nodeOpt.has_value())
        {
            return nodes::FieldNames::Value; // Default fallback
        }
        nodes::NodeBase * node = nodeOpt.value();

        // Check if this is a Begin node - if so, use the current variable context
        if (dynamic_cast<nodes::Begin *>(node) != nullptr)
        {
            if (!s_variableContextStack.empty())
            {
                // Clear the context after use
                std::string context = s_variableContextStack.back();
                s_variableContextStack.pop_back();
                return context; // Return the argument name as the port name
            }
            return nodes::FieldNames::Value; // Fallback
        }

        // Check if this is a DecomposeVector node with component information
        auto componentIt = s_componentMap.find(nodeId);
        if (componentIt != s_componentMap.end())
        {
            std::string const & component = componentIt->second;
            if (component == "x")
                return nodes::FieldNames::X;
            else if (component == "y")
                return nodes::FieldNames::Y;
            else if (component == "z")
                return nodes::FieldNames::Z;
        }

        // Check if the node has a "Result" output port (math operations)
        if (node->findOutputPort(nodes::FieldNames::Result))
        {
            return nodes::FieldNames::Result;
        }

        // Check if the node has a "Vector" output port (ConstantVector nodes)
        if (node->findOutputPort(nodes::FieldNames::Vector))
        {
            return nodes::FieldNames::Vector;
        }

        // Otherwise, assume it has a "Value" output port (constants, variables)
        return nodes::FieldNames::Value;
    }

    bool ExpressionToGraphConverter::isOperator(char c)
    {
        return c == '+' || c == '-' || c == '*' || c == '/';
    }

    int ExpressionToGraphConverter::getOperatorPrecedence(char op)
    {
        switch (op)
        {
        case '+':
        case '-':
            return 1;
        case '*':
        case '/':
            return 2;
        default:
            return 0;
        }
    }

    std::map<std::string, nodes::NodeId>
    ExpressionToGraphConverter::createArgumentNodes(std::vector<FunctionArgument> const & arguments,
                                                    nodes::Model & model)
    {
        std::map<std::string, nodes::NodeId> argumentNodes;

        // Ensure Begin/End nodes exist for function arguments
        if (!model.getBeginNode())
        {
            model.createBeginEnd();
        }

        nodes::Begin * beginNode = model.getBeginNode();

        for (auto const & arg : arguments)
        {
            if (arg.type == ArgumentType::Scalar)
            {
                // Add scalar argument to Begin node
                nodes::VariantParameter parameter(float{0.0f});
                model.addArgument(arg.name, parameter);
            }
            else if (arg.type == ArgumentType::Vector)
            {
                // Add vector argument to Begin node
                nodes::VariantParameter parameter(nodes::float3{0.0f, 0.0f, 0.0f});
                model.addArgument(arg.name, parameter);
            }

            // Store the Begin node ID as the source for this argument
            // and track the argument type for port resolution
            argumentNodes[arg.name] = beginNode->getId();
            s_beginNodeArguments[arg.name] = arg.type; // Store argument type
        }

        // Register outputs and update node IDs after adding all arguments
        if (!arguments.empty())
        {
            model.registerOutputs(*beginNode);
            beginNode->updateNodeIds();
        }

        return argumentNodes;
    }

    nodes::NodeId ExpressionToGraphConverter::parseComponentAccess(
      std::string const & expression,
      std::map<std::string, nodes::NodeId> const & argumentNodes,
      nodes::Model & model)
    {
        auto [argName, component] = parseComponentExpression(expression);

        if (argName.empty() || component.empty())
        {
            return 0; // Invalid component access
        }

        // Find the argument node
        auto it = argumentNodes.find(argName);
        if (it == argumentNodes.end())
        {
            return 0; // Argument not found
        }

        // Validate that the argument node is a vector type (not scalar)
        auto nodeOpt = model.getNode(it->second);
        if (!nodeOpt.has_value())
        {
            return 0; // Node not found
        }

        nodes::NodeBase * argNode = nodeOpt.value();
        std::string nodeTypeName = argNode->name();

        // Allow component access on vector nodes (ConstantVector) or Begin nodes with vector
        // arguments
        bool isVector = (nodeTypeName.find("ConstantVector") != std::string::npos);
        bool isBeginNode =
          (nodeTypeName == "Input" || dynamic_cast<nodes::Begin *>(argNode) != nullptr);

        if (!isVector && !isBeginNode)
        {
            return 0; // Component access not allowed on non-vector types
        }

        // For Begin nodes, we need to verify the argument is actually a vector type
        if (isBeginNode)
        {
            // Check if this argument was registered as a vector argument
            auto argIt = s_beginNodeArguments.find(argName);
            if (argIt == s_beginNodeArguments.end())
            {
                return 0; // Argument not found in Begin node arguments
            }
            if (argIt->second != ArgumentType::Vector)
            {
                return 0; // Argument is not a vector type, component access not allowed
            }
        }

        // Check if we already have a DecomposeVector node for this vector
        auto decomposeIt = s_vectorDecomposeNodes.find(argName);
        nodes::NodeBase * decomposeNode = nullptr;

        if (decomposeIt != s_vectorDecomposeNodes.end())
        {
            // Reuse existing DecomposeVector node
            auto nodeOpt = model.getNode(decomposeIt->second);
            if (nodeOpt.has_value())
            {
                decomposeNode = nodeOpt.value();
            }
        }

        if (!decomposeNode)
        {
            // Create a new DecomposeVector node
            decomposeNode = nodes::createNodeFromName("DecomposeVector", model);
            if (!decomposeNode)
            {
                return 0; // Failed to create decompose node
            }

            // Connect the vector argument to the decompose node
            // For Begin nodes, set the context so getOutputPortName knows which argument to use
            if (isBeginNode)
            {
                s_variableContextStack.push_back(argName);
            }

            if (!connectNodes(model,
                              it->second,
                              getOutputPortName(model, it->second),
                              decomposeNode->getId(),
                              nodes::FieldNames::A))
            {
                return 0; // Failed to connect
            }

            // Store the DecomposeVector node for reuse
            s_vectorDecomposeNodes[argName] = decomposeNode->getId();
        }

        // Create a unique identifier for this component access
        std::string componentAccessKey = expression; // Use the full expression like "pos.x"

        // Map this DecomposeVector node to the specific component
        // This will be used later when connecting nodes
        s_componentMap[decomposeNode->getId()] = component;

        // Return the DecomposeVector node directly
        return decomposeNode->getId();
    }

    bool ExpressionToGraphConverter::isComponentAccess(std::string const & expression)
    {
        // Check if it contains a dot, but is not at start or end
        size_t dotPos = expression.find('.');
        if (dotPos == std::string::npos || dotPos == 0 || dotPos == expression.length() - 1)
        {
            return false;
        }

        // Check if the expression is ONLY a component access (variable.component)
        // This means no operators should be present
        std::string operatorChars = "+-*/()";
        for (char op : operatorChars)
        {
            if (expression.find(op) != std::string::npos)
            {
                return false; // Contains operators, so it's not a simple component access
            }
        }

        // Check if there's exactly one dot
        if (expression.find('.', dotPos + 1) != std::string::npos)
        {
            return false; // Multiple dots
        }

        // Check if the part before the dot is a valid identifier
        std::string varName = expression.substr(0, dotPos);
        if (varName.empty() || !std::isalpha(varName[0]))
        {
            return false;
        }

        // Check if the part after the dot is a valid component (x, y, or z)
        std::string component = expression.substr(dotPos + 1);
        return component == "x" || component == "y" || component == "z";
    }

    std::pair<std::string, std::string>
    ExpressionToGraphConverter::parseComponentExpression(std::string const & expression)
    {
        size_t dotPos = expression.find('.');
        if (dotPos == std::string::npos || dotPos == 0 || dotPos == expression.length() - 1)
        {
            return {"", ""}; // Invalid format
        }

        std::string argName = expression.substr(0, dotPos);
        std::string component = expression.substr(dotPos + 1);

        // Validate component name
        if (component != "x" && component != "y" && component != "z")
        {
            return {"", ""}; // Invalid component
        }

        return {argName, component};
    }

    bool ExpressionToGraphConverter::isPreprocessedComponentAccess(std::string const & expression)
    {
        // Check if it matches pattern: identifier_[xyz]
        std::regex preprocessedRegex(R"([a-zA-Z][a-zA-Z0-9_]*_[xyz])");
        return std::regex_match(expression, preprocessedRegex);
    }

    std::string
    ExpressionToGraphConverter::convertPreprocessedToOriginal(std::string const & expression)
    {
        // Convert pos_x -> pos.x
        size_t underscorePos = expression.rfind('_');
        if (underscorePos != std::string::npos && underscorePos > 0 &&
            underscorePos < expression.length() - 1)
        {
            char component = expression[underscorePos + 1];
            if (component == 'x' || component == 'y' || component == 'z')
            {
                std::string argName = expression.substr(0, underscorePos);
                return argName + "." + component;
            }
        }
        return expression; // Return unchanged if not valid preprocessed form
    }

    nodes::NodeBase *
    ExpressionToGraphConverter::createComponentExtractorNode(std::string const & component,
                                                             nodes::Model & model)
    {
        // Create a simple pass-through node using multiplication by 1.0
        // This effectively creates a node that takes one input and outputs the same value
        nodes::NodeBase * multiplyNode = nodes::createNodeFromName("Multiplication", model);
        if (!multiplyNode)
        {
            return nullptr;
        }

        // Create a constant node with value 1.0 for the second input
        nodes::NodeId constantNodeId = createConstantNode(1.0, model);
        if (constantNodeId == 0)
        {
            return nullptr;
        }

        // Connect the constant 1.0 to the second input of the multiplication node
        if (!connectNodes(model,
                          constantNodeId,
                          nodes::FieldNames::Value,
                          multiplyNode->getId(),
                          nodes::FieldNames::B))
        {
            return nullptr;
        }

        return multiplyNode;
    }

    bool ExpressionToGraphConverter::validateOutputType(nodes::Model & model,
                                                        nodes::NodeId resultNodeId,
                                                        ArgumentType expectedType)
        {
            auto nodeOpt = model.getNode(resultNodeId);
            if (!nodeOpt.has_value())
            {
                return false; // Node not found
            }

            nodes::NodeBase * node = nodeOpt.value();
            std::string const outputPortName = getOutputPortName(model, resultNodeId);
            nodes::Port * outputPort = node->findOutputPort(outputPortName);
            if (outputPort == nullptr)
            {
                return false; // Unable to determine output port
            }

            std::type_index const portType = outputPort->getTypeIndex();
            ArgumentType actualType = ArgumentType::Scalar;
            if (portType == std::type_index(typeid(nodes::float3)))
            {
                actualType = ArgumentType::Vector;
            }

            // If the caller explicitly expects a vector but the expression produces a scalar,
            // treat this as an error. Otherwise allow the conversion and let downstream logic
            // adapt the End node to the actual output type.
            if (expectedType == ArgumentType::Vector && actualType != ArgumentType::Vector)
            {
                return false;
            }

            return true;
        }

    bool ExpressionToGraphConverter::connectToEndNode(nodes::Model & model,
                                                      nodes::NodeId resultNodeId,
                                                      FunctionOutput const & output)
    {
        // Ensure End node exists
        nodes::End * endNode = model.getEndNode();
        if (!endNode)
        {
            // Create Begin/End nodes if they don't exist
            model.createBeginEnd();
            endNode = model.getEndNode();
            if (!endNode)
            {
                return false; // Failed to create End node
            }
        }

        // Inspect the result node output type to determine the correct End node parameter
        auto resultNodeOpt = model.getNode(resultNodeId);
        if (!resultNodeOpt.has_value())
        {
            return false;
        }

        nodes::NodeBase * resultNode = resultNodeOpt.value();
        std::string resultPortName = getOutputPortName(model, resultNodeId);
        nodes::Port * resultPort = resultNode->findOutputPort(resultPortName);
        if (resultPort == nullptr)
        {
            return false;
        }

        ArgumentType actualType = ArgumentType::Scalar;
        std::type_index const resultType = resultPort->getTypeIndex();
        if (resultType == std::type_index(typeid(nodes::float3)))
        {
            actualType = ArgumentType::Vector;
        }

        ArgumentType parameterType = output.type;
        if (parameterType != actualType)
        {
            parameterType = actualType;
        }

        if (parameterType == ArgumentType::Vector)
        {
            nodes::VariantParameter parameter(nodes::float3{0.0f, 0.0f, 0.0f});
            model.addFunctionOutput(output.name, parameter);
        }
        else
        {
            nodes::VariantParameter parameter(float{0.0f});
            model.addFunctionOutput(output.name, parameter);
        }

        // Connect the result node to the End node's input parameter
        return connectNodes(model, resultNodeId, resultPortName, endNode->getId(), output.name);
    }

    bool
    ExpressionToGraphConverter::isFunctionCallWithComponentAccess(std::string const & expression)
    {
        // Look for pattern: functionName(...).component
        // e.g., "sin(pos).x", "cos(velocity).y"

        // Find the last dot in the expression
        size_t lastDot = expression.rfind('.');
        if (lastDot == std::string::npos)
        {
            return false; // No dot found
        }

        // Check if what follows the dot is a valid component
        std::string component = expression.substr(lastDot + 1); // Skip the dot itself
        if (component != "x" && component != "y" && component != "z")
        {
            return false; // Not a valid component
        }

        // Check if what precedes the dot looks like a function call
        std::string functionPart = expression.substr(0, lastDot);

        // Simple check: contains parentheses and ends with ')'
        return functionPart.find('(') != std::string::npos && functionPart.back() == ')';
    }

    bool ExpressionToGraphConverter::containsFunctionCallWithComponentAccess(
      std::string const & expression)
    {
        // Look for patterns like function(...).x anywhere in the expression
        std::vector<std::string> components = {"x", "y", "z"};

        for (const auto & component : components)
        {
            std::string pattern = "." + component;
            size_t pos = 0;

            while ((pos = expression.find(pattern, pos)) != std::string::npos)
            {
                // Found a component access, now check if it's preceded by a function call
                if (pos > 0)
                {
                    // Look backwards to find the matching closing parenthesis
                    size_t closeParen = pos;
                    while (closeParen > 0 && expression[closeParen - 1] != ')')
                    {
                        closeParen--;
                    }

                    if (closeParen > 0 && expression[closeParen - 1] == ')')
                    {
                        // Found a closing paren, now look for the matching opening paren and
                        // function name
                        int parenCount = 1;
                        size_t openParen = closeParen - 1;

                        while (openParen > 0 && parenCount > 0)
                        {
                            openParen--;
                            if (expression[openParen] == ')')
                            {
                                parenCount++;
                            }
                            else if (expression[openParen] == '(')
                            {
                                parenCount--;
                            }
                        }

                        if (parenCount == 0)
                        {
                            // Found matching parentheses, check if there's a function name before
                            // the opening paren
                            if (openParen > 0)
                            {
                                size_t funcStart = openParen;
                                while (funcStart > 0 && (std::isalnum(expression[funcStart - 1]) ||
                                                         expression[funcStart - 1] == '_'))
                                {
                                    funcStart--;
                                }

                                if (funcStart < openParen)
                                {
                                    // Found a function name, this is a function call with component
                                    // access
                                    return true;
                                }
                            }
                        }
                    }
                }

                pos += pattern.length();
            }
        }

        return false;
    }

    nodes::NodeId ExpressionToGraphConverter::parseFunctionCallWithComponentAccess(
      std::string const & expression,
      nodes::Model & model,
      std::map<std::string, nodes::NodeId> const & variableNodes)
    {
        // Split the expression into function call and component
        size_t lastDot = expression.rfind('.');
        if (lastDot == std::string::npos)
        {
            return 0; // Should not happen if isFunctionCallWithComponentAccess returned true
        }

        std::string functionPart = expression.substr(0, lastDot);
        std::string component = expression.substr(lastDot + 1); // Skip the dot

        // First, parse the function call to get the vector result
        nodes::NodeId functionResult = parseFunctionCall(functionPart, model, variableNodes);
        if (functionResult == 0)
        {
            return 0; // Function call parsing failed
        }

        // Create a DecomposeVector node to extract the component
        nodes::NodeBase * decomposeNode = nodes::createNodeFromName("DecomposeVector", model);
        if (!decomposeNode)
        {
            return 0; // Failed to create DecomposeVector node
        }

        // Connect the function result to the DecomposeVector node's input
        std::string functionOutputPort = getOutputPortName(model, functionResult);
        if (!connectNodes(model,
                          functionResult,
                          functionOutputPort,
                          decomposeNode->getId(),
                          nodes::FieldNames::A))
        {
            return 0; // Failed to connect
        }

        // Return the appropriate output port of the DecomposeVector node
        // The DecomposeVector node should have outputs named "x", "y", "z"
        nodes::NodeId decomposeId = decomposeNode->getId();

        // For now, we'll return the DecomposeVector node ID and assume the system
        // will use the correct output port based on the component name
        // Store the component mapping for later use
        s_componentMap[decomposeId] = component;

        return decomposeId;
    }

    nodes::NodeId ExpressionToGraphConverter::parseNestedFunctionCallWithComponentAccess(
      std::string const & expression,
      nodes::Model & model,
      std::map<std::string, nodes::NodeId> const & variableNodes)
    {
        // For nested expressions, we need to recursively parse the sub-expressions
        // that contain function calls with component access

        // We'll use a different strategy: manually parse the expression and build the graph
        return parseComplexExpression(expression, model, variableNodes);
    }

    std::string
    ExpressionToGraphConverter::preprocessComponentAccess(std::string const & expression)
    {
        // This method is no longer needed with the new approach
        return expression;
    }

    nodes::NodeId ExpressionToGraphConverter::parseComplexExpression(
      std::string const & expression,
      nodes::Model & model,
      std::map<std::string, nodes::NodeId> const & variableNodes)
    {
        // Handle complex expressions that contain function calls with component access
        // Strategy: Find function calls with component access and recursively build the graph

        // First, check if this is a simple function call with component access
        if (isFunctionCallWithComponentAccess(expression))
        {
            return parseFunctionCallWithComponentAccess(expression, model, variableNodes);
        }

        // Find function calls with component access using proper parentheses matching
        std::vector<std::tuple<std::string, size_t, size_t>> functionMatches;

        // Look for pattern: identifier(...)\.component
        std::regex pattern(R"([a-zA-Z_][a-zA-Z0-9_]*\s*\()");
        std::sregex_iterator iter(expression.begin(), expression.end(), pattern);
        std::sregex_iterator end;

        for (; iter != end; ++iter)
        {
            const std::smatch & match = *iter;
            size_t start = match.position();
            size_t openParen = start + match.length() - 1;

            // Find the matching closing parenthesis
            int depth = 1;
            size_t closeParen = openParen + 1;
            while (closeParen < expression.length() && depth > 0)
            {
                if (expression[closeParen] == '(')
                {
                    depth++;
                }
                else if (expression[closeParen] == ')')
                {
                    depth--;
                }
                closeParen++;
            }

            if (depth == 0)
            {
                closeParen--; // Point to the actual closing parenthesis

                // Check if followed by .x, .y, or .z
                if (closeParen + 2 < expression.length() && expression[closeParen + 1] == '.' &&
                    (expression[closeParen + 2] == 'x' || expression[closeParen + 2] == 'y' ||
                     expression[closeParen + 2] == 'z'))
                {

                    std::string fullMatch = expression.substr(start, closeParen + 3 - start);
                    functionMatches.emplace_back(fullMatch, start, closeParen + 3);
                }
            }
        }

        if (functionMatches.empty())
        {
            // No function calls with component access found, use regular parsing
            return parseAndBuildGraph(expression, model, variableNodes);
        }

        // Process function calls from right to left to handle nested cases properly
        std::string workingExpression = expression;
        std::map<std::string, nodes::NodeId> substitutions;
        int substitutionCounter = 0;

        // Sort matches by position in reverse order
        std::sort(functionMatches.begin(),
                  functionMatches.end(),
                  [](const auto & a, const auto & b) { return std::get<1>(a) > std::get<1>(b); });

        for (auto & [fullMatch, start, endPos] : functionMatches)
        {
            // Parse the function call components
            size_t openParen = fullMatch.find('(');
            size_t closeParen = fullMatch.rfind(')');
            size_t dot = fullMatch.rfind('.');

            std::string functionName = fullMatch.substr(0, openParen);
            std::string argument = fullMatch.substr(openParen + 1, closeParen - openParen - 1);
            std::string component = fullMatch.substr(dot + 1);

            // First, recursively parse the argument (which might contain more function calls)
            nodes::NodeId argumentNode = parseComplexExpression(argument, model, variableNodes);
            if (argumentNode == 0)
            {
                return 0; // Failed to parse argument
            }

            // Create the function call node using the same logic as parseFunctionCall
            std::string nodeTypeName;
            if (functionName == "sin")
                nodeTypeName = "Sine";
            else if (functionName == "cos")
                nodeTypeName = "Cosine";
            else if (functionName == "tan")
                nodeTypeName = "Tangent";
            else if (functionName == "asin")
                nodeTypeName = "ArcSin";
            else if (functionName == "acos")
                nodeTypeName = "ArcCos";
            else if (functionName == "atan")
                nodeTypeName = "ArcTan";
            else if (functionName == "sinh")
                nodeTypeName = "SinH";
            else if (functionName == "cosh")
                nodeTypeName = "CosH";
            else if (functionName == "tanh")
                nodeTypeName = "TanH";
            else if (functionName == "exp")
                nodeTypeName = "Exp";
            else if (functionName == "log")
                nodeTypeName = "Log";
            else if (functionName == "log2")
                nodeTypeName = "Log2";
            else if (functionName == "log10")
                nodeTypeName = "Log10";
            else if (functionName == "sqrt")
                nodeTypeName = "Sqrt";
            else if (functionName == "abs")
                nodeTypeName = "Abs";
            else if (functionName == "sign")
                nodeTypeName = "Sign";
            else if (functionName == "floor")
                nodeTypeName = "Floor";
            else if (functionName == "ceil")
                nodeTypeName = "Ceil";
            else if (functionName == "round")
                nodeTypeName = "Round";
            else if (functionName == "fract")
                nodeTypeName = "Fract";
            else if (functionName == "length")
                nodeTypeName = "Length";
            else if (functionName == "pow")
                nodeTypeName = "Pow";
            else if (functionName == "atan2")
                nodeTypeName = "ArcTan2";
            else if (functionName == "fmod")
                nodeTypeName = "Fmod";
            else if (functionName == "mod")
                nodeTypeName = "Mod";
            else if (functionName == "min")
                nodeTypeName = "Min";
            else if (functionName == "max")
                nodeTypeName = "Max";
            else if (functionName == "clamp")
                nodeTypeName = "Clamp";
            else
            {
                return 0; // Unsupported function
            }

            nodes::NodeId functionNodeId = createMathOperationNode(nodeTypeName, model);
            if (functionNodeId == 0)
            {
                return 0;
            }

            // Connect the argument to the function
            std::string argumentOutputPort = getOutputPortName(model, argumentNode);
            if (!connectNodes(
                  model, argumentNode, argumentOutputPort, functionNodeId, nodes::FieldNames::A))
            {
                return 0; // Failed to connect
            }

            // Create a DecomposeVector node to extract the component
            nodes::NodeBase * decomposeNode = nodes::createNodeFromName("DecomposeVector", model);
            if (!decomposeNode)
            {
                return 0; // Failed to create DecomposeVector node
            }

            // Connect the function result to the DecomposeVector node
            std::string functionOutputPort = getOutputPortName(model, functionNodeId);
            if (!connectNodes(model,
                              functionNodeId,
                              functionOutputPort,
                              decomposeNode->getId(),
                              nodes::FieldNames::A))
            {
                return 0; // Failed to connect
            }

            // Store the component mapping
            s_componentMap[decomposeNode->getId()] = component;

            // Create a unique placeholder
            std::string placeholder = "SUB" + std::to_string(substitutionCounter++);

            // Store the substitution
            substitutions[placeholder] = decomposeNode->getId();

            // Replace the function call with the placeholder in the working expression
            workingExpression.replace(start, endPos - start, placeholder);
        }

        // Now parse the remaining expression with the substitutions
        std::map<std::string, nodes::NodeId> extendedVariableNodes = variableNodes;
        for (auto const & sub : substitutions)
        {
            extendedVariableNodes[sub.first] = sub.second;
        }
        return parseAndBuildGraph(workingExpression, model, extendedVariableNodes);
    }

    namespace
    {
        /// Count how many times each node is referenced as a source by other nodes' parameters.
        std::map<nodes::NodeId, int>
        countFanOut(nodes::Model & model)
        {
            std::map<nodes::NodeId, int> fanOut;
            for (auto it = model.begin(); it != model.end(); ++it)
            {
                for (auto const & [paramName, param] : it->second->parameter())
                {
                    auto const & src = param.getConstSource();
                    if (src.has_value())
                    {
                        fanOut[src->nodeId]++;
                    }
                }
            }
            return fanOut;
        }

        /// Map node type name to the snippet function/operator representation.
        /// Returns empty string for types that need special handling (Begin, End, Constant, etc.).
        struct BinaryOp
        {
            char op;
        };
        struct UnaryFunc
        {
            std::string name;
        };
        struct BinaryFunc
        {
            std::string name;
        };
        struct TernaryFunc
        {
            std::string name;
        };

        std::string nodeTypeToFunctionName(std::string const & nodeType)
        {
            // Unary functions
            if (nodeType == "Sine") return "sin";
            if (nodeType == "Cosine") return "cos";
            if (nodeType == "Tangent") return "tan";
            if (nodeType == "ArcSin") return "asin";
            if (nodeType == "ArcCos") return "acos";
            if (nodeType == "ArcTan") return "atan";
            if (nodeType == "SinH") return "sinh";
            if (nodeType == "CosH") return "cosh";
            if (nodeType == "TanH") return "tanh";
            if (nodeType == "Exp") return "exp";
            if (nodeType == "Log") return "log";
            if (nodeType == "Log2") return "log2";
            if (nodeType == "Log10") return "log10";
            if (nodeType == "Sqrt") return "sqrt";
            if (nodeType == "Abs") return "abs";
            if (nodeType == "Sign") return "sign";
            if (nodeType == "Floor") return "floor";
            if (nodeType == "Ceil") return "ceil";
            if (nodeType == "Round") return "round";
            if (nodeType == "Fract") return "fract";
            if (nodeType == "Length") return "length";
            // Binary functions
            if (nodeType == "Pow") return "pow";
            if (nodeType == "ArcTan2") return "atan2";
            if (nodeType == "Fmod" || nodeType == "Mod") return "mod";
            if (nodeType == "Min") return "min";
            if (nodeType == "Max") return "max";
            // Ternary
            if (nodeType == "Clamp") return "clamp";
            // Quaternary
            if (nodeType == "Select") return "select";
            return {};
        }

        bool isUnaryNodeType(std::string const & nodeType)
        {
            static std::set<std::string> const types = {
              "Sine", "Cosine", "Tangent", "ArcSin", "ArcCos", "ArcTan",
              "SinH", "CosH", "TanH", "Exp", "Log", "Log2", "Log10",
              "Sqrt", "Abs", "Sign", "Floor", "Ceil", "Round", "Fract", "Length"};
            return types.count(nodeType) > 0;
        }

        bool isBinaryOperatorNodeType(std::string const & nodeType)
        {
            return nodeType == "Addition" || nodeType == "Subtraction" ||
                   nodeType == "Multiplication" || nodeType == "Division";
        }

        char binaryOperatorChar(std::string const & nodeType)
        {
            if (nodeType == "Addition") return '+';
            if (nodeType == "Subtraction") return '-';
            if (nodeType == "Multiplication") return '*';
            if (nodeType == "Division") return '/';
            return '?';
        }

        int operatorPrecedence(std::string const & nodeType)
        {
            if (nodeType == "Addition" || nodeType == "Subtraction") return 1;
            if (nodeType == "Multiplication" || nodeType == "Division") return 2;
            return 0;
        }

        /// Get the expression string for the node at the given source,
        /// looking up nodeId and port name from a Source struct.
        /// Returns the variable name if the node was already assigned one,
        /// or recurses to build the inline expression.
        ///
        /// Forward-declared because nodeToExpression and sourceExpression are
        /// mutually recursive: nodeToExpression generates the expression for a
        /// node; sourceExpression follows the input link from a parameter to
        /// reach the source node.
        std::string nodeToExpression(
          nodes::Model & model,
          nodes::NodeId nodeId,
          std::string const & portName,
          std::map<nodes::NodeId, int> const & fanOut,
          std::map<std::string, std::string> & assignedVars,
          std::vector<std::string> & statements,
          int & varCounter,
          int parentPrecedence);

        /// Get the source expression for a parameter by following its link.
        std::string sourceExpression(
          nodes::Model & model,
          nodes::VariantParameter const & param,
          std::map<nodes::NodeId, int> const & fanOut,
          std::map<std::string, std::string> & assignedVars,
          std::vector<std::string> & statements,
          int & varCounter,
          int parentPrecedence)
        {
            auto const & src = param.getConstSource();
            if (!src.has_value())
            {
                // No link: use the parameter's literal value
                auto val = param.getValue();
                if (auto const * f = std::get_if<float>(&val))
                {
                    // Format float without trailing zeros
                    std::ostringstream oss;
                    oss << *f;
                    return oss.str();
                }
                return "0";
            }
            return nodeToExpression(
              model, src->nodeId, src->shortName, fanOut, assignedVars, statements,
              varCounter, parentPrecedence);
        }

        std::string nodeToExpression(
          nodes::Model & model,
          nodes::NodeId nodeId,
          std::string const & portName,
          std::map<nodes::NodeId, int> const & fanOut,
          std::map<std::string, std::string> & assignedVars,
          std::vector<std::string> & statements,
          int & varCounter,
          int parentPrecedence)
        {
            // Check if we already assigned a variable for this node+port
            // For DecomposeVector, the port name matters (x/y/z produce different values)
            auto nodeOpt = model.getNode(nodeId);
            if (!nodeOpt.has_value())
            {
                return "0";
            }
            nodes::NodeBase * node = nodeOpt.value();
            std::string const nodeType = node->name();

            // For DecomposeVector nodes, include port name in the key
            std::string cacheKey = std::to_string(nodeId);
            if (nodeType == "DecomposeVector")
            {
                cacheKey += "." + portName;
            }

            auto assignedIt = assignedVars.find(cacheKey);
            if (assignedIt != assignedVars.end())
            {
                return assignedIt->second;
            }

            // Begin node: return the port name (which is the argument name)
            if (dynamic_cast<nodes::Begin const *>(node) != nullptr)
            {
                return portName;
            }

            // ConstantScalar: return the literal value
            if (nodeType == "ConstantScalar")
            {
                auto const & params = node->parameter();
                auto valIt = params.find(nodes::FieldNames::Value);
                if (valIt != params.end())
                {
                    auto val = valIt->second.getValue();
                    if (auto const * f = std::get_if<float>(&val))
                    {
                        std::ostringstream oss;
                        oss << *f;
                        return oss.str();
                    }
                }
                return "0";
            }

            // DecomposeVector: recurse into its A input and append .component
            if (nodeType == "DecomposeVector")
            {
                auto const & params = node->parameter();
                auto aIt = params.find(nodes::FieldNames::A);
                if (aIt != params.end())
                {
                    std::string vecExpr = sourceExpression(
                      model, aIt->second, fanOut, assignedVars, statements, varCounter, 100);
                    std::string expr = vecExpr + "." + portName;

                    // If this decompose+port is used multiple times, assign a variable
                    auto fanIt = fanOut.find(nodeId);
                    // For decompose vectors, we count references to the node overall
                    // but different ports are different values - check if used more than once
                    // We'll just inline them since component access is cheap
                    assignedVars[cacheKey] = expr;
                    return expr;
                }
                return "0";
            }

            // Build expression for this node
            std::string expr;

            if (isBinaryOperatorNodeType(nodeType))
            {
                auto const & params = node->parameter();
                auto aIt = params.find(nodes::FieldNames::A);
                auto bIt = params.find(nodes::FieldNames::B);
                if (aIt == params.end() || bIt == params.end())
                {
                    return "0";
                }

                int myPrec = operatorPrecedence(nodeType);
                std::string left = sourceExpression(
                  model, aIt->second, fanOut, assignedVars, statements, varCounter, myPrec);
                std::string right = sourceExpression(
                  model, bIt->second, fanOut, assignedVars, statements, varCounter, myPrec + 1);

                char op = binaryOperatorChar(nodeType);
                expr = left + " " + op + " " + right;

                // Wrap in parens if our precedence is lower than parent's
                if (myPrec < parentPrecedence)
                {
                    expr = "(" + expr + ")";
                }
            }
            else if (isUnaryNodeType(nodeType))
            {
                auto const & params = node->parameter();
                auto aIt = params.find(nodes::FieldNames::A);
                if (aIt == params.end())
                {
                    return "0";
                }
                std::string funcName = nodeTypeToFunctionName(nodeType);
                std::string arg = sourceExpression(
                  model, aIt->second, fanOut, assignedVars, statements, varCounter, 0);
                expr = funcName + "(" + arg + ")";
            }
            else if (nodeType == "Pow")
            {
                auto const & params = node->parameter();
                auto baseIt = params.find(nodes::FieldNames::Base);
                auto expIt = params.find(nodes::FieldNames::Exponent);
                if (baseIt == params.end() || expIt == params.end())
                {
                    return "0";
                }
                std::string baseExpr = sourceExpression(
                  model, baseIt->second, fanOut, assignedVars, statements, varCounter, 0);
                std::string expExpr = sourceExpression(
                  model, expIt->second, fanOut, assignedVars, statements, varCounter, 0);
                expr = "pow(" + baseExpr + ", " + expExpr + ")";
            }
            else if (nodeType == "Select")
            {
                auto const & params = node->parameter();
                auto aIt = params.find(nodes::FieldNames::A);
                auto bIt = params.find(nodes::FieldNames::B);
                auto cIt = params.find(nodes::FieldNames::C);
                auto dIt = params.find(nodes::FieldNames::D);
                if (aIt == params.end() || bIt == params.end() ||
                    cIt == params.end() || dIt == params.end())
                {
                    return "0";
                }
                std::string a = sourceExpression(
                  model, aIt->second, fanOut, assignedVars, statements, varCounter, 0);
                std::string b = sourceExpression(
                  model, bIt->second, fanOut, assignedVars, statements, varCounter, 0);
                std::string c = sourceExpression(
                  model, cIt->second, fanOut, assignedVars, statements, varCounter, 0);
                std::string d = sourceExpression(
                  model, dIt->second, fanOut, assignedVars, statements, varCounter, 0);
                expr = "select(" + a + ", " + b + ", " + c + ", " + d + ")";
            }
            else if (nodeType == "Clamp")
            {
                auto const & params = node->parameter();
                auto aIt = params.find(nodes::FieldNames::A);
                auto minIt = params.find(nodes::FieldNames::Min);
                auto maxIt = params.find(nodes::FieldNames::Max);
                if (aIt == params.end() || minIt == params.end() || maxIt == params.end())
                {
                    return "0";
                }
                std::string a = sourceExpression(
                  model, aIt->second, fanOut, assignedVars, statements, varCounter, 0);
                std::string mn = sourceExpression(
                  model, minIt->second, fanOut, assignedVars, statements, varCounter, 0);
                std::string mx = sourceExpression(
                  model, maxIt->second, fanOut, assignedVars, statements, varCounter, 0);
                expr = "clamp(" + a + ", " + mn + ", " + mx + ")";
            }
            else if (nodeType == "Min" || nodeType == "Max" ||
                     nodeType == "ArcTan2" || nodeType == "Fmod" || nodeType == "Mod")
            {
                auto const & params = node->parameter();
                auto aIt = params.find(nodes::FieldNames::A);
                auto bIt = params.find(nodes::FieldNames::B);
                if (aIt == params.end() || bIt == params.end())
                {
                    return "0";
                }
                std::string funcName = nodeTypeToFunctionName(nodeType);
                std::string a = sourceExpression(
                  model, aIt->second, fanOut, assignedVars, statements, varCounter, 0);
                std::string b = sourceExpression(
                  model, bIt->second, fanOut, assignedVars, statements, varCounter, 0);
                expr = funcName + "(" + a + ", " + b + ")";
            }
            else
            {
                // Unknown node type: use a placeholder
                expr = "/* unknown: " + nodeType + " */";
            }

            // If this node has fan-out > 1, assign it to a variable for reuse
            auto fanIt = fanOut.find(nodeId);
            int fanOutCount = (fanIt != fanOut.end()) ? fanIt->second : 0;
            if (fanOutCount > 1)
            {
                std::string varName = "v" + std::to_string(varCounter++);
                statements.push_back("float " + varName + " = " + expr);
                assignedVars[cacheKey] = varName;
                return varName;
            }

            assignedVars[cacheKey] = expr;
            return expr;
        }
    } // anonymous namespace

    std::string ExpressionToGraphConverter::convertGraphToSnippet(
      nodes::Model & model,
      std::vector<FunctionArgument> const & arguments,
      FunctionOutput const & output)
    {
        nodes::End * endNode = model.getEndNode();
        if (!endNode)
        {
            return {};
        }

        auto fanOut = countFanOut(model);

        std::map<std::string, std::string> assignedVars;
        std::vector<std::string> statements;
        int varCounter = 0;

        // Find the End node's input parameter that matches the output name
        auto const & endParams = endNode->parameter();
        std::string outputParamName = output.name;
        if (outputParamName.empty())
        {
            // Take the first connected parameter on the End node
            for (auto const & [name, param] : endParams)
            {
                if (param.getConstSource().has_value())
                {
                    outputParamName = name;
                    break;
                }
            }
        }

        auto paramIt = endParams.find(outputParamName);
        if (paramIt == endParams.end())
        {
            return {};
        }

        std::string resultExpr = sourceExpression(
          model, paramIt->second, fanOut, assignedVars, statements, varCounter, 0);

        // Build the final snippet
        std::string snippet;
        for (auto const & stmt : statements)
        {
            snippet += stmt + ";\n";
        }
        snippet += "return " + resultExpr + ";";

        return snippet;
    }

} // namespace gladius
