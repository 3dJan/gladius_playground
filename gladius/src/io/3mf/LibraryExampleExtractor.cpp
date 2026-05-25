#include "LibraryExampleExtractor.h"

#include "LibraryMetadata.h"

#include <lib3mf_implicit.hpp>

#include <algorithm>
#include <cctype>

namespace gladius::io
{
    namespace
    {
        /// @brief Replaces non-alphanumeric characters with underscores, matching the
        ///        transformation the importer applies to all node/parameter identifiers.
        std::string makeValidVariableName(std::string const & name)
        {
            std::string result;
            result.reserve(name.size());
            for (auto const c : name)
            {
                result += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
            }
            return result;
        }
    } // namespace
    std::vector<ExampleConstantValue>
    extractExampleConstants(std::filesystem::path const & filePath,
                            std::string const & taggedFunctionDisplayName)
    {
        try
        {
            auto wrapper = Lib3MF::CWrapper::loadLibrary();
            auto model = wrapper->CreateModel();
            auto reader = model->QueryReader("3mf");
            reader->ReadFromFile(filePath.string());

            auto const libMeta = readLibraryMetadata(model);
            if (!libMeta)
            {
                return {};
            }

            auto const taggedIds = parseResourceIds(libMeta->libraryFunctions);
            if (taggedIds.empty())
            {
                return {};
            }

            // Find the tagged function's model resource ID by matching its display name.
            Lib3MF_uint32 taggedFunctionResourceId = 0;
            {
                auto resIter = model->GetResources();
                while (resIter->MoveNext())
                {
                    auto res = resIter->GetCurrent();
                    auto const modelId = res->GetModelResourceID();
                    if (std::find(taggedIds.begin(), taggedIds.end(), modelId) == taggedIds.end())
                    {
                        continue;
                    }
                    auto implicitFunc = std::dynamic_pointer_cast<Lib3MF::CImplicitFunction>(res);
                    if (implicitFunc &&
                        implicitFunc->GetDisplayName() == taggedFunctionDisplayName)
                    {
                        taggedFunctionResourceId = modelId;
                        break;
                    }
                }
            }

            if (taggedFunctionResourceId == 0)
            {
                return {};
            }

            // Scan all implicit functions to find one that calls the tagged function.
            // Typically this is the assembly/example function used for thumbnail generation.
            auto resIter = model->GetResources();
            while (resIter->MoveNext())
            {
                auto res = resIter->GetCurrent();
                auto implicitFunc = std::dynamic_pointer_cast<Lib3MF::CImplicitFunction>(res);
                if (!implicitFunc)
                {
                    continue;
                }
                // Skip tagged functions (the library function itself).
                auto const modelId = res->GetModelResourceID();
                if (std::find(taggedIds.begin(), taggedIds.end(), modelId) != taggedIds.end())
                {
                    continue;
                }

                // Search for a FunctionCall node that references the tagged function.
                auto nodeIter = implicitFunc->GetNodes();
                while (nodeIter->MoveNext())
                {
                    auto node = nodeIter->GetCurrent();
                    auto funcCallNode =
                      std::dynamic_pointer_cast<Lib3MF::CFunctionCallNode>(node);
                    if (!funcCallNode)
                    {
                        continue;
                    }

                    // Resolve the called function's resource ID via the functionid reference.
                    auto const funcIdPort = funcCallNode->GetInputFunctionID();
                    if (!funcIdPort || funcIdPort->GetReference().empty())
                    {
                        continue;
                    }

                    std::string const & refName = funcIdPort->GetReference();
                    auto const dotPos = refName.find('.');
                    std::string const sourceNodeId =
                      dotPos != std::string::npos ? refName.substr(0, dotPos) : refName;

                    Lib3MF_uint32 calledFunctionId = 0;
                    {
                        auto scanIter = implicitFunc->GetNodes();
                        while (scanIter->MoveNext())
                        {
                            auto scanNode = scanIter->GetCurrent();
                            if (scanNode->GetIdentifier() != sourceNodeId)
                            {
                                continue;
                            }
                            auto resourceIdNode =
                              std::dynamic_pointer_cast<Lib3MF::CResourceIdNode>(scanNode);
                            if (resourceIdNode && resourceIdNode->GetResource())
                            {
                                calledFunctionId =
                                  resourceIdNode->GetResource()->GetModelResourceID();
                            }
                            break;
                        }
                    }

                    if (calledFunctionId != taggedFunctionResourceId)
                    {
                        continue;
                    }

                    // Found the FunctionCall — collect constants wired to its argument inputs.
                    std::vector<ExampleConstantValue> result;
                    auto inputIter = funcCallNode->GetInputs();
                    while (inputIter->MoveNext())
                    {
                        auto input = inputIter->GetCurrent();

                        // Skip the special functionid input (case-insensitive: lib3mf may
                        // produce "functionID" or "functionid" depending on source).
                        auto const inputId = input->GetIdentifier();
                        if (inputId.size() == 10 &&
                            std::equal(inputId.begin(), inputId.end(), "functionid",
                                       [](unsigned char a, unsigned char b)
                                       {
                                           return std::tolower(a) == std::tolower(b);
                                       }))
                        {
                            continue;
                        }

                        std::string const & inputRef = input->GetReference();
                        if (inputRef.empty())
                        {
                            continue;
                        }

                        auto const inputDotPos = inputRef.find('.');
                        std::string const srcNodeId = inputDotPos != std::string::npos
                                                        ? inputRef.substr(0, inputDotPos)
                                                        : inputRef;

                        // Find the source node and extract its constant value.
                        auto srcIter = implicitFunc->GetNodes();
                        while (srcIter->MoveNext())
                        {
                            auto srcNode = srcIter->GetCurrent();
                            if (srcNode->GetIdentifier() != srcNodeId)
                            {
                                continue;
                            }

                            if (srcNode->GetNodeType() == Lib3MF::eImplicitNodeType::Constant)
                            {
                                auto scalarNode =
                                  std::dynamic_pointer_cast<Lib3MF::CConstantNode>(srcNode);
                                if (scalarNode)
                                {
                                    ExampleConstantValue val;
                                    val.kind = ExampleConstantValue::Kind::Scalar;
                                    val.parameterName = makeValidVariableName(input->GetIdentifier());
                                    val.scalarValue =
                                      static_cast<float>(scalarNode->GetConstant());
                                    result.push_back(std::move(val));
                                }
                            }
                            else if (srcNode->GetNodeType() == Lib3MF::eImplicitNodeType::ConstVec)
                            {
                                auto vecNode =
                                  std::dynamic_pointer_cast<Lib3MF::CConstVecNode>(srcNode);
                                if (vecNode)
                                {
                                    auto const v = vecNode->GetVector();
                                    ExampleConstantValue val;
                                    val.kind = ExampleConstantValue::Kind::Vector;
                                    val.parameterName = makeValidVariableName(input->GetIdentifier());
                                    val.vectorValue = {
                                      static_cast<float>(v.m_Coordinates[0]),
                                      static_cast<float>(v.m_Coordinates[1]),
                                      static_cast<float>(v.m_Coordinates[2])};
                                    result.push_back(std::move(val));
                                }
                            }
                            else if (srcNode->GetNodeType() == Lib3MF::eImplicitNodeType::ConstMat)
                            {
                                auto matNode =
                                  std::dynamic_pointer_cast<Lib3MF::CConstMatNode>(srcNode);
                                if (matNode)
                                {
                                    auto const m = matNode->GetMatrix();
                                    ExampleConstantValue val;
                                    val.kind = ExampleConstantValue::Kind::Matrix;
                                    val.parameterName = makeValidVariableName(input->GetIdentifier());
                                    for (int row = 0; row < 4; ++row)
                                    {
                                        for (int col = 0; col < 4; ++col)
                                        {
                                            val.matrixValue[row][col] =
                                              static_cast<float>(m.m_Field[row][col]);
                                        }
                                    }
                                    result.push_back(std::move(val));
                                }
                            }
                            break;
                        }
                    }

                    return result;
                }
            }
        }
        catch (...)
        {
            // Example constant extraction is best-effort; return empty on any failure.
        }

        return {};
    }

} // namespace gladius::io
