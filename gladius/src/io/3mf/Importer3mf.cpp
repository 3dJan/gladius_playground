#include "Importer3mf.h"
#include "BeamLatticeImporter.h"

#include "Lib3mfLoader.h"
#include <algorithm>
#include <fmt/format.h>
#include <lib3mf_abi.hpp>
#include <lib3mf_implicit.hpp>
#include <lib3mf_types.hpp>
#include <map>
#include <set>
#include <vector>

#include "BeamLatticeResource.h"
#include "Builder.h"
#include "Document.h"
#include "FunctionComparator.h"
#include "ImageExtractor.h"
#include "MeshBVH.h"
#include "Parameter.h"
#include "Profiling.h"
#include "SpatialMeshResource.h"
#include "VdbImporter.h"
#include "nodes/DerivedNodes.h"
#include "nodes/utils.h"
#include <Eigen/Core>
#include <variant>

namespace gladius
{
    namespace nodes
    {
        class Builder;
    }
}

namespace gladius::io
{

    // Convert 3MF model unit to scaling from millimeters to model units
    // Returns units_per_mm so that: position_in_model_units = position_in_mm * units_per_mm
    static float computeUnitsPerMM(Lib3MF::PModel const & model)
    {
        if (!model)
        {
            return 1.0f;
        }
        Lib3MF::eModelUnit unit = model->GetUnit();
        // mm per unit
        double mm_per_unit = 1.0;
        switch (unit)
        {
        case Lib3MF::eModelUnit::MicroMeter:
            mm_per_unit = 0.001; // 1 µm = 0.001 mm
            break;
        case Lib3MF::eModelUnit::MilliMeter:
            mm_per_unit = 1.0;
            break;
        case Lib3MF::eModelUnit::CentiMeter:
            mm_per_unit = 10.0;
            break;
        case Lib3MF::eModelUnit::Meter:
            mm_per_unit = 1000.0;
            break;
        case Lib3MF::eModelUnit::Inch:
            mm_per_unit = 25.4;
            break;
        case Lib3MF::eModelUnit::Foot:
            mm_per_unit = 304.8;
            break;
        default:
            mm_per_unit = 1.0; // Default/fallback to millimeters
            break;
        }
        // units per mm
        double units_per_mm = 1.0 / mm_per_unit;
        return static_cast<float>(units_per_mm);
    }

    Importer3mf::Importer3mf(events::SharedLogger logger)
        : m_eventLogger(logger)
    {
        ProfileFunction
        try
        {
            m_wrapper = gladius::io::loadLib3mfScoped();
        }
        catch (std::exception & e)
        {
            if (m_eventLogger)
            {
                m_eventLogger->addEvent(
                  {fmt::format("Error initializing Importer3mf: {}", e.what()),
                   events::Severity::Error});
            }
            return;
        }
    }

    void Importer3mf::logWarnings(std::filesystem::path const & filename, Lib3MF::PReader reader)
    {
        if (!m_eventLogger)
        {
            throw std::runtime_error("No event logger set");
        }
        for (Lib3MF_uint32 iWarning = 0; iWarning < reader->GetWarningCount(); iWarning++)
        {
            Lib3MF_uint32 nErrorCode;
            std::string sWarningMessage = reader->GetWarning(iWarning, nErrorCode);

            m_eventLogger->addEvent({fmt::format("Warning #{} while reading 3mf file {}: {}",
                                                 filename.string(),
                                                 nErrorCode,
                                                 sWarningMessage),
                                     events::Severity::Warning});
        }
    }

    gladius::nodes::VariantParameter parameterFromType(Lib3MF::eImplicitPortType type)
    {
        using namespace gladius::nodes;
        switch (type)
        {
        case Lib3MF::eImplicitPortType::Scalar:
            return gladius::nodes::VariantParameter(0.f);
        case Lib3MF::eImplicitPortType::Vector:
            return gladius::nodes::VariantParameter(float3{0.f, 0.f, 0.f});
        case Lib3MF::eImplicitPortType::Matrix:
        {
            return gladius::nodes::VariantParameter(Matrix4x4(), ContentType::Transformation);
        }
        default:
            return gladius::nodes::VariantParameter(0);
        }
    }

    std::type_index typeIndexFrom3mfType(Lib3MF::eImplicitPortType type)
    {
        using namespace gladius::nodes;
        switch (type)
        {
        case Lib3MF::eImplicitPortType::Scalar:
            return ParameterTypeIndex::Float;
        case Lib3MF::eImplicitPortType::Vector:
            return ParameterTypeIndex::Float3;
        case Lib3MF::eImplicitPortType::Matrix:
        {
            return ParameterTypeIndex::Matrix4;
        }
        default:
            return ParameterTypeIndex::Float;
        }
    }

    gladius::nodes::NodeBase * createNode(gladius::nodes::Model & model,
                                          Lib3MF::eImplicitNodeType type)
    {
        // TODO(NormalizeDistanceField-3MF): Add import support for NormalizeDistanceField
        // nodes once the 3MF implicit function extension defines a corresponding node type.
        // TODO(FunctionGradient-3MF): Add import support for FunctionGradient nodes once
        // the 3MF implicit function extension defines a corresponding node type.

        ProfileFunction using namespace gladius;
        switch (type)
        {
        case Lib3MF::eImplicitNodeType::Addition:
            return model.create<nodes::Addition>();
        case Lib3MF::eImplicitNodeType::Subtraction:
            return model.create<nodes::Subtraction>();
        case Lib3MF::eImplicitNodeType::Multiplication:
            return model.create<nodes::Multiplication>();
        case Lib3MF::eImplicitNodeType::Division:
            return model.create<nodes::Division>();
        case Lib3MF::eImplicitNodeType::Constant:
            return model.create<nodes::ConstantScalar>();
        case Lib3MF::eImplicitNodeType::ConstVec:
            return model.create<nodes::ConstantVector>();
        case Lib3MF::eImplicitNodeType::ConstMat:
            return model.create<nodes::ConstantMatrix>();
        case Lib3MF::eImplicitNodeType::ComposeVector:
            return model.create<nodes::ComposeVector>();
        case Lib3MF::eImplicitNodeType::DecomposeVector:
            return model.create<nodes::DecomposeVector>();
        case Lib3MF::eImplicitNodeType::ComposeMatrix:
            return model.create<nodes::ComposeMatrix>();
        case Lib3MF::eImplicitNodeType::MatrixFromColumns:
            return model.create<nodes::ComposeMatrixFromColumns>();
        case Lib3MF::eImplicitNodeType::MatrixFromRows:
            return model.create<nodes::ComposeMatrixFromRows>();
        case Lib3MF::eImplicitNodeType::Dot:
            return model.create<nodes::DotProduct>();
        case Lib3MF::eImplicitNodeType::Cross:
            return model.create<nodes::CrossProduct>();
        case Lib3MF::eImplicitNodeType::MatVecMultiplication:
            return model.create<nodes::MatrixVectorMultiplication>();
        case Lib3MF::eImplicitNodeType::Transpose:
            return model.create<nodes::Transpose>();
        case Lib3MF::eImplicitNodeType::Inverse:
            return model.create<nodes::Inverse>();
        case Lib3MF::eImplicitNodeType::Sinus:
            return model.create<nodes::Sine>();
        case Lib3MF::eImplicitNodeType::Cosinus:
            return model.create<nodes::Cosine>();
        case Lib3MF::eImplicitNodeType::Tan:
            return model.create<nodes::Tangent>();
        case Lib3MF::eImplicitNodeType::ArcSin:
            return model.create<nodes::ArcSin>();
        case Lib3MF::eImplicitNodeType::ArcCos:
            return model.create<nodes::ArcCos>();
        case Lib3MF::eImplicitNodeType::ArcTan:
            return model.create<nodes::ArcTan>();
        case Lib3MF::eImplicitNodeType::ArcTan2:
            return model.create<nodes::ArcTan2>();
        case Lib3MF::eImplicitNodeType::Min:
            return model.create<nodes::Min>();
        case Lib3MF::eImplicitNodeType::Max:
            return model.create<nodes::Max>();
        case Lib3MF::eImplicitNodeType::Abs:
            return model.create<nodes::Abs>();
        case Lib3MF::eImplicitNodeType::Fmod:
            return model.create<nodes::Fmod>();
        case Lib3MF::eImplicitNodeType::Pow:
            return model.create<nodes::Pow>();
        case Lib3MF::eImplicitNodeType::Sqrt:
            return model.create<nodes::Sqrt>();
        case Lib3MF::eImplicitNodeType::Exp:
            return model.create<nodes::Exp>();
        case Lib3MF::eImplicitNodeType::Log:
            return model.create<nodes::Log>();
        case Lib3MF::eImplicitNodeType::Log2:
            return model.create<nodes::Log2>();
        case Lib3MF::eImplicitNodeType::Log10:
            return model.create<nodes::Log10>();
        case Lib3MF::eImplicitNodeType::Select:
            return model.create<nodes::Select>();
        case Lib3MF::eImplicitNodeType::Clamp:
            return model.create<nodes::Clamp>();
        case Lib3MF::eImplicitNodeType::Sinh:
            return model.create<nodes::SinH>();
        case Lib3MF::eImplicitNodeType::Cosh:
            return model.create<nodes::CosH>();
        case Lib3MF::eImplicitNodeType::Tanh:
            return model.create<nodes::TanH>();
        case Lib3MF::eImplicitNodeType::Round:
            return model.create<nodes::Round>();
        case Lib3MF::eImplicitNodeType::Ceil:
            return model.create<nodes::Ceil>();
        case Lib3MF::eImplicitNodeType::Floor:
            return model.create<nodes::Floor>();
        case Lib3MF::eImplicitNodeType::Sign:
            return model.create<nodes::Sign>();
        case Lib3MF::eImplicitNodeType::Fract:
            return model.create<nodes::Fract>();
        case Lib3MF::eImplicitNodeType::FunctionCall:
            return model.create<nodes::FunctionCall>();
        case Lib3MF::eImplicitNodeType::Mesh:
            return model.create<nodes::SignedDistanceToMesh>();
        case Lib3MF::eImplicitNodeType::Length:
            return model.create<nodes::Length>();
        case Lib3MF::eImplicitNodeType::ConstResourceID:
            return model.create<nodes::Resource>();
        case Lib3MF::eImplicitNodeType::VectorFromScalar:
            return model.create<nodes::VectorFromScalar>();
        case Lib3MF::eImplicitNodeType::UnsignedMesh:
            return model.create<nodes::UnsignedDistanceToMesh>();
        case Lib3MF::eImplicitNodeType::Mod:
            return model.create<nodes::Mod>();
        case Lib3MF::eImplicitNodeType::BeamLattice:
            return model.create<nodes::SignedDistanceToBeamLattice>();
        case Lib3MF::eImplicitNodeType::FunctionGradient:
            return model.create<nodes::FunctionGradient>();
        case Lib3MF::eImplicitNodeType::NormalizeDistance:
            return model.create<nodes::NormalizeDistanceField>();
        default:
            throw std::runtime_error("Unknown node type");
            return nullptr;
        }
    }

    // Extract Nodename from "NodeName.OutputName"
    std::string extractNodeName(std::string const & name)
    {
        ProfileFunction auto const pos = name.find('.');
        if (pos == std::string::npos)
        {
            return name;
        }
        else
        {
            return name.substr(0, pos);
        }
    }

    // Extract OutputName from "NodeName.OutputName"
    std::string extractOutputName(std::string const & name)
    {
        ProfileFunction auto const pos = name.find('.');
        if (pos == std::string::npos)
        {
            return name;
        }
        else
        {
            return name.substr(pos + 1);
        }
    }

