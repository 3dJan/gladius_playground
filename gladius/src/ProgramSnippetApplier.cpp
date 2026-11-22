#include "ProgramSnippetApplier.h"

#include "ExpressionParser.h"
#include "ExpressionToGraphConverter.h"
#include "FunctionCallPortSync.h"
#include "nodes/Assembly.h"
#include "nodes/DerivedNodes.h"
#include "nodes/Model.h"
#include "nodes/NodeBase.h"

#include <algorithm>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>

namespace gladius
{
    namespace
    {
        /// Header of a function block: "// Function: displayName (ID: 123)"
        std::regex const & headerRegex()
        {
            static std::regex const regex(R"(//\s*Function:\s*(.+?)\s*\(ID:\s*(\d+)\))");
            return regex;
        }

        /// A typed function argument inside a signature: "vec3 pos"
        std::regex const & sigArgRegex()
        {
            static std::regex const regex(R"((vec3|float|mat4)\s+(\w+))");
            return regex;
        }

        ArgumentType toArgumentType(std::string const & typeStr)
        {
            if (typeStr == "vec3")
            {
                return ArgumentType::Vector;
            }
            if (typeStr == "mat4")
            {
                return ArgumentType::Matrix;
            }
            return ArgumentType::Scalar;
        }

        std::string trimmed(std::string const & text)
        {
            auto const first = text.find_first_not_of(" \t");
            if (first == std::string::npos)
            {
                return "";
            }
            auto const last = text.find_last_not_of(" \t");
            return text.substr(first, last - first + 1);
        }

        /// Read the return-type list of a multi-output signature: "(float shape, vec3 color)".
        std::vector<FunctionOutput> parseOutputList(std::string const & returnTypes)
        {
            std::vector<FunctionOutput> outputs;
            auto begin = std::sregex_iterator(returnTypes.begin(), returnTypes.end(), sigArgRegex());
            auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it)
            {
                outputs.emplace_back((*it)[2].str(), toArgumentType((*it)[1].str()));
            }
            return outputs;
        }

        /// Read the parameter list of a signature: "vec3 pos, float radius".
        std::vector<FunctionArgument> parseArgumentList(std::string const & argStr)
        {
            std::vector<FunctionArgument> args;
            auto begin = std::sregex_iterator(argStr.begin(), argStr.end(), sigArgRegex());
            auto end = std::sregex_iterator();
            for (auto it = begin; it != end; ++it)
            {
                args.emplace_back((*it)[2].str(), toArgumentType((*it)[1].str()));
            }
            return args;
        }

