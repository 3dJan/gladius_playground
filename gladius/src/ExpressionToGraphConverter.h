#pragma once

#include <array>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "FunctionArgument.h"
#include "nodes/nodesfwd.h"

namespace gladius
{
    class ExpressionParser;
}

namespace gladius::nodes
{
    class Assembly;
    class Model;
    class NodeBase;
    using NodeId = int;
    using ResourceId = uint32_t;
}

namespace gladius
{
    /**
     * @brief Converts mathematical expressions to Gladius node graphs
     *
     * This class takes a parsed mathematical expression and creates the corresponding
     * node graph in a Gladius model. It handles the creation of math operation nodes,
     * variable input nodes, and the connections between them.
     */
    class ExpressionToGraphConverter
    {
      public:
        /// Marker prefix embedded in generated snippets for node types that cannot be converted
        static constexpr char const * UNSUPPORTED_NODE_MARKER = "/* unsupported:";
        /**
         * @brief Convert a mathematical expression to a node graph
         * @param expression The mathematical expression string
         * @param model The model to add nodes to
         * @param parser The expression parser instance
         * @param arguments Optional function arguments (for vector support)
         * @param output Optional function output specification (name and type)
         * @return The NodeId of the output node, or 0 if conversion failed
         */
        static nodes::NodeId
        convertExpressionToGraph(std::string const & expression,
                                 nodes::Model & model,
                                 ExpressionParser & parser,
                                 std::vector<FunctionArgument> const & arguments = {},
                                 FunctionOutput const & output = {});

        /**
         * @brief Convert a multi-line snippet to a node graph
         * @param snippet The multi-line snippet string
         * @param model The model to add nodes to
         * @param parser The expression parser instance
         * @param arguments Optional function arguments (for vector support)
         * @param output Optional function output specification (name and type)
         * @return The NodeId of the output node, or 0 if conversion failed
         */
        static nodes::NodeId
        convertSnippetToGraph(std::string const & snippet,
                              nodes::Model & model,
                              ExpressionParser & parser,
                              std::vector<FunctionArgument> const & arguments = {},
                              FunctionOutput const & output = {},
                              nodes::Assembly * assembly = nullptr);

        /// @brief Convert a multi-line snippet to a node graph with multiple outputs.
        /// For multi-output functions, output assignments use the form `outputName = expr;`.
        static nodes::NodeId
        convertSnippetToGraph(std::string const & snippet,
                              nodes::Model & model,
                              ExpressionParser & parser,
                              std::vector<FunctionArgument> const & arguments,
                              std::vector<FunctionOutput> const & outputs,
                              nodes::Assembly * assembly);

        /**
         * @brief Convert a node graph back to a multi-line snippet
         * @param model The model containing the nodes
         * @param arguments Optional function arguments
         * @param output Optional function output specification
         * @return The generated snippet string
         */
        static std::string
        convertGraphToSnippet(nodes::Model & model,
                              std::vector<FunctionArgument> const & arguments = {},
                              FunctionOutput const & output = {},
                              nodes::Assembly * assembly = nullptr);

        /// @brief Convert a node graph back to a snippet, handling multiple outputs.
        /// When outputs has more than one entry, each output is emitted as
        /// `outputName = expr;` instead of a single `return` statement.
        static std::string
        convertGraphToSnippet(nodes::Model & model,
                              std::vector<FunctionArgument> const & arguments,
                              std::vector<FunctionOutput> const & outputs,
                              nodes::Assembly * assembly);

        /**
         * @brief Check if an expression can be converted to a graph
         * @param expression The expression to check
         * @param parser The expression parser to use for validation
         * @return true if the expression can be converted to a graph
         */
        static bool canConvertToGraph(std::string const & expression, ExpressionParser & parser);