    // make name suitable as a opencl c variable name
    std::string makeValidVariableName(std::string const & name)
    {
        ProfileFunction std::string result;
        for (auto const c : name)
        {
            if (std::isalnum(c))
            {
                result += c;
            }
            else
            {
                result += '_';
            }
        }
        return result;
    }

    void Importer3mf::loadImplicitFunctions(Lib3MF::PModel fileModel, Document & doc)
    {
        ProfileFunction auto resourceIterator = fileModel->GetResources();
        while (resourceIterator->MoveNext())
        {
            auto res = resourceIterator->GetCurrent();
            auto implicitFunc = dynamic_cast<Lib3MF::CImplicitFunction *>(res.get());
            if (implicitFunc)
            {
                processImplicitFunction(doc, implicitFunc);
            }

            auto functionFromImage3d = dynamic_cast<Lib3MF::CFunctionFromImage3D *>(res.get());
            if (functionFromImage3d)
            {
                processFunctionFromImage3d(doc, functionFromImage3d);
            }
        }

        doc.getAssembly()->updateInputsAndOutputs();
    }

    void Importer3mf::loadImplicitFunctionsFiltered(Lib3MF::PModel fileModel,
                                                    Document & doc,
                                                    std::vector<Duplicates> const & duplicates)
    {
        ProfileFunction;

        // If there are no duplicates to filter out, just use the regular method
        if (duplicates.empty())
        {
            loadImplicitFunctions(fileModel, doc);
            return;
        }

        // Create a set of resource IDs to skip (the duplicate functions)
        std::set<Lib3MF_uint32> duplicateIds;
        for (auto const & duplicate : duplicates)
        {
            if (duplicate.duplicateFunction)
            {
                duplicateIds.insert(duplicate.duplicateFunction->GetUniqueResourceID());
            }
        }

        // Log the duplicate IDs
        for (auto const & duplicate : duplicates)
        {
            if (duplicate.duplicateFunction)
            {
                if (m_eventLogger)
                {
                    m_eventLogger->addEvent(
                      {fmt::format("Duplicate function detected with ID: {}",
                                   duplicate.duplicateFunction->GetUniqueResourceID()),
                       events::Severity::Info});
                }
            }
        }

        // Process resources, skipping those in the duplicateIds set
        auto resourceIterator = fileModel->GetResources();
        while (resourceIterator->MoveNext())
        {
            auto res = resourceIterator->GetCurrent();
            Lib3MF_uint32 resourceId = res->GetUniqueResourceID();

            auto implicitFuncCheck = dynamic_cast<Lib3MF::CImplicitFunction *>(res.get());
            if (implicitFuncCheck)
            {
            }

            // Skip if this resource is in our duplicates list
            if (duplicateIds.find(resourceId) != duplicateIds.end())
            {
                if (m_eventLogger)
                {
                    m_eventLogger->addEvent(
                      {fmt::format("Skipped loading duplicate function with ID: {}", resourceId),
                       events::Severity::Info});
                }
                continue;
            }

            // Process as usual if not a duplicate
            auto implicitFunc = dynamic_cast<Lib3MF::CImplicitFunction *>(res.get());
            if (implicitFunc)
            {
                processImplicitFunction(doc, implicitFunc);
            }

            auto functionFromImage3d = dynamic_cast<Lib3MF::CFunctionFromImage3D *>(res.get());
            if (functionFromImage3d)
            {
                processFunctionFromImage3d(doc, functionFromImage3d);
            }
        }

        doc.getAssembly()->updateInputsAndOutputs();
    }

    TextureTileStyle toTextureTileStyle(Lib3MF::eTextureTileStyle style)
    {
        ProfileFunction switch (style)
        {
        case Lib3MF::eTextureTileStyle::Wrap:
            return TextureTileStyle::TTS_REPEAT;
        case Lib3MF::eTextureTileStyle::Mirror:
            return TextureTileStyle::TTS_MIRROR;
        case Lib3MF::eTextureTileStyle::Clamp:
            return TextureTileStyle::TTS_CLAMP;
        case Lib3MF::eTextureTileStyle::NoTileStyle:
            return TextureTileStyle::TTS_NONE;
        default:
            return TextureTileStyle::TTS_REPEAT;
        }
    }

    SamplingFilter toSamplingFilter(Lib3MF::eTextureFilter filter)
    {
        ProfileFunction switch (filter)
        {
        case Lib3MF::eTextureFilter::Auto:
            return SF_LINEAR;
        case Lib3MF::eTextureFilter::Linear:
            return SF_LINEAR;
        case Lib3MF::eTextureFilter::Nearest:
            return SF_NEAREST;
        default:
            return SF_LINEAR;
        }
    }

    void Importer3mf::processFunctionFromImage3d(Document & doc,
                                                 Lib3MF::CFunctionFromImage3D * func)
    {
        ProfileFunction if (!func)
        {
            return;
        }

        nodes::SamplingSettings settings{};
        Lib3MF::eTextureTileStyle tileStyleU;
        Lib3MF::eTextureTileStyle tileStyleV;
        Lib3MF::eTextureTileStyle tileStyleW;
        func->GetTileStyles(tileStyleU, tileStyleV, tileStyleW);

        settings.tileStyleU = toTextureTileStyle(tileStyleU);
        settings.tileStyleV = toTextureTileStyle(tileStyleV);
        settings.tileStyleW = toTextureTileStyle(tileStyleW);

        settings.filter = toSamplingFilter(func->GetFilter());

        settings.offset = func->GetOffset();
        settings.scale = func->GetScale();

        nodes::Builder builder;
        builder.createFunctionFromImage3D(*doc.getAssembly(),
                                          func->GetModelResourceID(),
                                          func->GetImage3D()->GetModelResourceID(),
                                          settings);
    }

    void Importer3mf::processImplicitFunction(Document & doc, Lib3MF::CImplicitFunction * func)
    {
        ProfileFunction;

        m_eventLogger = doc.getSharedLogger();

        auto & assembly = *doc.getAssembly();

        auto const modelResId = func->GetModelResourceID();
        auto existingModel = assembly.findModel(modelResId);
        if (existingModel)
        {
            return;
        }

        assembly.addModelIfNotExisting(func->GetModelResourceID());

        auto & idToNode = m_nodeMaps[func->GetModelResourceID()];
        auto modelIter = doc.getAssembly()->getFunctions().find(func->GetModelResourceID());

        if (modelIter == doc.getAssembly()->getFunctions().end())
        {
            if (m_eventLogger)
            {
                m_eventLogger->addEvent(
                  {fmt::format("Failed to create model: {}", func->GetModelResourceID()),
                   events::Severity::Error});
            }
            return;
        }

        auto model = modelIter->second;

        if (!model)
        {
            if (m_eventLogger)
            {
                m_eventLogger->addEvent({fmt::format("Failed to create model, model is null: {}",
                                                     func->GetModelResourceID()),
                                         events::Severity::Error});
            }
            return;
        }

        model->setDisplayName(func->GetDisplayName());
        model->setResourceId(func->GetModelResourceID());

        model->createBeginEnd();
        auto inputIter = func->GetInputs();
        while (inputIter->MoveNext())
        {
            auto input = inputIter->GetCurrent();
            model->addArgument(makeValidVariableName(input->GetIdentifier()),
                               parameterFromType(input->GetType()));
        }

        auto nodeIter = func->GetNodes();
        // create nodes
        while (nodeIter->MoveNext())
        {
            auto node3mf = nodeIter->GetCurrent();

            auto * newNode = createNode(*model, node3mf->GetNodeType());

            if (node3mf->GetNodeType() == Lib3MF::eImplicitNodeType::FunctionCall)
            {
                // add all arguments
                auto inputIter = node3mf->GetInputs();
                while (inputIter->MoveNext())
                {
                    auto input = inputIter->GetCurrent();
                    auto newInput = newNode->addInput(input->GetIdentifier());
                    *newInput = parameterFromType(input->GetType());
                    newInput->setParentId(newNode->getId());
                }

                // add all outputs
                auto outputIter = node3mf->GetOutputs();
                while (outputIter->MoveNext())
                {
                    auto output = outputIter->GetCurrent();
                    newNode->addOutputPort(output->GetIdentifier(),
                                           typeIndexFrom3mfType(output->GetType()));
                }
            }

            // For ComposeMatrix, MatrixFromColumns and MatrixFromRows replace "Matrix" output with
            // "Result"
            if (node3mf->GetNodeType() == Lib3MF::eImplicitNodeType::ComposeMatrix ||
                node3mf->GetNodeType() == Lib3MF::eImplicitNodeType::MatrixFromColumns ||
                node3mf->GetNodeType() == Lib3MF::eImplicitNodeType::MatrixFromRows)
            {
                auto * port = newNode->findOutputPort("Matrix");
                if (port)
                {
                    newNode->getOutputs().erase("Matrix");
                    port->setUniqueName(newNode->getUniqueName() + "_" + "Result");
                    port->setShortName("Result");
                    newNode->getOutputs()["Result"] = *port;
                }
            }
            newNode->setUniqueName(node3mf->GetIdentifier());
            // tag
            newNode->setTag(node3mf->GetTag());
            model->registerInputs(*newNode);
            model->registerOutputs(*newNode);
            idToNode[makeValidVariableName(node3mf->GetIdentifier())] = newNode;
            newNode->setDisplayName(node3mf->GetDisplayName());
        }

        nodeIter = func->GetNodes();
        // connect nodes
        while (nodeIter->MoveNext())
        {
            auto node3mf = nodeIter->GetCurrent();
            connectNode(*node3mf, *func, *model);
        }

        connectOutputs(*model, *model->getEndNode(), *func);

        // Don't set logger or call updateTypes() during import - validation runs after loading
        // completes and will update types properly. Setting the logger here causes validation
        // errors to be logged for the temporarily incomplete graph.
    }

