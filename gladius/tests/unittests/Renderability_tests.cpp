/**
 * @file Renderability_tests.cpp
 * @brief Tests for the renderability probe: the facts that separate "the program compiles" from
 *        "something shows up in the viewport".
 */

#include "Document.h"
#include "EventLogger.h"
#include "Renderability.h"
#include "io/3mf/VolumetricScaffold.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <memory>
#include <string>

namespace gladius::design::tests
{
    namespace
    {
        /**
         * @brief Locates the shipped volumetric template.
         * @details Tests and benchmarks run from the binary directory, where CMake copies
         *          examples/; the source tree is the fallback for an ad hoc run.
         */
        [[nodiscard]] std::filesystem::path findTemplate()
        {
            for (auto const & candidate :
                 {std::filesystem::path{"examples/template.3mf"},
                  std::filesystem::path{"../../../../../examples/template.3mf"}})
            {
                if (std::filesystem::exists(candidate))
                {
                    return candidate;
                }
            }
            return {};
        }

        [[nodiscard]] bool hasBlocker(RenderabilityFacts const & facts, std::string const & code)
        {
            return std::any_of(facts.blockers.begin(),
                               facts.blockers.end(),
                               [&code](RenderabilityBlocker const & blocker)
                               { return blocker.code == code; });
        }

        [[nodiscard]] Document makeDocument()
        {
            return Document{std::make_shared<events::Logger>(events::OutputMode::Silent)};
        }
    } // namespace

    TEST(Renderability, BlankDocument_ReportsMissingLevelSetAndBuildItem)
    {
        // Arrange
        auto document = makeDocument();
        document.newModel();

        // Act
        auto const facts = collectRenderabilityFacts(&document);

        // Assert: the program of a blank document compiles, but nothing of it is ever sampled
        EXPECT_FALSE(facts.isRenderable());
        EXPECT_EQ(facts.buildItemCount, 0);
        EXPECT_EQ(facts.levelSetCount, 0);
        EXPECT_TRUE(hasBlocker(facts, "no_build_items"));
        EXPECT_TRUE(hasBlocker(facts, "no_levelset"));
        EXPECT_NE(facts.blockersAsText().find("ensure_output_chain"), std::string::npos)
          << facts.blockersAsText();
    }

    TEST(Renderability, NoDocument_ReportsBlockerInsteadOfThrowing)
    {
        auto const facts = collectRenderabilityFacts(nullptr);

        EXPECT_FALSE(facts.isRenderable());
        EXPECT_TRUE(hasBlocker(facts, "no_document"));
    }

    TEST(Renderability, VolumetricTemplate_SceneIsRenderable)
    {
        // Arrange
        auto const path = findTemplate();
        if (path.empty())
        {
            GTEST_SKIP() << "Volumetric project template not found";
        }
        auto document = makeDocument();
        document.load(path);

        // Act
        auto const facts = collectRenderabilityFacts(&document);

        // Assert: what File > New gives the user is the reference case for a renderable document
        EXPECT_TRUE(facts.isRenderable()) << facts.blockersAsText();
        EXPECT_GE(facts.buildItemCount, 1);
        EXPECT_GE(facts.levelSetCount, 1);
        EXPECT_NE(facts.rootFunctionId, 0);
        EXPECT_TRUE(facts.rootFunctionHasShapeOutput);
        EXPECT_TRUE(facts.rootShapeConsumed);
    }

    TEST(Renderability, FunctionWithoutOutputChain_LevelSetAndBuildItemMakeItRenderable)
    {
        // Arrange
        auto document = makeDocument();
        document.newModel();
        ASSERT_FALSE(collectRenderabilityFacts(&document).isRenderable());

        auto const model = document.get3mfModel();
        ASSERT_NE(model, nullptr);
        auto function = model->AddImplicitFunction();
        function->SetDisplayName("main");
        function->AddOutput("shape", "shape", Lib3MF::eImplicitPortType::Scalar);

        // Act
        auto const scaffold = io::createVolumetricOutputChain(model,
                                                              function,
                                                              {-50.0f, -50.0f, -50.0f},
                                                              {50.0f, 50.0f, 50.0f});

        // Assert
        ASSERT_TRUE(scaffold.success) << scaffold.error;
        EXPECT_NE(scaffold.levelSetId, 0u);
        EXPECT_EQ(scaffold.channel, "shape");

        auto const facts = collectRenderabilityFacts(&document);
        EXPECT_TRUE(facts.isRenderable()) << facts.blockersAsText();
        EXPECT_EQ(facts.buildItemCount, 1);
        EXPECT_EQ(facts.levelSetCount, 1);
        EXPECT_EQ(facts.rootFunctionId, static_cast<ResourceId>(function->GetModelResourceID()));
    }

    TEST(Renderability, DegenerateDomain_IsRejected)
    {
        // Arrange
        auto document = makeDocument();
        document.newModel();
        auto const model = document.get3mfModel();
        ASSERT_NE(model, nullptr);
        auto function = model->AddImplicitFunction();

        // Act
        auto const scaffold = io::createVolumetricOutputChain(model,
                                                              function,
                                                              {10.0f, 10.0f, 10.0f},
                                                              {10.0f, 10.0f, 10.0f});

        // Assert
        EXPECT_FALSE(scaffold.success);
        EXPECT_NE(scaffold.error.find("empty"), std::string::npos) << scaffold.error;
    }
}