        /// Parse the signature line that contains the opening brace.
        void parseSignature(std::string const & line, ProgramFunctionBlock & block)
        {
            auto const signature = trimmed(line);
            auto const parenOpen = line.find('(');
            auto parenClose = line.find(')');

            if (signature.front() == '(')
            {
                // Multi-output form: "(float shape, vec3 color) name(vec3 pos) {"
                auto const closeReturnParen = signature.find(')');
                if (closeReturnParen != std::string::npos)
                {
                    block.outputs =
                      parseOutputList(signature.substr(1, closeReturnParen - 1));
                }
                if (parenClose != std::string::npos)
                {
                    auto const argOpen = line.find('(', parenClose + 1);
                    if (argOpen != std::string::npos)
                    {
                        parenClose = line.find(')', argOpen + 1);
                        block.args = parseArgumentList(
                          line.substr(argOpen + 1, parenClose - argOpen - 1));
                    }
                }
            }
            else if (signature.substr(0, 4) == "vec3")
            {
                block.outputs.emplace_back(
                  FunctionOutput::defaultOutput().name, ArgumentType::Vector);
            }
            else if (signature.substr(0, 4) == "mat4")
            {
                block.outputs.emplace_back(
                  FunctionOutput::defaultOutput().name, ArgumentType::Matrix);
            }
            else
            {
                block.outputs.push_back(FunctionOutput::defaultOutput());
            }

            if (block.args.empty() && parenOpen != std::string::npos &&
                parenClose != std::string::npos && parenClose > parenOpen)
            {
                block.args =
                  parseArgumentList(line.substr(parenOpen + 1, parenClose - parenOpen - 1));
            }

            if (block.args.empty())
            {
                block.args.emplace_back("pos", ArgumentType::Vector);
            }
        }
    }

    std::vector<ProgramFunctionBlock> ProgramSnippetApplier::parseBlocks(std::string const & program)
    {
        std::vector<ProgramFunctionBlock> blocks;

        ProgramFunctionBlock current;
        bool inFunction = false;
        int braceDepth = 0;
        std::string bodyAccum;

        std::istringstream stream(program);
        std::string line;
        while (std::getline(stream, line))
        {
            std::smatch match;
            if (std::regex_search(line, match, headerRegex()))
            {
                current.displayName = match[1].str();
                current.resourceId = static_cast<nodes::ResourceId>(std::stoul(match[2].str()));
                current.args.clear();
                current.outputs.clear();
                inFunction = false;
                braceDepth = 0;
                bodyAccum.clear();
                continue;
            }

            if (current.resourceId != 0 && !inFunction)
            {
                auto const bracePos = line.find('{');
                if (bracePos != std::string::npos)
                {
                    parseSignature(line, current);
                    inFunction = true;
                    braceDepth = 1;

                    // Support single-line bodies: "float foo(vec3 p) { return 1.0; }"
                    auto afterBrace = line.substr(bracePos + 1);
                    for (char c : afterBrace)
                    {
                        if (c == '{')
                        {
                            ++braceDepth;
                        }
                        if (c == '}')
                        {
                            --braceDepth;
                        }
                    }
                    if (braceDepth <= 0)
                    {
                        auto const bodyEnd = afterBrace.rfind('}');
                        if (bodyEnd != std::string::npos)
                        {
                            afterBrace = afterBrace.substr(0, bodyEnd);
                        }
                        current.body = trimmed(afterBrace);
                        blocks.push_back(current);
                        current = {};
                        inFunction = false;
                    }
                    else
                    {
                        auto const first = afterBrace.find_first_not_of(" \t");
                        if (first != std::string::npos)
                        {
                            bodyAccum = afterBrace.substr(first);
                        }
                    }
                    continue;
                }
            }

            if (!inFunction)
            {
                continue;
            }

            for (char c : line)
            {
                if (c == '{')
                {
                    ++braceDepth;
                }
                if (c == '}')
                {
                    --braceDepth;
                }
            }

            if (braceDepth <= 0)
            {
                current.body = bodyAccum;
                blocks.push_back(current);
                current = {};
                inFunction = false;
                continue;
            }

            // Strip the leading 2-space indent used by generated listings.
            if (line.size() >= 2 && line[0] == ' ' && line[1] == ' ')
            {
                line = line.substr(2);
            }
            if (!bodyAccum.empty())
            {
                bodyAccum += "\n";
            }
            bodyAccum += line;
        }

        if (blocks.empty())
        {
            throw std::runtime_error("No function blocks found in program snippet");
        }
        return blocks;
    }

    nodes::NodeId ProgramSnippetApplier::parseBlock(ProgramFunctionBlock const & block,
                                                    nodes::Assembly & assembly,
                                                    ExpressionParser & parser,
                                                    nodes::Model & candidate,
                                                    std::vector<FunctionOutput> const & outputs,
                                                    std::string & error)
    {
        candidate.createBeginEnd();
        try
        {
            auto const nodeId = ExpressionToGraphConverter::convertSnippetToGraph(
              block.body, candidate, parser, block.args, outputs, &assembly);
            if (nodeId == 0)
            {
                error = "convertSnippetToGraph returned 0";
            }
            return nodeId;
        }
        catch (std::exception const & e)
        {
            error = e.what();
            return 0;
        }
    }

    std::string ProgramSnippetApplier::findDanglingReference(nodes::Assembly & assembly,
                                                             nodes::Model & candidate)
    {
        for (auto & [nodeId, nodePtr] : candidate)
        {
            nodes::ResourceId calledId = 0;
            if (auto * fc = dynamic_cast<nodes::FunctionCall *>(nodePtr.get()))
            {
                fc->resolveFunctionId();
                calledId = fc->getFunctionId();
            }
            else if (auto * fg = dynamic_cast<nodes::FunctionGradient *>(nodePtr.get()))
            {
                fg->resolveFunctionId();
                calledId = fg->getFunctionId();
            }
            else if (auto * ndf = dynamic_cast<nodes::NormalizeDistanceField *>(nodePtr.get()))
            {
                ndf->resolveFunctionId();
                calledId = ndf->getFunctionId();
            }

            if (calledId != 0 && !assembly.findModel(calledId))
            {
                return "references undefined function ID " + std::to_string(calledId);
            }
        }
        return "";
    }

    ProgramSnippetApplyReport ProgramSnippetApplier::apply(std::string const & program,
                                                           nodes::Assembly & assembly,
                                                           ExpressionParser & parser)
    {
        ProgramSnippetApplyReport report;
        auto blocks = parseBlocks(program);

        // Functions that contain unsupported nodes cannot be round-tripped. They are
        // reported instead of silently dropped, and their current graph stays untouched.
        for (auto const & block : blocks)
        {
            if (block.body.find(ExpressionToGraphConverter::UNSUPPORTED_NODE_MARKER) !=
                std::string::npos)
            {
                report.skipped.push_back(
                  {block.resourceId,
                   block.displayName,
                   "Body contains unsupported nodes and cannot be converted."});
            }
        }
        blocks.erase(std::remove_if(blocks.begin(),
                                    blocks.end(),
                                    [](ProgramFunctionBlock const & b)
                                    {
                                        return b.body.find(
                                         ExpressionToGraphConverter::UNSUPPORTED_NODE_MARKER) !=
                                          std::string::npos;
                                    }),
                     blocks.end());

        // First pass: make sure every function exists so cross-function calls can resolve,
        // and remember the previous graph of every model we are about to touch. Any failure
        // later on restores those graphs, so the assembly is never left half updated.
        std::vector<nodes::ResourceId> createdIds;
        std::map<nodes::ResourceId, nodes::Model> previousModels;
        std::map<nodes::ResourceId, events::SharedLogger> previousLoggers;
        for (auto const & block : blocks)
        {
            auto model = assembly.findModel(block.resourceId);
            if (assembly.addModelIfNotExisting(block.resourceId))
            {
                createdIds.push_back(block.resourceId);
                model = assembly.findModel(block.resourceId);
                model->createBeginEnd();
            }
            else
            {
                // Model's copy ctor does not carry the logger, so keep it separately.
                previousModels.emplace(block.resourceId, *model);
                previousLoggers[block.resourceId] = model->getLogger();
            }
        }

        auto rollback = [&assembly, &createdIds, &previousModels, &previousLoggers]
        {
            for (auto const & [resourceId, previous] : previousModels)
            {
                auto model = assembly.findModel(resourceId);
                if (model != nullptr)
                {
                    *model = previous;
                    model->setLogger(previousLoggers[resourceId]);
                }
            }
            for (auto const resourceId : createdIds)
            {
                assembly.deleteModel(resourceId);
            }
        };

        // Second pass: parse every block into a throwaway model before committing it.
        for (auto const & block : blocks)
        {
            auto model = assembly.findModel(block.resourceId);

            // When the listing only carries default output names ("result"), keep the
            // names already used by the model (e.g. "shape") so level set channels
            // and other references keep working. Count and types must match to avoid
            // silently turning a vec3 output into a scalar one (or vice versa).
            auto effectiveOutputs = block.outputs;
            bool const hasOnlyDefaultNames = std::all_of(
              effectiveOutputs.begin(),
              effectiveOutputs.end(),
              [](auto const & out) { return out.name == FunctionOutput::defaultOutput().name; });
            if (hasOnlyDefaultNames)
            {
                std::vector<FunctionOutput> existingOutputs;
                if (auto * endNode = model->getEndNode())
                {
                    for (auto const & [name, param] : endNode->constParameter())
                    {
                        if (param.getConstSource().has_value())
                        {
                            auto typeIdx = param.getConstSource()->type;
                            existingOutputs.emplace_back(
                              name,
                              typeIdx == nodes::ParameterTypeIndex::Float3
                                ? ArgumentType::Vector
                              : typeIdx == nodes::ParameterTypeIndex::Matrix4
                                ? ArgumentType::Matrix
                                : ArgumentType::Scalar);
                        }
                    }
                }
                bool const typesMatch =
                  existingOutputs.size() == effectiveOutputs.size() &&
                  std::equal(
                    existingOutputs.begin(),
                    existingOutputs.end(),
                    effectiveOutputs.begin(),
                    [](auto const & a, auto const & b) { return a.type == b.type; });
                if (!existingOutputs.empty() && typesMatch)
                {
                    effectiveOutputs = existingOutputs;
                }
            }

            nodes::Model candidate;
            candidate.setResourceId(block.resourceId);
            candidate.setDisplayName(block.displayName);
            candidate.setManaged(model->isManaged());
            candidate.setLogger(model->getLogger());

            std::string error;
            auto const nodeId = parseBlock(block, assembly, parser, candidate, effectiveOutputs, error);
            if (nodeId == 0)
            {
                report.failed.push_back({block.resourceId, block.displayName, error});
                continue;
            }

            candidate.updateGraphAndOrderIfNeeded();

            auto const dangling = findDanglingReference(assembly, candidate);
            if (!dangling.empty())
            {
                report.failed.push_back({block.resourceId, block.displayName, dangling});
                continue;
            }

            // Commit right away: later blocks may call this function and need its
            // resolved output types. A failure rolls back every previous commit.
            *model = std::move(candidate);
            report.parsed.push_back({block.resourceId, block.displayName, {}});
        }

        if (!report.failed.empty())
        {
            rollback();
            report.parsed.clear();
        }
        else
        {
            // The replaced functions may expose a different set of outputs than before, so the
            // call nodes of all other models have to be adapted (and stale outputs removed).
            syncFunctionCallPorts(assembly);
        }

        return report;
    }

    void ProgramSnippetApplier::applyOrThrow(std::string const & program,
                                             nodes::Assembly & assembly,
                                             ExpressionParser & parser)
    {
        auto const report = apply(program, assembly, parser);

        if (report.parsed.empty())
        {
            std::string reason = "no function could be applied";
            if (!report.failed.empty())
            {
                reason = report.failed.front().message;
            }
            else if (!report.skipped.empty())
            {
                reason = "all functions contain unsupported nodes";
            }
            throw std::runtime_error("Program snippet failed: " + reason);
        }

        // Functions with unsupported nodes keep their graph, so the rest of the
        // listing did apply. Callers that need a faithful round-trip must know.
        if (!report.skipped.empty())
        {
            std::ostringstream message;
            message << "Program snippet applied partially: " << report.parsed.size()
                    << " function(s) applied, " << report.skipped.size()
                    << " skipped (unsupported nodes):";
            for (auto const & skipped : report.skipped)
            {
                message << " '" << skipped.displayName << "' (ID: " << skipped.resourceId << ")";
            }
            throw std::runtime_error(message.str());
        }
    }
}