    void Importer3mf::connectNode(Lib3MF::CImplicitNode & node3mf,
                                  Lib3MF::CImplicitFunction & func,
                                  nodes::Model & model)
    {
        ProfileFunction auto & idToNode = m_nodeMaps[func.GetModelResourceID()];

        auto node = idToNode.at(makeValidVariableName(node3mf.GetIdentifier()));
        auto inputIter = node3mf.GetInputs();
        while (inputIter->MoveNext())
        {
            auto input = inputIter->GetCurrent();
            auto parameterName = makeValidVariableName(input->GetIdentifier());
            auto * parameter = node->getParameter(parameterName);
            auto const nodeType = node3mf.GetNodeType();
            bool const nodeIsFunctionCall = nodeType == Lib3MF::eImplicitNodeType::FunctionCall;
            bool const nodeIsFunctionGradient =
              nodeType == Lib3MF::eImplicitNodeType::FunctionGradient;
            bool const nodeIsNormalizeDistance =
              nodeType == Lib3MF::eImplicitNodeType::NormalizeDistance;

            bool const allowDynamicInputs =
              nodeIsFunctionCall || nodeIsFunctionGradient || nodeIsNormalizeDistance;

            if (!parameter && allowDynamicInputs)
            {
                parameter = node->addInput(parameterName);
            }

            if (!parameter)
            {
                // Skip silently - validation will catch missing parameters after load
                continue;
            }

            // Skip internal parameters (e.g. start/end memory offsets on mesh distance
            // nodes). They are runtime-only and must never be loaded from a 3MF file.
            // Warn so the user knows the file was malformed and will be fixed on next save.
            if (parameter->isInternal())
            {
                if (m_eventLogger)
                {
                    m_eventLogger->addEvent(
                      {fmt::format("Ignoring internal parameter '{}' on node '{}'. "
                                   "This parameter is for internal use only and will be removed "
                                   "from the file when saved.",
                                   parameterName,
                                   node->getUniqueName()),
                       events::Severity::Warning});
                }
                continue;
            }

            *parameter = parameterFromType(input->GetType());
            parameter->setParentId(node->getId());

            if (parameterName == nodes::FieldNames::FunctionId)
            {
                parameter->setInputSourceRequired(false);
            }
            else if (parameterName == nodes::FieldNames::StepSize)
            {
                parameter->setInputSourceRequired(false);
                parameter->setModifiable(true);
                parameter->setValue(1e-3f);
            }
            else if (allowDynamicInputs)
            {
                parameter->marksAsArgument();
                parameter->setInputSourceRequired(true);
            }

            auto sourcePort = resolveInput(model, input);
            if (sourcePort)
            {
                parameter->setInputFromPort(*sourcePort);
            }
        }

        if (node3mf.GetNodeType() == Lib3MF::eImplicitNodeType::Constant)
        {
            auto scalarNode = dynamic_cast<Lib3MF::CConstantNode *>(&node3mf);

            if (!scalarNode)
            {
                throw std::runtime_error(
                  fmt::format("Could not cast node {} to ConstScalarNode", node->getUniqueName()));
            }
            auto value = scalarNode->GetConstant();
            auto * parameter = node->getParameter(nodes::FieldNames::Value);
            if (parameter)
            {
                parameter->setValue(static_cast<float>(value));
                parameter->setInputSourceRequired(false); // Fix: Constants don't need input sources
            }
        }
        else if (node3mf.GetNodeType() == Lib3MF::eImplicitNodeType::ConstVec)
        {
            auto vectorNode = dynamic_cast<Lib3MF::CConstVecNode *>(&node3mf);
            if (!vectorNode)
            {
                throw std::runtime_error(
                  fmt::format("Could not cast node {} to ConstVecNode", node->getUniqueName()));
            }
            auto value = vectorNode->GetVector();

            int i = 0;
            for (auto const & fieldName :
                 {nodes::FieldNames::X, nodes::FieldNames::Y, nodes::FieldNames::Z})
            {
                auto * parameter = node->getParameter(fieldName);
                if (parameter)
                {
                    parameter->setValue(static_cast<float>(value.m_Coordinates[i]));
                    parameter->setInputSourceRequired(
                      false); // Fix: Constants don't need input sources
                }
                i++;
            }
        }
        else if (node3mf.GetNodeType() == Lib3MF::eImplicitNodeType::ConstMat)
        {
            auto matrixNode = dynamic_cast<Lib3MF::CConstMatNode *>(&node3mf);
            if (!matrixNode)
            {
                throw std::runtime_error(
                  fmt::format("Could not cast node {} to ConstMatNode", node->getUniqueName()));
            }
            auto matrix = matrixNode->GetMatrix();
            int i = 0;
            for (auto const & fieldNames : {nodes::FieldNames::M00,
                                            nodes::FieldNames::M01,
                                            nodes::FieldNames::M02,
                                            nodes::FieldNames::M03,
                                            nodes::FieldNames::M10,
                                            nodes::FieldNames::M11,
                                            nodes::FieldNames::M12,
                                            nodes::FieldNames::M13,
                                            nodes::FieldNames::M20,
                                            nodes::FieldNames::M21,
                                            nodes::FieldNames::M22,
                                            nodes::FieldNames::M23,
                                            nodes::FieldNames::M30,
                                            nodes::FieldNames::M31,
                                            nodes::FieldNames::M32,
                                            nodes::FieldNames::M33})
            {
                auto * parameter = node->getParameter(fieldNames);
                auto col = i % 4;
                auto row = i / 4;
                if (parameter)
                {
                    parameter->setValue(static_cast<float>(matrix.m_Field[row][col]));
                    parameter->setInputSourceRequired(
                      false); // Fix: Constants don't need input sources
                }
                i++;
            }
        }
        else if (node3mf.GetNodeType() == Lib3MF::eImplicitNodeType::ConstResourceID)
        {
            auto resourceNode3mf = dynamic_cast<Lib3MF::CResourceIdNode *>(&node3mf);
            if (!resourceNode3mf)
            {
                throw std::runtime_error(
                  fmt::format("Could not cast node {} to ResourceIdNode", node3mf.GetIdentifier()));
            }
            auto resourceNode = dynamic_cast<nodes::Resource *>(&*node);
            if (!resourceNode)
            {
                throw std::runtime_error(
                  fmt::format("Could not cast node {} to ResourceNode", node->getUniqueName()));
            }
            auto resource = resourceNode3mf->GetResource();

            if (!resource)
            {
                if (m_eventLogger)
                {
                    m_eventLogger->addEvent(
                      {fmt::format("Resource not found: {}", node3mf.GetIdentifier()),
                       events::Severity::Warning});
                }
                return;
            }
            auto resourceId = resource->GetModelResourceID();
            resourceNode->setResourceId(resourceId);
            
            // Mark resourceid parameter as not requiring input connection - value is set directly
            auto * param = resourceNode->getParameter(nodes::FieldNames::ResourceId);
            if (param)
            {
                param->setInputSourceRequired(false);
            }
        }

        if (node3mf.GetNodeType() == Lib3MF::eImplicitNodeType::FunctionGradient)
        {
            auto gradientNode3mf = dynamic_cast<Lib3MF::CFunctionGradientNode *>(&node3mf);
            auto gradientNode = dynamic_cast<nodes::FunctionGradient *>(node);
            if (gradientNode && gradientNode3mf)
            {
                gradientNode->setSelectedScalarOutput(gradientNode3mf->GetScalarOutputName());
                gradientNode->setSelectedVectorInput(gradientNode3mf->GetVectorInputName());
                gradientNode->resolveFunctionId();
            }
        }
        else if (node3mf.GetNodeType() == Lib3MF::eImplicitNodeType::NormalizeDistance)
        {
            auto normalizeNode3mf = dynamic_cast<Lib3MF::CNormalizeDistanceNode *>(&node3mf);
            auto normalizeNode = dynamic_cast<nodes::NormalizeDistanceField *>(node);
            if (normalizeNode && normalizeNode3mf)
            {
                normalizeNode->setSelectedScalarOutput(normalizeNode3mf->GetScalarOutputName());
                normalizeNode->setSelectedVectorInput(normalizeNode3mf->GetVectorInputName());
                normalizeNode->resolveFunctionId();
            }
        }
    }

    void Importer3mf::connectOutputs(gladius::nodes::Model & model,
                                     gladius::nodes::NodeBase & endNode,
                                     Lib3MF::CImplicitFunction & func)
    {
        ProfileFunction
        {

            auto outputIter = func.GetOutputs();
            while (outputIter->MoveNext())
            {
                auto output = outputIter->GetCurrent();

                auto parameterName = makeValidVariableName(output->GetIdentifier());
                auto parameter = endNode.parameter()[parameterName] =
                  parameterFromType(output->GetType());
            }

            model.registerInputs(endNode);
        }
        {
            auto outputIter = func.GetOutputs();
            while (outputIter->MoveNext())
            {
                auto output = outputIter->GetCurrent();

                auto parameterName = makeValidVariableName(output->GetIdentifier());
                auto parameter = endNode.getParameter(parameterName);
                if (!parameter)
                {
                    // Skip silently - validation will catch missing parameters after load
                    continue;
                }

                auto sourcePort = resolveInput(model, output);
                if (sourcePort)
                {
                    // Silently ignore link failures - validation will catch these after load
                    (void) model.addLink(sourcePort->getId(), parameter->getId());
                }
                // Skip silently if source port not found - validation will catch these after load
            }
        }
    }

    nodes::Port * Importer3mf::resolveInput(nodes::Model & model, Lib3MF::PImplicitPort & input)
    {
        ProfileFunction

          auto refName = input->GetReference();
        if (refName.empty())
        {
            return nullptr;
        }
        auto sourceNodeName = makeValidVariableName(extractNodeName(refName));
        auto & idToNode = m_nodeMaps.at(model.getResourceId());

        gladius::nodes::NodeBase * sourceNode = nullptr;
        if (sourceNodeName == "inputs")
        {
            sourceNode = model.getBeginNode();
        }
        else
        {
            auto sourceNodeIter = idToNode.find(sourceNodeName);
            if (sourceNodeIter == idToNode.end())
            {
                // Skip silently - validation will catch missing nodes after load
                return nullptr;
            }
            sourceNode = sourceNodeIter->second;
        }

        auto & outputs = sourceNode->getOutputs();
        auto sourcePortName = makeValidVariableName(extractOutputName(refName));
        auto sourcePortIter = outputs.find(sourcePortName);
        if (sourcePortIter == outputs.end())
        {
            // Legacy files used ".matrix" as the output name; newer versions use "Result"
            std::string lowerRef = refName;
            std::transform(lowerRef.begin(), lowerRef.end(), lowerRef.begin(), ::tolower);
            auto matrixPos = lowerRef.rfind(".matrix");
            if (matrixPos != std::string::npos)
            {
                std::string legacyRef = refName;
                std::string fallbackRef = refName.substr(0, matrixPos) + ".Result";
                auto fallbackPortName = makeValidVariableName(extractOutputName(fallbackRef));
                auto fallbackIter = outputs.find(fallbackPortName);

                if (fallbackIter == outputs.end())
                {
                    // Also try lowercase variant for safety
                    fallbackRef = refName.substr(0, matrixPos) + ".result";
                    fallbackPortName = makeValidVariableName(extractOutputName(fallbackRef));
                    fallbackIter = outputs.find(fallbackPortName);
                }

                if (fallbackIter != outputs.end())
                {
                    input->SetReference(fallbackRef);
                    if (m_eventLogger)
                    {
                        m_eventLogger->addEvent({fmt::format("Resolved legacy reference {} to {}",
                                                             legacyRef,
                                                             fallbackRef),
                                                 events::Severity::Info});
                    }
                    return &fallbackIter->second;
                }
            }

            // Skip silently - validation will catch missing ports after load
            return nullptr;
        }
        return &sourcePortIter->second;
    }

    Vector3 toVector3(Lib3MF::sPosition const & a)
    {
        return Vector3(a.m_Coordinates[0], a.m_Coordinates[1], a.m_Coordinates[2]);
    }

    openvdb::Vec3s toOpenVdbVector(Lib3MF::sPosition const & a)
    {
        return openvdb::Vec3s(a.m_Coordinates[0], a.m_Coordinates[1], a.m_Coordinates[2]);
    }

    nodes::Matrix4x4 matrix4x4From3mfTransform(Lib3MF::sTransform const & transform)
    {
        nodes::Matrix4x4 mat = identityMatrix();

        for (int row = 0; row < 4; ++row)
        {
            for (int col = 0; col < 3; ++col)
            {
                mat[row][col] = transform.m_Fields[row][col];
            }
        }

        return mat;
    }

    std::vector<Lib3MF::PImplicitFunction>
    Importer3mf::collectImplicitFunctions(Lib3MF::PModel const & model) const
    {
        ProfileFunction std::vector<Lib3MF::PImplicitFunction> implicitFunctions;
        auto resourceIterator = model->GetResources();

        while (resourceIterator->MoveNext())
        {
            auto resource = resourceIterator->GetCurrent();
            auto implicitFunc = std::dynamic_pointer_cast<Lib3MF::CImplicitFunction>(resource);

            if (implicitFunc)
            {
                implicitFunctions.push_back(implicitFunc);
            }
        }

        return implicitFunctions;
    }

    std::vector<Duplicates> Importer3mf::findDuplicatedFunctions(
      std::vector<Lib3MF::PImplicitFunction> const & originalFunctions,
      Lib3MF::PModel const & extendedModel) const
    {
        ProfileFunction std::vector<Duplicates> duplicates;

        // For each original function, search for an equivalent in the extended model
        for (const auto & originalFunction : originalFunctions)
        {
            // Skip if function is null (should not happen, but let's be safe)
            if (!originalFunction)
            {
                continue;
            }

            // Use the FunctionComparator to find an equivalent function
            auto equivalentFunction = findEquivalentFunction(*extendedModel, *originalFunction);

            // If an equivalent function is found, store the function pointers in the result
            if (equivalentFunction)
            {
                Duplicates duplicate{originalFunction, equivalentFunction};

                duplicates.push_back(duplicate);
            }
        }

        return duplicates;
    }