        /// Generate a unique GLSL-compatible identifier from a display name and resource ID.
        /// Sanitizes the name: replaces non-alphanumeric chars with '_', collapses consecutive '_',
        /// prepends 'f_' if result starts with a digit, and appends '_resourceId'.
        static std::string generateUniqueFunctionName(std::string const & displayName,
                                                      nodes::ResourceId resourceId);

        /// Convert all functions in an assembly to a single code listing.
        /// Functions are topologically sorted by call dependencies.
        /// Throws std::runtime_error if circular dependencies are detected.
        static std::string convertProgramToSnippet(nodes::Assembly & assembly);

        /// Annotate a program snippet with [root] markers for root functions.
        /// Inserts " [root]" after "// Function: name (ID: N)" headers for
        /// functions whose resource IDs appear in rootFunctionIds.
        static std::string annotateRootFunctions(std::string const & snippet,
                                                 std::set<nodes::ResourceId> const & rootFunctionIds);

        /// Parse a multi-function code listing and create/update function graphs.
        /// Each function must be in the format produced by convertProgramToSnippet.
        /// Throws std::runtime_error on parse errors or dangling references.
        static void setProgramSnippet(std::string const & program,
                                      nodes::Assembly & assembly,
                                      ExpressionParser & parser);

      private:
        /**
         * @brief Create variable input nodes for the expression
         * @param variables List of variable names found in the expression
         * @param model The model to add nodes to
         * @param arguments Function arguments for type information
         * @return Map of variable names to their corresponding node IDs
         */
        static std::map<std::string, nodes::NodeId>
        createVariableNodes(std::vector<std::string> const & variables,
                            nodes::Model & model,
                            std::vector<FunctionArgument> const & arguments);

        /**
         * @brief Create argument input nodes based on function arguments
         * @param arguments Function arguments definition
         * @param model The model to add nodes to
         * @return Map of argument names to their corresponding node IDs
         */
        static std::map<std::string, nodes::NodeId>
        createArgumentNodes(std::vector<FunctionArgument> const & arguments, nodes::Model & model);

        /**
         * @brief Parse component access expression (e.g., "A.x", "pos.y")
         * @param expression The component access expression
         * @param argumentNodes Map of argument names to node IDs
         * @param model The model to add nodes to
         * @return The NodeId of the component output, or 0 if parsing failed
         */
        static nodes::NodeId
        parseComponentAccess(std::string const & expression,
                             std::map<std::string, nodes::NodeId> const & argumentNodes,
                             nodes::Model & model);

        /**
         * @brief Create a component extractor node (pass-through for vector components)
         * @param component The component name ("x", "y", or "z")
         * @param model The model to add the node to
         * @return The created node, or nullptr if creation failed
         */
        static nodes::NodeBase * createComponentExtractorNode(std::string const & component,
                                                              nodes::Model & model);

        /**
         * @brief Track DecomposeVector node component mappings (queued per node)
         */
        static std::map<nodes::NodeId, std::string> s_componentMap;

        /**
         * @brief Track vector nodes to reuse DecomposeVector nodes
         */
        static std::map<std::string, nodes::NodeId> s_vectorDecomposeNodes;

        /**
         * @brief Track Begin node argument output ports and their types
         * Maps argument name to ArgumentType for Begin node outputs
         */
        static std::map<std::string, ArgumentType> s_beginNodeArguments;

        /**
         * @brief Maps snippet identifiers (in_argName) to actual Begin node port names (argName).
         * Used to translate in_-prefixed variable references back to the real argument name
         * during snippet→graph conversion when resolving Begin node output ports.
         */
        static thread_local std::map<std::string, std::string> s_argSnippetToPortName;

        /**
         * @brief Track current variable context for Begin node port resolution
         */
        static thread_local std::vector<std::string> s_variableContextStack;

        /// Assembly context for resolving cross-function calls during snippet→graph conversion
        static thread_local nodes::Assembly * s_assemblyContext;

