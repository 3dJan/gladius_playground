#pragma once

#include "FunctionArgument.h"
#include "nodes/nodesfwd.h"
#include "nodes/types.h"

#include <string>
#include <vector>

namespace gladius
{
    class ExpressionParser;

    /// One function block of a multi-function program listing.
    struct ProgramFunctionBlock
    {
        std::string displayName;
        nodes::ResourceId resourceId{0};
        std::string body;
        std::vector<FunctionArgument> args;
        std::vector<FunctionOutput> outputs;
    };

    /// Outcome of applying one function block to an assembly.
    struct ProgramFunctionApplyResult
    {
        nodes::ResourceId resourceId{0};
        std::string displayName;
        std::string message;
    };

    /// Aggregated outcome of applying a whole program listing.
    struct ProgramSnippetApplyReport
    {
        std::vector<ProgramFunctionApplyResult> parsed;
        std::vector<ProgramFunctionApplyResult> failed;
        std::vector<ProgramFunctionApplyResult> skipped;

        /// True when at least one function was applied and none failed or was skipped.
        [[nodiscard]] bool isComplete() const
        {
            return !parsed.empty() && failed.empty() && skipped.empty();
        }
    };

    /**
     * @brief Applies multi-function program listings to a nodes::Assembly.
     *
     * Every function is parsed into a throwaway model before it replaces the model in
     * the assembly, so a failing function never leaves its previous graph blanked.
     * A listing is applied all-or-nothing: on any failure the previous graphs of all
     * touched functions are restored and the newly created functions are removed.
     */
    class ProgramSnippetApplier
    {
      public:
        /// Parse a multi-function program listing into function blocks.
        /// Throws std::runtime_error when the listing contains no function block.
        static std::vector<ProgramFunctionBlock>
        parseBlocks(std::string const & program);

        /// Parse all blocks and commit them only if every block parsed cleanly.
        /// On any failure the assembly is left exactly as it was and the failures
        /// are reported instead of thrown. Blocks whose body contains unsupported
        /// nodes are reported as skipped and keep their existing graph.
        /// Throws std::runtime_error only when the listing itself is invalid.
        static ProgramSnippetApplyReport apply(std::string const & program,
                                               nodes::Assembly & assembly,
                                               ExpressionParser & parser);

        /// Replace every function graph in the assembly. Throws when nothing was applied
        /// or when functions had to be skipped. Kept for callers that rely on
        /// throw-on-error semantics.
        static void applyOrThrow(std::string const & program,
                                 nodes::Assembly & assembly,
                                 ExpressionParser & parser);

      private:
        /// Parse one block into `candidate`. Returns the output node id, 0 on failure.
        static nodes::NodeId parseBlock(ProgramFunctionBlock const & block,
                                        nodes::Assembly & assembly,
                                        ExpressionParser & parser,
                                        nodes::Model & candidate,
                                        std::vector<FunctionOutput> const & effectiveOutputs,
                                        std::string & error);

        /// Verify that every function referenced by `candidate` exists in the assembly.
        /// Returns an empty string when all references resolve.
        static std::string findDanglingReference(nodes::Assembly & assembly,
                                                 nodes::Model & candidate);
    };
}