    std::set<Lib3MF_uint32>
    Importer3mf::collectFunctionResourceIds(Lib3MF::PModel const & model) const
    {
        ProfileFunction std::set<Lib3MF_uint32> functionResourceIds;
        auto resourceIterator = model->GetResources();
        while (resourceIterator->MoveNext())
        {
            auto resource = resourceIterator->GetCurrent();
            auto implicitFunc = dynamic_cast<Lib3MF::CImplicitFunction *>(resource.get());
            auto functionFromImage3d = dynamic_cast<Lib3MF::CFunctionFromImage3D *>(resource.get());

            if (implicitFunc || functionFromImage3d)
            {
                functionResourceIds.insert(resource->GetResourceID());
            }
        }
        return functionResourceIds;
    }

    void
    Importer3mf::replaceDuplicatedFunctionReferences(std::vector<Duplicates> const & duplicates,
                                                     Lib3MF::PModel const & model) const
    {
        ProfileFunction

          // If no duplicates were found, no need to replace anything
          if (duplicates.empty())
        {
            return;
        }

        // Get all implicit functions from the model
        auto resourceIterator = model->GetResources();

        // Iterate through all resources
        while (resourceIterator->MoveNext())
        {
            auto resource = resourceIterator->GetCurrent();

            // Check if the resource is an implicit function
            auto implicitFunction = std::dynamic_pointer_cast<Lib3MF::CImplicitFunction>(resource);
            if (!implicitFunction)
            {
                // Not an implicit function, skip
                continue;
            }

            // Get all nodes in this function
            auto nodeIterator = implicitFunction->GetNodes();

            // Iterate through all nodes in the function
            while (nodeIterator->MoveNext())
            {
                auto node = nodeIterator->GetCurrent();

                // Check if this is a ResourceIdNode (ConstResourceID type)
                if (node->GetNodeType() == Lib3MF::eImplicitNodeType::ConstResourceID)
                {
                    auto resourceIdNode = std::dynamic_pointer_cast<Lib3MF::CResourceIdNode>(node);
                    if (!resourceIdNode)
                    {
                        if (m_eventLogger)
                        {
                            m_eventLogger->addEvent(
                              {fmt::format("Could not cast node {} to ResourceIdNode",
                                           node->GetIdentifier()),
                               events::Severity::Warning});
                        }
                        continue;
                    }

                    // Get the resource referenced by this node
                    Lib3MF::PResource referencedResource;
                    try
                    {
                        referencedResource = resourceIdNode->GetResource();
                    }
                    catch (const std::exception & e)
                    {
                        if (m_eventLogger)
                        {
                            m_eventLogger->addEvent(
                              {fmt::format("Error retrieving resource from ResourceIdNode {}: {}",
                                           node->GetIdentifier(),
                                           e.what()),
                               events::Severity::Warning});
                        }
                        continue;
                    }

                    if (!referencedResource)
                    {
                        if (m_eventLogger)
                        {
                            m_eventLogger->addEvent(
                              {fmt::format("ResourceIdNode {} references a null resource",
                                           node->GetIdentifier()),
                               events::Severity::Warning});
                        }
                        continue;
                    }

                    // Check if this resource is one of our duplicate functions
                    for (const auto & duplicate : duplicates)
                    {
                        // If the referenced resource ID matches a duplicate function's ID
                        if (referencedResource->GetUniqueResourceID() ==
                            duplicate.duplicateFunction->GetUniqueResourceID())
                        {
                            // Replace the reference with the original function
                            try
                            {
                                auto originalResource = model->GetResourceByID(
                                  duplicate.originalFunction->GetUniqueResourceID());
                                resourceIdNode->SetResource(originalResource);

                                if (m_eventLogger)
                                {
                                    m_eventLogger->addEvent(
                                      {fmt::format(
                                         "Replaced reference to duplicate function {} "
                                         "with original function {}",
                                         duplicate.duplicateFunction->GetUniqueResourceID(),
                                         duplicate.originalFunction->GetUniqueResourceID()),
                                       events::Severity::Info});
                                }
                            }
                            catch (const std::exception & e)
                            {
                                if (m_eventLogger)
                                {
                                    m_eventLogger->addEvent(
                                      {fmt::format(
                                         "Error replacing function reference in node {}: {}",
                                         node->GetIdentifier(),
                                         e.what()),
                                       events::Severity::Error});
                                }
                            }

                            // We've handled this node, no need to check other duplicates
                            break;
                        }
                    }
                }
            }
        }
    }

    void Importer3mf::collectBooleanReferencedMeshIds(
      Lib3MF::PModel const & model,
      Lib3MF::PBooleanObject const & booleanObject,
      std::set<Lib3MF_uint32> & meshIds,
      std::set<Lib3MF_uint32> & visitedBooleanIds) const
    {
        if (!model || !booleanObject)
        {
            return;
        }

        auto const booleanId = booleanObject->GetModelResourceID();
        if (!visitedBooleanIds.insert(booleanId).second)
        {
            return;
        }

        try
        {
            auto const baseObject = booleanObject->GetBaseObject();
            if (baseObject && baseObject->IsMeshObject())
            {
                meshIds.insert(baseObject->GetModelResourceID());
            }
            else if (baseObject && baseObject->IsBooleanObject())
            {
                auto const baseBoolean =
                  model->GetBooleanObjectByID(baseObject->GetUniqueResourceID());
                collectBooleanReferencedMeshIds(model, baseBoolean, meshIds, visitedBooleanIds);
            }
        }
        catch (std::exception const & e)
        {
            if (m_eventLogger)
            {
                m_eventLogger->addEvent(
                  {fmt::format("Could not collect boolean base mesh references for resource {}: {}",
                               booleanId,
                               e.what()),
                   events::Severity::Warning});
            }
        }

        auto const operandCount = booleanObject->GetOperandCount();
        for (Lib3MF_uint32 operandIndex = 0; operandIndex < operandCount; ++operandIndex)
        {
            try
            {
                Lib3MF::PMeshObject operandMesh;
                (void) booleanObject->GetOperand(operandIndex, operandMesh);
                if (operandMesh)
                {
                    meshIds.insert(operandMesh->GetModelResourceID());
                }
            }
            catch (std::exception const & e)
            {
                if (m_eventLogger)
                {
                    m_eventLogger->addEvent(
                      {fmt::format("Could not collect boolean operand {} mesh reference for "
                                   "resource {}: {}",
                                   operandIndex,
                                   booleanId,
                                   e.what()),
                       events::Severity::Warning});
                }
            }
        }
    }

    std::set<Lib3MF_uint32> Importer3mf::collectBboxOnlyMeshIds(Lib3MF::PModel const & model) const
    {
        std::set<Lib3MF_uint32> bboxOnlyIds;
        std::set<Lib3MF_uint32> regularIds;

        auto objectIterator = model->GetObjects();
        while (objectIterator->MoveNext())
        {
            auto const object = objectIterator->GetCurrentObject();
            if (!object->IsLevelSetObject())
            {
                continue;
            }
            auto levelSet = model->GetLevelSetByID(object->GetUniqueResourceID());
            if (!levelSet)
            {
                continue;
            }
            auto mesh = levelSet->GetMesh();
            if (!mesh)
            {
                continue;
            }
            auto const meshId = mesh->GetModelResourceID();
            if (levelSet->GetMeshBBoxOnly())
            {
                bboxOnlyIds.insert(meshId);
            }
            else
            {
                regularIds.insert(meshId);
            }
        }

        auto booleanIterator = model->GetBooleanObjects();
        while (booleanIterator->MoveNext())
        {
            std::set<Lib3MF_uint32> visitedBooleanIds;
            collectBooleanReferencedMeshIds(model,
                                            booleanIterator->GetCurrentBooleanObject(),
                                            regularIds,
                                            visitedBooleanIds);
        }

        // Return only IDs that are exclusively used as bounding boxes (not also as real geometry)
        std::set<Lib3MF_uint32> result;
        for (auto const id : bboxOnlyIds)
        {
            if (regularIds.find(id) == regularIds.end())
            {
                result.insert(id);
            }
        }
        return result;
    }

    void Importer3mf::loadMeshes(Lib3MF::PModel model, Document & doc)
    {
        ProfileFunction

        auto const bboxOnlyMeshIds = collectBboxOnlyMeshIds(model);

        auto objectIterator = model->GetObjects();
        while (objectIterator->MoveNext())
        {
            auto const object = objectIterator->GetCurrentObject();

            if (object->IsMeshObject())
            {
                if (bboxOnlyMeshIds.count(object->GetModelResourceID()) != 0u)
                {
                    // Skip meshes referenced only as bounding boxes in level sets;
                    // loading them would waste memory and trigger expensive NanoVDB builds.
                    continue;
                }
                auto const meshObj = model->GetMeshObjectByID(object->GetUniqueResourceID());
                loadMeshIfNecessary(model, meshObj, doc);
            }
            else
            {
                // Skip non-mesh objects
            }
        }
    }

    void Importer3mf::loadMeshes(Lib3MF::PModel model,
                                 Document & doc,
                                 std::set<Lib3MF_uint32> const & resourceIds)
    {
        ProfileFunction

        auto const bboxOnlyMeshIds = collectBboxOnlyMeshIds(model);

        auto objectIterator = model->GetObjects();
        while (objectIterator->MoveNext())
        {
            auto const object = objectIterator->GetCurrentObject();

            if (!object->IsMeshObject())
            {
                continue;
            }

            if (bboxOnlyMeshIds.count(object->GetModelResourceID()) != 0u)
            {
                // Skip meshes referenced only as bounding boxes in level sets;
                // they are needed only for domain AABBs and must never be
                // instantiated as SpatialMeshResources.
                continue;
            }

            if (resourceIds.find(object->GetResourceID()) == resourceIds.end())
            {
                continue;
            }

            auto const meshObj = model->GetMeshObjectByID(object->GetUniqueResourceID());
            loadMeshIfNecessary(model, meshObj, doc);
        }
    }

    void Importer3mf::loadComponentObjects(Lib3MF::PModel model, Document & doc)
    {
        ProfileFunction auto objectIterator = model->GetObjects();
        nodes::Builder builder;
        float const units_per_mm = computeUnitsPerMM(model);
        while (objectIterator->MoveNext())
        {
            auto const object = objectIterator->GetCurrentObject();

            if (object->IsComponentsObject())
            {
                nodes::Components components;
                auto const compObjs = model->GetComponentsObjectByID(object->GetUniqueResourceID());

                for (Lib3MF_uint32 i = 0; i < compObjs->GetComponentCount(); ++i)
                {
                    auto const component = compObjs->GetComponent(i);
                    components.push_back({component->GetObjectResourceID(),
                                          matrix4x4From3mfTransform(component->GetTransform())});
                }

                builder.addCompositeModel(doc, object->GetResourceID(), components, units_per_mm);
            }
        }
    }

