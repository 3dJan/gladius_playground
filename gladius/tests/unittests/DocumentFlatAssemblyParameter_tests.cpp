#include "Document.h"
#include "EventLogger.h"
#include "nodes/DerivedNodes.h"
#include "nodes/ParameterOrigin.h"

#include <gtest/gtest.h>

#include <memory>
#include <utility>

namespace gladius_tests
{
    using namespace gladius;

    TEST(DocumentFlatAssemblyParameter,
         NumericValueUpdate_PatchesFlattenedCopyWithoutRebuildingGraph)
    {
        events::SharedLogger logger = std::make_shared<events::Logger>();
        gladius::Document document{std::move(logger)};
        document.newModel();

        auto assembly = document.getAssembly();
        ASSERT_NE(assembly, nullptr);
        auto sourceModel = assembly->assemblyModel();
        ASSERT_NE(sourceModel, nullptr);

        nodes::VariantParameter * sourceParameter{};
        for (auto & [nodeId, node] : *sourceModel)
        {
            (void) nodeId;
            auto * sourceConstant = dynamic_cast<nodes::ConstantScalar *>(node.get());
            if (sourceConstant != nullptr)
            {
                sourceParameter = &sourceConstant->parameter().at(nodes::FieldNames::Value);
                break;
            }
        }
        ASSERT_NE(sourceParameter, nullptr);
                auto const origin = gladius::nodes::ParameterOrigin{
                    assembly->getAssemblyModelId(), sourceParameter->getId()};

        auto flatAssembly = document.getFlatAssembly();
        ASSERT_NE(flatAssembly, nullptr);
        auto flatModel = flatAssembly->assemblyModel();
        ASSERT_NE(flatModel, nullptr);

        nodes::VariantParameter * flatParameter{};
        for (auto const & [parameterId, parameter] : flatModel->getConstParameterRegistry())
        {
            (void) parameterId;
            if (parameter == nullptr || parameter->getOrigin() != origin ||
                parameter->getConstSource().has_value())
            {
                continue;
            }
            flatParameter = dynamic_cast<nodes::VariantParameter *>(parameter);
            if (flatParameter != nullptr)
            {
                break;
            }
        }
        ASSERT_NE(flatParameter, nullptr);

        auto const graphSize = flatModel->getGraph().getSize();
        auto const outputOrder = flatModel->getOutputOrder();
        sourceParameter->setValue(2.5f);

        document.updateFlatAssemblyParameterValues();

        ASSERT_TRUE(std::holds_alternative<float>(flatParameter->getValue()));
        EXPECT_FLOAT_EQ(std::get<float>(flatParameter->getValue()), 2.5f);
        EXPECT_EQ(flatModel->getGraph().getSize(), graphSize);
        EXPECT_EQ(flatModel->getOutputOrder(), outputOrder);
    }

    TEST(DocumentResourceRemoval, MissingDependencyGraph_IsBuiltBeforeCheckingResource)
    {
        events::SharedLogger logger = std::make_shared<events::Logger>();
        gladius::Document document{std::move(logger)};
        auto model = document.get3mfModel();
        ASSERT_NE(model, nullptr);

        auto function = model->AddImplicitFunction();
        ASSERT_NE(function, nullptr);
        auto const resourceId = function->GetModelResourceID();
        ASSERT_NE(resourceId, 0u);
        ASSERT_EQ(document.getResourceDependencyGraphSnapshot(), nullptr);

        auto const result = document.isItSafeToDeleteResource(ResourceKey(resourceId));

        EXPECT_TRUE(result.canBeRemoved);
        EXPECT_NE(document.getResourceDependencyGraphSnapshot(), nullptr);
    }
}