        /// Create a binary math-operation node, parse two arguments, and connect them.
        static nodes::NodeId createTwoInputNode(
          std::string const & nodeTypeName,
          std::vector<std::string> const & args,
          nodes::Model & model,
          std::map<std::string, nodes::NodeId> const & variableNodes,
          std::string const & inputA = nodes::FieldNames::A,
          std::string const & inputB = nodes::FieldNames::B);

        /**
         * @brief Create a mathematical operation node
         * @param operation The operation name (e.g., "Addition", "Multiplication")
         * @param model The model to add the node to
         * @return The NodeId of the created node, or 0 if creation failed
         */
        static nodes::NodeId createMathOperationNode(std::string const & operation,
                                                     nodes::Model & model);

        /**
         * @brief Create a constant value node
         * @param value The constant value
         * @param model The model to add the node to
         * @return The NodeId of the created node, or 0 if creation failed
         */
        static nodes::NodeId createConstantNode(double value, nodes::Model & model);

        /// Create a ConstantVector node with the given x/y/z values.
        static nodes::NodeId
        createConstantVectorNode(double x, double y, double z, nodes::Model & model);

        /// Create a ConstantMatrix node with the given 16 values (row-major m00..m33).
        static nodes::NodeId createConstantMatrixNode(
          std::array<double, 16> const & values, nodes::Model & model);

        /**
         * @brief Connect two nodes via their ports
         * @param model The model containing the nodes
         * @param fromNodeId Source node ID
         * @param fromPortName Source port name
         * @param toNodeId Destination node ID
         * @param toPortName Destination port name
         * @return true if connection was successful
         */
        static bool connectNodes(nodes::Model & model,
                                 nodes::NodeId fromNodeId,
                                 std::string const & fromPortName,
                                 nodes::NodeId toNodeId,
                                 std::string const & toPortName);

        /**
         * @brief Parse expression manually and build graph recursively
         *
         * Since muParser doesn't provide AST access, we'll implement a simple
         * recursive descent parser for basic mathematical expressions.
         *
         * @param expression The expression to parse
         * @param model The model to add nodes to
         * @param variableNodes Map of variable names to node IDs
         * @return The NodeId of the result node, or 0 if parsing failed
         */
        static nodes::NodeId
        parseAndBuildGraph(std::string const & expression,
                           nodes::Model & model,
                           std::map<std::string, nodes::NodeId> const & variableNodes);

        /**
         * @brief Parse a function call and create the appropriate math node
         *
         * @param expression The function call expression (e.g., "sin(x)", "pow(x,2)")
         * @param model The model to add nodes to
         * @param variableNodes Map of variable names to node IDs
         * @return The NodeId of the result node, or 0 if parsing failed
         */
        static nodes::NodeId
        parseFunctionCall(std::string const & expression,
                          nodes::Model & model,
                          std::map<std::string, nodes::NodeId> const & variableNodes);

        /**
         * @brief Check if expression is a function call with component access (e.g., "sin(pos).x")
         * @param expression The expression to check
         * @return true if it's a function call followed by component access
         */
        static bool isFunctionCallWithComponentAccess(std::string const & expression);

        /**
         * @brief Check if expression contains function calls with component access anywhere within
         * it
         * @param expression The expression to check
         * @return true if it contains function calls with component access
         */
        static bool containsFunctionCallWithComponentAccess(std::string const & expression);

        /**
         * @brief Parse a function call with component access (e.g., "sin(pos).x")
         * @param expression The expression to parse
         * @param model The model to add nodes to
         * @param variableNodes Map of variable names to node IDs
         * @return The NodeId of the component result, or 0 if parsing failed
         */
        static nodes::NodeId parseFunctionCallWithComponentAccess(
          std::string const & expression,
          nodes::Model & model,
          std::map<std::string, nodes::NodeId> const & variableNodes);