    void Importer3mf::loadMeshIfNecessary(Lib3MF::PModel model,
                                          Lib3MF::PMeshObject meshObject,
                                          Document & doc)
    {
        ProfileFunction auto key =
          ResourceKey(static_cast<uint32_t>(meshObject->GetModelResourceID()), ResourceType::Mesh);
        key.setDisplayName(meshObject->GetName());

        if (doc.getGeneratorContext().resourceManager.hasResource(key))
        {
            return;
        }

        auto const numVertices = meshObject->GetVertexCount();
        auto const numFaces = meshObject->GetTriangleCount();

        if (numFaces == 0u)
        {
            // Still check for beam lattice even if mesh has no triangles
            loadBeamLatticeIfNecessary(model, meshObject, doc);
            return;
        }

        // Build spatial mesh data using BVH for fast SDF queries
        std::vector<float4> vertices;
        std::vector<TriangleIndices> indices;
        vertices.reserve(numVertices);
        indices.reserve(numFaces);

        {
            ZoneScopedN("Importer3mf::extractMeshBuffers");
            for (auto vertexIndex = 0u; vertexIndex < numVertices; ++vertexIndex)
            {
                auto const v = meshObject->GetVertex(vertexIndex);
                vertices.push_back(float4{v.m_Coordinates[0], v.m_Coordinates[1], v.m_Coordinates[2], 0.0f});
            }

            for (auto faceIndex = 0u; faceIndex < numFaces; ++faceIndex)
            {
                auto const & triangle = meshObject->GetTriangle(faceIndex);
                indices.push_back(TriangleIndices{
                    static_cast<int>(triangle.m_Indices[0]),
                    static_cast<int>(triangle.m_Indices[1]),
                    static_cast<int>(triangle.m_Indices[2])
                });
            }
        }

        // Optional mesh repair pre-pass (welding, degenerate removal, orientation,
        // small-hole filling). All steps default to disabled — when nothing is
        // enabled this call is a no-op.
        mesh_repair::MeshRepairResult repairResult{};
        {
            ZoneScopedN("Importer3mf::repairMesh");
            repairResult = mesh_repair::repairMesh(vertices, indices, m_meshRepairConfig);
        }
        if (m_eventLogger &&
            (repairResult.weldedVertices != 0u || repairResult.removedTriangles != 0u ||
             repairResult.flippedTriangles != 0u || repairResult.filledHoles != 0u))
        {
            m_eventLogger->addEvent({fmt::format(
                                       "Mesh repair on resource {}: welded {} vertices, removed {} "
                                       "degenerate triangles, flipped {}, filled {} holes "
                                       "({} triangles added)",
                                       meshObject->GetModelResourceID(),
                                       repairResult.weldedVertices,
                                       repairResult.removedTriangles,
                                       repairResult.flippedTriangles,
                                       repairResult.filledHoles,
                                       repairResult.addedTriangles),
                                     gladius::events::Severity::Info});
        }

        SpatialMeshData spatialData;
        {
            ZoneScopedN("Importer3mf::buildSpatialMeshData");
            MeshBVHBuilder builder;
            spatialData = builder.build(vertices, indices);
        }
        auto & resourceManager = doc.getGeneratorContext().resourceManager;
        resourceManager.addResource(
          key, std::move(spatialData), m_meshSdfEvaluationConfig, m_nanovdbBuildPolicy);

        auto * resource = resourceManager.getResourcePtr(key);
        auto * spatialMesh = dynamic_cast<SpatialMeshResource *>(resource);
        if (spatialMesh != nullptr && spatialMesh->hasMeshQualityIssues())
        {
            auto const message = spatialMesh->formatMeshQualityMessage(key.getDisplayName());
            bool const strictNanoVdb = m_meshSdfEvaluationConfig.method == MeshSdfMethod::NanoVDB &&
                                       m_nanovdbBuildPolicy.failurePolicy ==
                                           NanoVdbFailurePolicy::Fail;
            if (m_eventLogger)
            {
                m_eventLogger->addEvent({message,
                                         strictNanoVdb ? gladius::events::Severity::Error
                                                       : gladius::events::Severity::Warning});
            }
            if (strictNanoVdb)
            {
                resourceManager.deleteResource(key);
                throw NanoVdbBuildRejectedError(message);
            }
        }
        if (spatialMesh != nullptr && spatialMesh->hasNanoVdbBuildIssue())
        {
            auto const message = spatialMesh->formatNanoVdbBuildMessage(key.getDisplayName());
            if (m_eventLogger)
            {
                m_eventLogger->addEvent({message,
                                         m_nanovdbBuildPolicy.failurePolicy ==
                                             NanoVdbFailurePolicy::Fail
                                           ? gladius::events::Severity::Error
                                           : gladius::events::Severity::Warning});
            }

            if (m_nanovdbBuildPolicy.failurePolicy == NanoVdbFailurePolicy::Fail)
            {
                resourceManager.deleteResource(key);
                throw NanoVdbBuildRejectedError(message);
            }
        }

        // Also load beam lattice if present
        loadBeamLatticeIfNecessary(model, meshObject, doc);
    }

    void Importer3mf::loadBeamLatticeIfNecessary(Lib3MF::PModel model,
                                                 Lib3MF::PMeshObject meshObject,
                                                 Document & doc)
    {
        ProfileFunction

        try
        {
            // Create resource key for beam lattice (use same resource ID but different type)
            auto key = ResourceKey(static_cast<uint32_t>(meshObject->GetModelResourceID()),
                                   ResourceType::BeamLattice);
            key.setDisplayName(meshObject->GetName() + "_BeamLattice");

            // Check if beam lattice resource already exists
            if (doc.getGeneratorContext().resourceManager.hasResource(key))
            {
                return;
            }

            // Use the new BeamLatticeImporter for unified processing
            BeamLatticeImporter importer(m_eventLogger);
            if (!importer.process(meshObject))
            {
                return; // No beam lattice or processing failed
            }

            // Get processed data from importer
            const auto & beams = importer.getBeams();
            const auto & balls = importer.getBalls();
            const auto & ballConfig = importer.getBallConfig();

            if (beams.empty())
            {
                // Having a beam lattice without beams is permitted by the 3MF spec
                // and should not be treated as a warning. Log as informational and skip.
                if (m_eventLogger)
                {
                    m_eventLogger->addEvent({fmt::format("BeamLattice {} has no beams — skipping",
                                                         meshObject->GetModelResourceID()),
                                             gladius::events::Severity::Info});
                }
                return;
            }

            // Read clipping information from beam lattice (separate from main processing)
            Lib3MF::eBeamLatticeClipMode clippingMode = Lib3MF::eBeamLatticeClipMode::NoClipMode;
            Lib3MF_uint32 clippingMeshResourceId = 0;

            try
            {
                Lib3MF::PBeamLattice beamLattice = meshObject->BeamLattice();
                if (beamLattice)
                {
                    beamLattice->GetClipping(clippingMode, clippingMeshResourceId);
                }
            }
            catch (const std::exception & e)
            {
                // If clipping info is not available, continue with no clipping
                clippingMode = Lib3MF::eBeamLatticeClipMode::NoClipMode;
                if (m_eventLogger)
                {
                    m_eventLogger->addEvent(
                      {fmt::format(
                         "Warning: Could not read clipping information from beam lattice {}: {}",
                         meshObject->GetModelResourceID(),
                         e.what()),
                       gladius::events::Severity::Warning});
                }
            }

            // Create beam lattice resource with processed data
            auto beamLatticeResource =
              std::make_unique<BeamLatticeResource>(key,
                                                    std::move(std::vector<BeamData>(beams)),
                                                    std::move(std::vector<BallData>(balls)),
                                                    ballConfig);

            // Update display name to include ball information
            std::string displayName = meshObject->GetName() + "_BeamLattice";
            if (!balls.empty())
            {
                displayName += fmt::format(" ({} balls)", balls.size());
            }
            key.setDisplayName(displayName);

            // Add the resource to the manager
            doc.getGeneratorContext().resourceManager.addResource(key,
                                                                  std::move(beamLatticeResource));

            if (m_eventLogger)
            {
                m_eventLogger->addEvent(
                  {fmt::format("Successfully loaded beam lattice {} with {} beams and {} balls",
                               meshObject->GetModelResourceID(),
                               beams.size(),
                               balls.size()),
                   gladius::events::Severity::Info});
            }
        }
        catch (const std::exception & e)
        {
            if (m_eventLogger)
            {
                m_eventLogger->addEvent({fmt::format("Error loading beam lattice from mesh {}: {}",
                                                     meshObject->GetModelResourceID(),
                                                     e.what()),
                                         gladius::events::Severity::Error});
            }
        }
    }

    BoundingBox Importer3mf::computeBoundingBox(Lib3MF::PMeshObject mesh)
    {
        ProfileFunction BoundingBox bbox;
        bbox.min = {std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max(),
                    std::numeric_limits<float>::max(),
                    1.f};
        bbox.max = {-std::numeric_limits<float>::max(),
                    -std::numeric_limits<float>::max(),
                    -std::numeric_limits<float>::max(),
                    1.f};

        for (auto i = 0u; i < mesh->GetVertexCount(); ++i)
        {
            auto const vertex = mesh->GetVertex(i);

            bbox.min.x = std::min(bbox.min.x, vertex.m_Coordinates[0]);
            bbox.min.y = std::min(bbox.min.y, vertex.m_Coordinates[1]);
            bbox.min.z = std::min(bbox.min.z, vertex.m_Coordinates[2]);

            bbox.max.x = std::max(bbox.max.x, vertex.m_Coordinates[0]);
            bbox.max.y = std::max(bbox.max.y, vertex.m_Coordinates[1]);
            bbox.max.z = std::max(bbox.max.z, vertex.m_Coordinates[2]);
        }
        return bbox;
    }

    void Importer3mf::addMeshObject(Lib3MF::PModel model,
                                    ResourceKey const & key,
                                    Lib3MF::PMeshObject mesh,
                                    nodes::Matrix4x4 const & trafo,
                                    Document & doc)
    {
        nodes::Builder builder;
        bool requiresMesh = true;
        if (mesh)
        {
            auto volume = mesh->GetVolumeData();
            float const units_per_mm = computeUnitsPerMM(model);
            auto coordinateSystemPort = builder.addTransformationToInputCs(
              *doc.getAssembly()->assemblyModel(), trafo, units_per_mm);

            if (requiresMesh)
            {
                builder.addResourceRef(
                  *doc.getAssembly()->assemblyModel(), key, coordinateSystemPort);
            }

            if (volume)
            {
                addVolumeData(volume, model, doc, builder, coordinateSystemPort);
            }
        }
    }

    void Importer3mf::addBeamLatticeObject(Lib3MF::PModel model,
                                           ResourceKey const & key,
                                           Lib3MF::PMeshObject meshObject,
                                           nodes::Matrix4x4 const & trafo,
                                           Document & doc)
    {
        // BEAM_LATTICE_VERIFICATION_MARKER_2025_09_05
        nodes::Builder builder;

        if (meshObject)
        {
            // Check if mesh object has a beam lattice (same pattern as in
            // loadBeamLatticeIfNecessary)
            Lib3MF::PBeamLattice beamLattice = meshObject->BeamLattice();
            if (beamLattice)
            {
                float const units_per_mm = computeUnitsPerMM(model);
                auto coordinateSystemPort = builder.addTransformationToInputCs(
                  *doc.getAssembly()->assemblyModel(), trafo, units_per_mm);

                // Read clipping information from beam lattice
                Lib3MF::eBeamLatticeClipMode clippingMode =
                  Lib3MF::eBeamLatticeClipMode::NoClipMode;
                Lib3MF_uint32 clippingMeshResourceId = 0;

                try
                {
                    beamLattice->GetClipping(clippingMode, clippingMeshResourceId);
                }
                catch (const std::exception & e)
                {
                    // If clipping info is not available, continue with no clipping
                    clippingMode = Lib3MF::eBeamLatticeClipMode::NoClipMode;
                    if (m_eventLogger)
                    {
                        m_eventLogger->addEvent({fmt::format("Warning: Could not read clipping "
                                                             "information from beam lattice {}: {}",
                                                             meshObject->GetModelResourceID(),
                                                             e.what()),
                                                 gladius::events::Severity::Warning});
                    }
                }

                // Handle clipping based on the mode
                if (clippingMode == Lib3MF::eBeamLatticeClipMode::NoClipMode)
                {
                    // No clipping - use the current behavior
                    builder.addBeamLatticeRef(
                      *doc.getAssembly()->assemblyModel(), key, coordinateSystemPort);
                }
                else
                {
                    // Clipping required - get clipping mesh resource key
                    auto clippingMeshKey = ResourceKey(clippingMeshResourceId, ResourceType::Mesh);

                    // Check if clipping mesh resource exists
                    if (!doc.getGeneratorContext().resourceManager.hasResource(clippingMeshKey))
                    {
                        if (m_eventLogger)
                        {
                            m_eventLogger->addEvent(
                              {fmt::format(
                                 "Error: Clipping mesh resource {} not found for beam lattice {}",
                                 clippingMeshResourceId,
                                 meshObject->GetModelResourceID()),
                               gladius::events::Severity::Error});
                        }
                        // Fallback to no clipping
                        builder.addBeamLatticeRef(
                          *doc.getAssembly()->assemblyModel(), key, coordinateSystemPort);
                    }
                    else
                    {
                        // Apply clipping
                        builder.addBeamLatticeWithClipping(*doc.getAssembly()->assemblyModel(),
                                                           key,
                                                           clippingMeshKey,
                                                           static_cast<int>(clippingMode),
                                                           coordinateSystemPort);
                    }
                }
            }
        }
    }

