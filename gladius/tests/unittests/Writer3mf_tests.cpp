
#include "Document.h"
#include "EventLogger.h"
#include "ExpressionParser.h"
#include "ProgramSnippetApplier.h"
#include "io/3mf/Writer3mf.h"
#include "nodes/types.h"
#include "nodes/utils.h"

#include <gmock/gmock.h>

#include <algorithm>

namespace gladius_tests
{

    using namespace gladius;

    TEST(Writer3mfTest, ConvertMatrix4x4)
    {
        // Test case 1: Identity matrix
        gladius::nodes::Matrix4x4 identityMat = identityMatrix();
        
        Lib3MF::sMatrix4x4 convertedMatrix = io::convertMatrix4x4(identityMat);
        for (int row = 0; row < 4; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                if (row == col)
                    EXPECT_EQ(convertedMatrix.m_Field[row][col], 1.0);
                else
                    EXPECT_EQ(convertedMatrix.m_Field[row][col], 0.0);
            }
        }

        // Test case 2: Arbitrary matrix
        gladius::nodes::Matrix4x4 arbitraryMatrix = zeroMatrix();
        
        arbitraryMatrix[0] [0] = 1.0;
        arbitraryMatrix[1] [1] = 2.0;
        arbitraryMatrix[2] [2] = 3.0;
        arbitraryMatrix[3] [3] = 4.0;
        
        convertedMatrix = io::convertMatrix4x4(arbitraryMatrix);
        for (int row = 0; row < 4; ++row)
        {
            for (int col = 0; col < 4; ++col)
            {
                if (row == col)
                    EXPECT_EQ(convertedMatrix.m_Field[row][col], arbitraryMatrix[row][col]);
                else
                    EXPECT_EQ(convertedMatrix.m_Field[row][col], 0.0);
            }
        }
    }

    namespace
    {
        /// @brief Collects all error events which were added to the logger since `beginOffset`
        std::vector<std::string> collectErrors(events::SharedLogger const & logger,
                                               std::size_t beginOffset)
        {
            std::vector<std::string> errors;
            for (auto it = logger->cbegin() + static_cast<long>(beginOffset);
                 it != logger->cend();
                 ++it)
            {
                if (it->getSeverity() == events::Severity::Error
                    || it->getSeverity() == events::Severity::FatalError)
                {
                    errors.push_back(it->getMessage());
                }
            }
            return errors;
        }
    } // namespace

    TEST(Writer3mfTest, UpdateModel_ForwardReferencingHelper_WritesWithoutErrors)
    {
        // Arrange: a low id function which calls a helper that carries a higher id. Writing the
        // functions in ascending id order would need the helper to exist before it is written.
        constexpr char const * const snippet =
          "// Function: sphere (ID: 60)\n"
          "float sphere_60(vec3 pos, float radius) {\n"
          "  return length(pos) - radius;\n"
          "}\n"
          "\n"
          "// Function: main (ID: 3)\n"
          "(vec3 color, float shape) main_3(vec3 pos) {\n"
          "  color = vec3(0.1, 0.2, 1.0);\n"
          "  shape = sphere_60(pos, 5.0);\n"
          "}\n";

        auto logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
        Document doc{logger};
        doc.newModel();
        auto assembly = doc.getAssembly();
        ASSERT_NE(assembly, nullptr);

        ExpressionParser parser;
        auto const report = ProgramSnippetApplier::apply(snippet, *assembly, parser);
        ASSERT_TRUE(report.failed.empty()) << report.failed.front().message;
        doc.rebuildResourceDependencyGraph();

        // Act
        auto const before = logger->size();
        doc.update3mfModel();

        // Assert: no writer errors and both functions landed in the 3MF model
        auto const errors = collectErrors(logger, before);
        EXPECT_TRUE(errors.empty())
          << (errors.empty() ? std::string{"no error"} : errors.front());

        std::vector<std::string> writtenNames;
        auto functionIter = doc.get3mfModel()->GetFunctions();
        while (functionIter->MoveNext())
        {
            writtenNames.push_back(functionIter->GetCurrentFunction()->GetDisplayName());
        }
        EXPECT_THAT(writtenNames, ::testing::AllOf(::testing::Contains("main"),
                                                  ::testing::Contains("sphere")));
    }
} // namespace gladius_tests