        /**
         * @brief Parse a nested function call with component access (e.g.,
         * "cos(sin(pos).x+sin(pos).x)")
         * @param expression The expression to parse
         * @param model The model to add nodes to
         * @param variableNodes Map of variable names to node IDs
         * @return The NodeId of the result node, or 0 if parsing failed
         */
        static nodes::NodeId parseNestedFunctionCallWithComponentAccess(
          std::string const & expression,
          nodes::Model & model,
          std::map<std::string, nodes::NodeId> const & variableNodes);

        /**
         * @brief Preprocess component access in expressions by replacing them with temporary
         * variables
         * @param expression The expression containing component access patterns
         * @return The preprocessed expression that muParser can understand
         */
        static std::string preprocessComponentAccess(std::string const & expression);

        /**
         * @brief Parse complex expressions that may contain nested function calls with component
         * access
         * @param expression The expression to parse
         * @param model The model to add nodes to
         * @param variableNodes Map of variable names to node IDs
         * @return The NodeId of the result node, or 0 if parsing failed
         */
        static nodes::NodeId
        parseComplexExpression(std::string const & expression,
                               nodes::Model & model,
                               std::map<std::string, nodes::NodeId> const & variableNodes);

        /**
         * @brief Parse a comma-separated argument list
         * @param argumentsStr The arguments string (without parentheses)
         * @return Vector of argument expressions
         */
        static std::vector<std::string> parseArgumentList(std::string const & argumentsStr);

        /**
         * @brief Check if a function takes only one argument
         */
        static bool isSingleArgumentFunction(std::string const & functionName);

        /**
         * @brief Check if a function takes two arguments
         */
        static bool isBinaryFunction(std::string const & functionName);

        /**
         * @brief Check if a function takes three arguments
         */
        static bool isTernaryFunction(std::string const & functionName);

        /**
         * @brief Remove whitespace from expression
         */
        static std::string removeWhitespace(std::string const & expr);

        /**
         * @brief Find the main operator in an expression (respecting precedence)
         */
        static std::pair<size_t, char> findMainOperator(std::string const & expr);

        /**
         * @brief Check if a character is a valid operator
         */
        static bool isOperator(char c);

        /**
         * @brief Get operator precedence
         */
        static int getOperatorPrecedence(char op);

        /**
         * @brief Check if an expression is a component access (e.g., "A.x")
         * @param expression The expression to check
         * @return true if it's a component access expression
         */
        static bool isComponentAccess(std::string const & expression);

        /**
         * @brief Extract argument name and component from component access expression
         * @param expression The component access expression (e.g., "A.x")
         * @return Pair of argument name and component name, or empty strings if invalid
         */
        static std::pair<std::string, std::string>
        parseComponentExpression(std::string const & expression);

        /**
         * @brief Get the correct output port name for a node
         */
        static std::string getOutputPortName(nodes::Model & model, nodes::NodeId nodeId);

        /**
         * @brief Check if an expression is a preprocessed component access (e.g., "pos_x")
         * @param expression The expression to check
         * @return true if it's a preprocessed component access expression
         */
        static bool isPreprocessedComponentAccess(std::string const & expression);

        /**
         * @brief Convert preprocessed component access back to original form
         * @param expression The preprocessed expression (e.g., "pos_x")
         * @return Original form (e.g., "pos.x"), or unchanged if not valid
         */
        static std::string convertPreprocessedToOriginal(std::string const & expression);

        /**
         * @brief Connect the expression result to the End node with specified output
         * @param model The model containing the nodes
         * @param resultNodeId The node ID of the expression result
         * @param output The function output specification
         * @return true if connection was successful
         */
        static bool connectToEndNode(nodes::Model & model,
                                     nodes::NodeId resultNodeId,
                                     FunctionOutput const & output);

        /**
         * @brief Validate that the expression result type matches the expected output type
         * @param model The model containing the nodes
         * @param resultNodeId The node ID of the expression result
         * @param expectedType The expected output type
         * @return true if types are compatible
         */
        static bool validateOutputType(nodes::Model & model,
                                       nodes::NodeId resultNodeId,
                                       ArgumentType expectedType);
    };

} // namespace gladius