    void Importer3mf::addVolumeData(Lib3MF::PVolumeData & volume,
                                    Lib3MF::PModel & model,
                                    gladius::Document & doc,
                                    gladius::nodes::Builder & builder,
                                    gladius::nodes::Port & coordinateSystemPort)
    {
        auto color = volume->GetColor();
        if (color)
        {
            auto funcId = color->GetFunctionResourceID();
            auto res = model->GetResourceByID(funcId);
            if (!res)
            {
                if (m_eventLogger)
                {
                    m_eventLogger->addEvent(
                      {fmt::format("Could not find color function with unique id {} "
                                   "to resolve the model id",
                                   funcId),
                       events::Severity::Error});
                }
                return;
            }
            auto modelFuncId = res->GetModelResourceID();

            auto colorFunction = doc.getAssembly()->findModel(modelFuncId);
            if (!colorFunction)
            {
                if (m_eventLogger)
                {
                    m_eventLogger->addEvent(
                      {fmt::format("Could not find color function with id {}", modelFuncId),
                       events::Severity::Error});
                }
                return;
            }

            auto transform = matrix4x4From3mfTransform(color->GetTransform());
            builder.appendFunctionForColorOutput(
              *doc.getAssembly()->assemblyModel(), *colorFunction, coordinateSystemPort, transform);

            doc.getAssembly()->updateInputsAndOutputs();
        }
    }

    nodes::Port & Importer3mf::createMeshSdfNode(ResourceKey const & key,
                                                 nodes::Port & coordinateSystemPort,
                                                 nodes::Model & target)
    {
        auto resourceNode = target.create<nodes::Resource>();
        resourceNode->parameter().at(nodes::FieldNames::ResourceId) =
          nodes::VariantParameter(key.getResourceId().value_or(0));

        auto meshSdfNode = target.create<nodes::SignedDistanceToMesh>();
        meshSdfNode->parameter().at(nodes::FieldNames::Pos).setInputFromPort(coordinateSystemPort);
        meshSdfNode->parameter()
          .at(nodes::FieldNames::Mesh)
          .setInputFromPort(resourceNode->getOutputValue());

        return meshSdfNode->getOutputs().at(nodes::FieldNames::Distance);
    }

    nodes::Port & Importer3mf::combineSdf(nodes::Model & target,
                                          nodes::Port & lhs,
                                          nodes::Port & rhs,
                                          Lib3MF::eBooleanOperation operation)
    {
        switch (operation)
        {
        case Lib3MF::eBooleanOperation::Union:
        {
            auto unionNode = target.create<nodes::Min>();
            unionNode->setInputA(lhs);
            unionNode->setInputB(rhs);
            return unionNode->getResultOutputPort();
        }
        case Lib3MF::eBooleanOperation::Intersection:
        {
            auto intersectionNode = target.create<nodes::Max>();
            intersectionNode->setInputA(lhs);
            intersectionNode->setInputB(rhs);
            return intersectionNode->getResultOutputPort();
        }
        case Lib3MF::eBooleanOperation::Difference:
        {
            auto negateNode = target.create<nodes::Multiplication>();
            auto & minusOne =
              nodes::Builder::ensureConstantScalar(target, "BooleanDifferenceNegate", -1.0f);
            negateNode->setInputA(rhs);
            negateNode->parameter().at(nodes::FieldNames::B).setInputFromPort(minusOne);

            auto differenceNode = target.create<nodes::Max>();
            differenceNode->setInputA(lhs);
            differenceNode->setInputB(negateNode->getResultOutputPort());
            return differenceNode->getResultOutputPort();
        }
        default:
            throw std::runtime_error("Unsupported 3MF boolean operation");
        }
    }

    nodes::Port & Importer3mf::createLevelSetSdfNode(Lib3MF::PModel const & model,
                                                     Lib3MF::PLevelSet const & levelSet,
                                                     nodes::Port & coordinateSystemPort,
                                                     nodes::Model & target,
                                                     Document & doc)
    {
        if (!levelSet)
        {
            throw std::runtime_error("Cannot build SDF for null level set base object");
        }

        auto function = levelSet->GetFunction();
        if (!function)
        {
            throw std::runtime_error("No function found for boolean level set base object");
        }

        auto const functionResourceId = function->GetResourceID();
        auto const functionResource = model->GetResourceByID(functionResourceId);
        if (!functionResource)
        {
            throw std::runtime_error(
              fmt::format("Could not resolve level set function resource {}", functionResourceId));
        }

        auto const modelFunctionId = functionResource->GetModelResourceID();
        auto const gladiusFunction = doc.getAssembly()->findModel(modelFunctionId);
        if (!gladiusFunction)
        {
            throw std::runtime_error(
              fmt::format("Could not find imported level set function {}", modelFunctionId));
        }

        auto mesh = levelSet->GetMesh();
        if (!mesh)
        {
            throw std::runtime_error("No mesh domain found for boolean level set base object");
        }

        auto channelName = levelSet->GetChannelName();
        if (channelName.empty())
        {
            channelName = nodes::FieldNames::Shape;
        }

        nodes::Builder builder;
        auto const levelSetTransform = matrix4x4From3mfTransform(levelSet->GetTransform());
        auto & functionCoordinateSystemPort =
          builder.insertTransformation(target, coordinateSystemPort, levelSetTransform, 1.0f);

        auto const boundingBox = computeBoundingBox(mesh);
        auto boundingBoxNode = target.create<nodes::BoxMinMax>();
        boundingBoxNode->parameter()
          .at(nodes::FieldNames::Pos)
          .setInputFromPort(coordinateSystemPort);

        auto minVectorNode = target.create<nodes::ConstantVector>();
        minVectorNode->parameter().at(nodes::FieldNames::X) =
          nodes::VariantParameter(boundingBox.min.x);
        minVectorNode->parameter().at(nodes::FieldNames::Y) =
          nodes::VariantParameter(boundingBox.min.y);
        minVectorNode->parameter().at(nodes::FieldNames::Z) =
          nodes::VariantParameter(boundingBox.min.z);

        auto maxVectorNode = target.create<nodes::ConstantVector>();
        maxVectorNode->parameter().at(nodes::FieldNames::X) =
          nodes::VariantParameter(boundingBox.max.x);
        maxVectorNode->parameter().at(nodes::FieldNames::Y) =
          nodes::VariantParameter(boundingBox.max.y);
        maxVectorNode->parameter().at(nodes::FieldNames::Z) =
          nodes::VariantParameter(boundingBox.max.z);

        boundingBoxNode->parameter()
          .at(nodes::FieldNames::Min)
          .setInputFromPort(minVectorNode->getVectorOutputPort());
        boundingBoxNode->parameter()
          .at(nodes::FieldNames::Max)
          .setInputFromPort(maxVectorNode->getVectorOutputPort());

        auto resourceNode = target.create<nodes::Resource>();
        resourceNode->parameter().at(nodes::FieldNames::ResourceId) =
          nodes::VariantParameter(gladiusFunction->getResourceId());

        auto functionCallNode = target.create<nodes::FunctionCall>();
        functionCallNode->parameter()
          .at(nodes::FieldNames::FunctionId)
          .setInputFromPort(resourceNode->getOutputValue());
        functionCallNode->updateInputsAndOutputs(*gladiusFunction);
        target.registerInputs(*functionCallNode);
        target.registerOutputs(*functionCallNode);

        auto positionInput = functionCallNode->parameter().find(nodes::FieldNames::Pos);
        if (positionInput == functionCallNode->parameter().end())
        {
            throw std::runtime_error("Level set function has no pos input");
        }
        positionInput->second.setInputFromPort(functionCoordinateSystemPort);

        auto shapeOutput = functionCallNode->getOutputs().find(channelName);
        if (shapeOutput == functionCallNode->getOutputs().end())
        {
            throw std::runtime_error(
              fmt::format("Level set function has no output named {}", channelName));
        }
        if (shapeOutput->second.getTypeIndex() != nodes::ParameterTypeIndex::Float)
        {
            throw std::runtime_error(
              fmt::format("Level set output {} is not scalar", channelName));
        }

        auto intersectionNode = target.create<nodes::Max>();
        intersectionNode->setInputA(boundingBoxNode->getOutputs().at(nodes::FieldNames::Shape));
        intersectionNode->setInputB(shapeOutput->second);
        return intersectionNode->getResultOutputPort();
    }

    nodes::Port & Importer3mf::buildObjectSdf(Lib3MF::PModel const & model,
                                              Lib3MF::PObject const & object,
                                              nodes::Port & coordinateSystemPort,
                                              nodes::Model & target,
                                              Document & doc)
    {
        if (!object)
        {
            throw std::runtime_error("Cannot build SDF for null 3MF object");
        }

        if (object->IsMeshObject())
        {
            auto key = ResourceKey{static_cast<uint32_t>(object->GetModelResourceID()),
                                   ResourceType::Mesh};
            key.setDisplayName(object->GetName());
            return createMeshSdfNode(key, coordinateSystemPort, target);
        }

        if (object->IsBooleanObject())
        {
            auto const booleanObject = model->GetBooleanObjectByID(object->GetUniqueResourceID());
            return buildBooleanSdf(model, booleanObject, coordinateSystemPort, target, doc);
        }

        if (object->IsLevelSetObject())
        {
            auto const levelSet = model->GetLevelSetByID(object->GetUniqueResourceID());
            return createLevelSetSdfNode(model, levelSet, coordinateSystemPort, target, doc);
        }

        auto const message = fmt::format("Unsupported base object type in boolean resource {}",
                                         object->GetModelResourceID());
        if (m_eventLogger)
        {
            m_eventLogger->addEvent({message, events::Severity::Error});
        }
        throw std::runtime_error(message);
    }

    nodes::Port & Importer3mf::buildBooleanSdf(Lib3MF::PModel const & model,
                                               Lib3MF::PBooleanObject const & booleanObject,
                                               nodes::Port & coordinateSystemPort,
                                               nodes::Model & target,
                                               Document & doc)
    {
        if (!booleanObject)
        {
            throw std::runtime_error("Cannot build SDF for null boolean object");
        }

        nodes::Builder builder;
        auto const baseTransform =
          inverseMatrix(matrix4x4From3mfTransform(booleanObject->GetBaseTransform()));
        auto & baseCoordinateSystemPort =
          builder.insertTransformation(target, coordinateSystemPort, baseTransform, 1.0f);

        auto baseObject = booleanObject->GetBaseObject();
        auto * currentSdf =
          &buildObjectSdf(model, baseObject, baseCoordinateSystemPort, target, doc);

        auto const operandCount = booleanObject->GetOperandCount();
        if (operandCount == 0u && m_eventLogger)
        {
            m_eventLogger->addEvent(
              {fmt::format("Boolean resource {} has no operands; using base object only",
                           booleanObject->GetModelResourceID()),
               events::Severity::Warning});
        }

        for (Lib3MF_uint32 operandIndex = 0; operandIndex < operandCount; ++operandIndex)
        {
            Lib3MF::PMeshObject operandMesh;
            auto const operandTransform =
              inverseMatrix(matrix4x4From3mfTransform(booleanObject->GetOperand(operandIndex,
                                                                                 operandMesh)));
            if (!operandMesh)
            {
                throw std::runtime_error(
                  fmt::format("Boolean resource {} has null operand {}",
                              booleanObject->GetModelResourceID(),
                              operandIndex));
            }

            auto & operandCoordinateSystemPort =
              builder.insertTransformation(target, coordinateSystemPort, operandTransform, 1.0f);
            auto operandKey = ResourceKey{static_cast<uint32_t>(operandMesh->GetModelResourceID()),
                                          ResourceType::Mesh};
            operandKey.setDisplayName(operandMesh->GetName());
            auto & operandSdf = createMeshSdfNode(operandKey, operandCoordinateSystemPort, target);
            currentSdf =
              &combineSdf(target, *currentSdf, operandSdf, booleanObject->GetOperation());
        }

        return *currentSdf;
    }

    void Importer3mf::addBooleanObject(Lib3MF::PModel model,
                                       Lib3MF::PBooleanObject booleanObject,
                                       nodes::Matrix4x4 const & trafo,
                                       Document & doc)
    {
        if (!booleanObject)
        {
            if (m_eventLogger)
            {
                m_eventLogger->addEvent({"No boolean object to import", events::Severity::Error});
            }
            return;
        }

        nodes::Builder builder;
        auto & target = *doc.getAssembly()->assemblyModel();
        float const units_per_mm = computeUnitsPerMM(model);
        auto & coordinateSystemPort = builder.addTransformationToInputCs(target, trafo, units_per_mm);
        auto & booleanSdf = buildBooleanSdf(model, booleanObject, coordinateSystemPort, target, doc);

        auto * shapeSink = target.getEndNode()->getParameter(nodes::FieldNames::Shape);
        if (!shapeSink)
        {
            throw std::runtime_error("End node is required to have a shape parameter");
        }

        auto * lastShapePort = builder.getLastShape(target);
        if (!lastShapePort)
        {
            shapeSink->setInputFromPort(booleanSdf);
            return;
        }

        auto unionNode = target.create<nodes::Min>();
        unionNode->setInputA(*lastShapePort);
        unionNode->setInputB(booleanSdf);
        shapeSink->setInputFromPort(unionNode->getResultOutputPort());
    }

    void Importer3mf::addLevelSetObject(Lib3MF::PModel model,
                                        ResourceKey const & key,
                                        Lib3MF::PLevelSet levelSet,
                                        nodes::Matrix4x4 const & trafo,
                                        Document & doc)
    {
        nodes::Builder builder;
        if (levelSet)
        {
            auto function = levelSet->GetFunction();
            if (!function)
            {
                if (m_eventLogger)
                {
                    m_eventLogger->addEvent(
                      {"No function found for level set", events::Severity::Error});
                }
                return;
            }

            auto funcId = levelSet->GetFunction()->GetResourceID();
            auto res = model->GetResourceByID(funcId);
            if (!res)
            {
                if (m_eventLogger)
                {
                    m_eventLogger->addEvent({fmt::format("Could not find function with model id {} "
                                                         "to resolve the model id",
                                                         funcId),
                                             events::Severity::Error});
                }
                return;
            }
            auto modelFuncId = res->GetModelResourceID();

            auto gladiusFunction = doc.getAssembly()->findModel(modelFuncId);
            if (!gladiusFunction)
            {
                if (m_eventLogger)
                {
                    m_eventLogger->addEvent(
                      {fmt::format("Could not find boundary function with id {}", modelFuncId),
                       events::Severity::Error});
                }
                return;
            }
            auto channelName = levelSet->GetChannelName();
            if (channelName.empty())
            {
                channelName = "shape";
            }

            float const units_per_mm = computeUnitsPerMM(model);
            auto buildItemCoordinateSystemPort = builder.addTransformationToInputCs(
              *doc.getAssembly()->assemblyModel(), trafo, units_per_mm);

            auto levelSetTransform = levelSet->GetTransform();
            auto levelSetTrafo = matrix4x4From3mfTransform(levelSetTransform);

            auto levelSetCoordinateSystemPort =
              builder.insertTransformation(*doc.getAssembly()->assemblyModel(),
                                           buildItemCoordinateSystemPort,
                                           levelSetTrafo,
                                           1.0f);

            auto mesh = levelSet->GetMesh();
            if (!mesh)
            {
                if (m_eventLogger)
                {
                    m_eventLogger->addEvent(
                      {"No mesh found for level set", events::Severity::Error});
                }
                return;
            }

            // Calculate the bounding box for this level set
            BoundingBox bbox;
            if (levelSet->GetMeshBBoxOnly())
            {
                bbox = computeBoundingBox(mesh);
            }
            else
            {

                bbox = computeBoundingBox(mesh);
                // Also load the mesh reference if needed
                auto referencedMeshKey =
                  ResourceKey(mesh->GetModelResourceID(), ResourceType::Mesh);
                loadMeshIfNecessary(model, mesh, doc);
            }

            // Use the new method that creates a complete level set operation:
            // (function ∩ bounding_box) and unions it with existing level sets
            builder.addLevelSetWithDomain(*doc.getAssembly()->assemblyModel(),
                                          *gladiusFunction,
                                          levelSetCoordinateSystemPort,
                                          channelName,
                                          bbox,
                                          buildItemCoordinateSystemPort);

            doc.getAssembly()->setFallbackValueLevelSet((levelSet->GetFallBackValue()));

            auto volumeData = levelSet->GetVolumeData();

            if (volumeData)
            {
                addVolumeData(volumeData, model, doc, builder, levelSetCoordinateSystemPort);
            }

            doc.getAssembly()->updateInputsAndOutputs();
        }
    }

    void Importer3mf::loadImageStacks(std::filesystem::path const & filename,
                                      Lib3MF::PModel model,
                                      Document & doc)
    {
        ProfileFunction auto image3dIterator = model->GetImage3Ds();

        ImageExtractor extractor;
        if (!extractor.loadFromArchive(filename))
        {
            throw std::runtime_error(fmt::format("Could not open file {}", filename.string()));
        }

        try
        {
            while (image3dIterator->MoveNext())
            {
                auto image3d = image3dIterator->GetCurrentImage3D();
                auto & resMan = doc.getGeneratorContext().resourceManager;

                if (image3d->IsImageStack())
                {
                    auto imageStack3mf = model->GetImageStackByID(image3d->GetUniqueResourceID());
                    FileList fileList;
                    for (auto index = 0u; index < imageStack3mf->GetSheetCount(); ++index)
                    {
                        auto sheet = imageStack3mf->GetSheet(index);
                        if (!sheet)
                        {
                            if (m_eventLogger)
                            {
                                m_eventLogger->addEvent(
                                  {"Sheet happens: Sheet not found", events::Severity::Error});
                            }
                            continue;
                        }
                        fileList.push_back(sheet->GetPath());
                    }

                    bool const useVdb = extractor.determinePixelFormat(fileList.front()) ==
                                        PixelFormat::GRAYSCALE_8BIT;

                    ResourceType resourceType =
                      useVdb ? ResourceType::Vdb : ResourceType::ImageStack;
                    auto key = ResourceKey{image3d->GetModelResourceID(), resourceType};

                    // Check if resource already exists
                    if (resMan.hasResource(key))
                    {
                        continue;
                    }

                    key.setDisplayName(image3d->GetName());
                    if (m_eventLogger)
                    {
                        m_eventLogger->addEvent(
                          {fmt::format("Creating Image3D resource key id={}, type={}, name=\"{}\"",
                                       image3d->GetModelResourceID(),
                                       static_cast<int>(resourceType),
                                       image3d->GetName()),
                           events::Severity::Info});
                    }
                    if (useVdb)
                    {
                        auto grid = extractor.loadAsVdbGrid(fileList, FileLoaderType::Archive);
                        resMan.addResource(key, std::move(grid));
                        if (m_eventLogger)
                        {
                            m_eventLogger->addEvent(
                              {fmt::format("Added VDB resource id={} (sheets: {})",
                                           image3d->GetModelResourceID(),
                                           imageStack3mf->GetSheetCount()),
                               events::Severity::Info});
                        }
                    }
                    else
                    {

                        auto imageStack = extractor.loadImageStack(fileList);
                        imageStack.setResourceId(image3d->GetModelResourceID());

                        resMan.addResource(key, std::move(imageStack));
                        if (m_eventLogger)
                        {
                            m_eventLogger->addEvent(
                              {fmt::format("Added ImageStack resource id={} (sheets: {})",
                                           image3d->GetModelResourceID(),
                                           imageStack3mf->GetSheetCount()),
                               events::Severity::Info});
                        }
                    }
                }
            }
        }
        catch (const std::exception & e)
        {
            if (m_eventLogger)
            {
                m_eventLogger->addEvent(
                  {fmt::format("Error while loading image stack: {}", e.what()),
                   events::Severity::Error});
            }
            else
            {
                std::cerr << "Error while loading image stack: " << e.what() << std::endl;
            }
        }
    }

    void Importer3mf::loadBuildItems(Lib3MF::PModel model, Document & doc)
    {
        ProfileFunction;

        doc.getAssembly()->assemblyModel()->setManaged(true);
        auto buildItemIterator = model->GetBuildItems();
        while (buildItemIterator->MoveNext())
        {
            auto currentBuildItem = buildItemIterator->GetCurrent();
            nodes::Matrix4x4 const transformation =
              matrix4x4From3mfTransform(currentBuildItem->GetObjectTransform());
            nodes::Matrix4x4 const trafo = inverseMatrix(transformation);

            auto buildItemIter = doc.addBuildItem({currentBuildItem->GetObjectResourceID(),
                                                   transformation,
                                                   currentBuildItem->GetPartNumber()});

            auto objectRes = currentBuildItem->GetObjectResource();

            if (!objectRes)
            {
                if (m_eventLogger)
                {
                    m_eventLogger->addEvent(
                      {"No object resource for build item", events::Severity::Error});
                }
                continue;
            }

            if (objectRes->IsComponentsObject())
            {
                auto compObjs = model->GetComponentsObjectByID(objectRes->GetUniqueResourceID());
                if (!compObjs)
                {
                    if (m_eventLogger)
                    {
                        m_eventLogger->addEvent(
                          {"No components object for build item", events::Severity::Error});
                    }
                    continue;
                }

                // loop over components
                for (Lib3MF_uint32 i = 0; i < compObjs->GetComponentCount(); ++i)
                {
                    auto component = compObjs->GetComponent(i);

                    auto componentObj = component->GetObjectResource();
                    if (!componentObj)
                    {
                        if (m_eventLogger)
                        {
                            m_eventLogger->addEvent(
                              {"No components object for component", events::Severity::Error});
                        }
                        continue;
                    }

                    nodes::Matrix4x4 componentTrafo = identityMatrix();

                    if (component->HasTransform())
                    {
                        auto const transformationComponent =
                          matrix4x4From3mfTransform(component->GetTransform());
                        componentTrafo = inverseMatrix(transformationComponent);
                    }

                    buildItemIter->addComponent(
                      {componentObj->GetModelResourceID(), componentTrafo});
                    // Create a typed ResourceKey for components
                    ResourceKey key{componentObj->GetModelResourceID()};
                    key.setDisplayName(componentObj->GetName());

                    // Determine the correct resource type for mesh components (Mesh vs BeamLattice)
                    if (componentObj->IsMeshObject())
                    {
                        auto mesh = model->GetMeshObjectByID(componentObj->GetUniqueResourceID());
                        if (mesh && mesh->BeamLattice())
                        {
                            // Beam lattice component
                            key =
                              ResourceKey{static_cast<uint32_t>(componentObj->GetModelResourceID()),
                                          ResourceType::BeamLattice};
                            key.setDisplayName(componentObj->GetName() + "_BeamLattice");
                        }
                        else
                        {
                            // Regular mesh component
                            key =
                              ResourceKey{static_cast<uint32_t>(componentObj->GetModelResourceID()),
                                          ResourceType::Mesh};
                            key.setDisplayName(componentObj->GetName());
                        }
                    }

                    createObject(*componentObj, model, key, componentTrafo, doc);
                }
            }
            else
            {
                auto key = ResourceKey{objectRes->GetModelResourceID(), ResourceType::Mesh};
                key.setDisplayName(currentBuildItem->GetObjectResource()->GetName());

                // Check if this object has a beam lattice to determine the correct resource type
                if (objectRes->IsMeshObject())
                {
                    auto mesh = model->GetMeshObjectByID(objectRes->GetUniqueResourceID());
                    if (mesh && mesh->BeamLattice())
                    {
                        // This is a beam lattice object - create key with BeamLattice type
                        key = ResourceKey{static_cast<uint32_t>(objectRes->GetModelResourceID()),
                                          ResourceType::BeamLattice};
                        key.setDisplayName(currentBuildItem->GetObjectResource()->GetName() +
                                           "_BeamLattice");
                    }
                }

                createObject(*objectRes, model, key, trafo, doc);
            }
        }

        // Normalize distances to mm via Builder helper
        nodes::Builder::applyDistanceNormalization(*doc.getAssembly()->assemblyModel(),
                                                   computeUnitsPerMM(model));
    }

    void Importer3mf::createObject(Lib3MF::CObject & objectRes,
                                   Lib3MF::PModel & model,
                                   gladius::ResourceKey & key,
                                   const gladius::nodes::Matrix4x4 & trafo,
                                   gladius::Document & doc)
    {
        if (objectRes.IsMeshObject())
        {
            auto mesh = model->GetMeshObjectByID(objectRes.GetUniqueResourceID());

            // Check for beam lattice first (using same pattern as loadBeamLatticeIfNecessary)
            if (mesh)
            {
                if (mesh->GetTriangleCount() > 0)
                {
                    addMeshObject(model, key, mesh, trafo, doc);
                }
                Lib3MF::PBeamLattice beamLattice = mesh->BeamLattice();
                if (beamLattice)
                {
                    addBeamLatticeObject(model, key, mesh, trafo, doc);
                }
            }
        }
        else if (objectRes.IsLevelSetObject())
        {
            auto levelSet = model->GetLevelSetByID(objectRes.GetUniqueResourceID());
            addLevelSetObject(model, key, levelSet, trafo, doc);
        }
        else if (objectRes.IsBooleanObject())
        {
            auto booleanObject = model->GetBooleanObjectByID(objectRes.GetUniqueResourceID());
            addBooleanObject(model, booleanObject, trafo, doc);
        }
    }

    void Importer3mf::load(std::filesystem::path const & filename, Document & doc)
    {
        ProfileFunction

          doc.newEmptyModel();

        auto const model = m_wrapper->CreateModel();
        auto const reader = model->QueryReader("3mf");
        doc.set3mfModel(model);

        reader->SetStrictModeActive(false);
        try
        {
            reader->ReadFromFile(filename.string());
        }
        catch (Lib3MF::ELib3MFException const & e)
        {
            if (m_eventLogger)
            {
                m_eventLogger->addEvent({fmt::format("Error #{} while reading 3mf file {}: {}",
                                                     filename.string(),
                                                     e.getErrorCode(),
                                                     e.what()),
                                         events::Severity::Error});
            }
        }

        logWarnings(filename, reader);

        loadImageStacks(filename, model, doc);
        loadImplicitFunctions(model, doc);
        loadMeshes(model, doc);
        // loadComponentObjects(model, doc);
        loadBuildItems(model, doc);
    }

    /// Remove attachments from sourceModel whose paths already exist in targetModel.
    /// This prevents "Duplicate Attachment Path" errors from MergeFromModel when
    /// the same library file is imported more than once.
    static void removeDuplicateAttachments(Lib3MF::PModel const & sourceModel,
                                           Lib3MF::PModel const & targetModel)
    {
        // Collect existing attachment paths from the target model
        std::set<std::string> targetPaths;
        auto const targetCount = targetModel->GetAttachmentCount();
        for (Lib3MF_uint32 i = 0; i < targetCount; ++i)
        {
            targetPaths.insert(targetModel->GetAttachment(i)->GetPath());
        }

        // Remove source attachments whose path already exists in the target.
        // Iterate in reverse because RemoveAttachment can shift indices.
        auto sourceCount = sourceModel->GetAttachmentCount();
        for (auto i = static_cast<int64_t>(sourceCount) - 1; i >= 0; --i)
        {
            auto attachment = sourceModel->GetAttachment(static_cast<Lib3MF_uint32>(i));
            if (targetPaths.count(attachment->GetPath()) > 0)
            {
                sourceModel->RemoveAttachment(attachment.get());
            }
        }

        // Also remove the source's package thumbnail if the target already has one
        if (sourceModel->HasPackageThumbnailAttachment() &&
            targetModel->HasPackageThumbnailAttachment())
        {
            sourceModel->RemovePackageThumbnailAttachment();
        }
    }

    void Importer3mf::merge(std::filesystem::path const & filename, Document & doc)
    {
        ProfileFunction auto targetModel = doc.get3mfModel();
        if (!targetModel)
        {
            load(filename, doc);
            return;
        }

        auto const modelToMergeFrom = m_wrapper->CreateModel();
        auto const reader = modelToMergeFrom->QueryReader("3mf");

        reader->SetStrictModeActive(false);
        try
        {
            reader->ReadFromFile(filename.string());
        }
        catch (Lib3MF::ELib3MFException const & e)
        {
            if (m_eventLogger)
            {
                m_eventLogger->addEvent({fmt::format("Error #{} while reading 3mf file {}: {}",
                                                     filename.string(),
                                                     e.getErrorCode(),
                                                     e.what()),
                                         events::Severity::Error});
            }
            logWarnings(filename, reader);
            return;
        }

        logWarnings(filename, reader);
        merge(modelToMergeFrom, filename, doc);
    }

    void Importer3mf::merge(Lib3MF::PModel const & modelToMergeFrom,
                             std::filesystem::path const & sourceFilename,
                             Document & doc)
    {
        ProfileFunction auto targetModel = doc.get3mfModel();
        if (!targetModel)
        {
            return;
        }

        auto core = doc.getCore();
        auto computeToken = core->waitForComputeToken();

        try
        {
            // backup the list of function ids
            std::set<Lib3MF_uint32> functionResourceIds = collectFunctionResourceIds(targetModel);
            // store the ptr to the original functions
            auto implicitFunctions = collectImplicitFunctions(targetModel);

            if (m_eventLogger)
            {
                m_eventLogger->addEvent(
                  {fmt::format("Merge: {} existing functions, {} existing implicit functions",
                               functionResourceIds.size(),
                               implicitFunctions.size()),
                   events::Severity::Info});
            }

            removeDuplicateAttachments(modelToMergeFrom, targetModel);
            targetModel->MergeFromModel(modelToMergeFrom.get());

            if (m_eventLogger)
            {
                m_eventLogger->addEvent(
                  {fmt::format("Merge: MergeFromModel succeeded for {}",
                               sourceFilename.string()),
                   events::Severity::Info});
            }

            // now find all duplicated functions
            size_t numDuplicatesPrevious = 0;
            size_t numDuplicatesCurrent = 0;
            std::vector<Duplicates> duplicates;
            do
            {
                numDuplicatesPrevious = numDuplicatesCurrent;

                duplicates = findDuplicatedFunctions(implicitFunctions, targetModel);
                numDuplicatesCurrent = duplicates.size();

                // replace the references to the duplicated functions with the original ones
                replaceDuplicatedFunctionReferences(duplicates, targetModel);

            } while (numDuplicatesPrevious != numDuplicatesCurrent);

            // NOTE: We intentionally do NOT call RemoveResource on duplicate functions
            // in the live target model. lib3mf's RemoveResource corrupts internal state
            // on models with cross-function ResourceIdNode references. The duplicates
            // are harmless: references have been redirected by
            // replaceDuplicatedFunctionReferences, and loadImplicitFunctionsFiltered
            // skips loading them into the Document.

            if (m_eventLogger)
            {
                m_eventLogger->addEvent(
                  {fmt::format("Merge: {} duplicates found, loading functions...",
                               duplicates.size()),
                   events::Severity::Info});
            }

            loadImageStacks(sourceFilename, targetModel, doc);
            loadImplicitFunctionsFiltered(targetModel, doc, duplicates);

            if (m_eventLogger)
            {
                auto const funcCount = doc.getAssembly()->getFunctions().size();
                m_eventLogger->addEvent(
                  {fmt::format("Merge: complete, assembly now has {} functions", funcCount),
                   events::Severity::Info});
            }

            doc.rebuildResourceDependencyGraph();
        }
        catch (Lib3MF::ELib3MFException const & e)
        {
            if (m_eventLogger)
            {
                m_eventLogger->addEvent(
                  {fmt::format("Error #{} while merging 3mf file {}: {}",
                               e.getErrorCode(),
                               sourceFilename.string(),
                               e.what()),
                   events::Severity::Error});
            }
        }
        catch (std::exception const & e)
        {
            if (m_eventLogger)
            {
                m_eventLogger->addEvent(
                  {fmt::format("Error while merging 3mf file {}: {}",
                               sourceFilename.string(),
                               e.what()),
                   events::Severity::Error});
            }
        }
    }

    Lib3MF::PWrapper Importer3mf::get3mfWrapper() const
    {
        return m_wrapper;
    }

    void loadFrom3mfFile(std::filesystem::path const filename, Document & doc)
    {
        ProfileFunction Importer3mf importer{doc.getSharedLogger()};
        importer.setMeshRepairConfig(doc.getMeshRepairConfig());
        importer.setMeshSdfEvaluationConfig(doc.getMeshSdfEvaluationConfig());
        importer.setNanoVdbBuildPolicy(doc.getNanoVdbBuildPolicy());
        importer.load(filename, doc);
    }

    void mergeFrom3mfFile(std::filesystem::path filename, Document & doc)
    {
        ProfileFunction Importer3mf importer{doc.getSharedLogger()};
        importer.setMeshRepairConfig(doc.getMeshRepairConfig());
        importer.setMeshSdfEvaluationConfig(doc.getMeshSdfEvaluationConfig());
        importer.setNanoVdbBuildPolicy(doc.getNanoVdbBuildPolicy());
        importer.merge(filename, doc);
    }

    void mergeModelInto3mfDoc(Lib3MF::PModel const & sourceModel,
                              std::filesystem::path const & sourceFilename,
                              Document & doc)
    {
        ProfileFunction Importer3mf importer{doc.getSharedLogger()};
        importer.setMeshRepairConfig(doc.getMeshRepairConfig());
        importer.setMeshSdfEvaluationConfig(doc.getMeshSdfEvaluationConfig());
        importer.setNanoVdbBuildPolicy(doc.getNanoVdbBuildPolicy());
        importer.merge(sourceModel, sourceFilename, doc);
    }
} // namespace gladius::io