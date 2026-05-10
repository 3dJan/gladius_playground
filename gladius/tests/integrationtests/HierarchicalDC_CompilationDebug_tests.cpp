/// @file HierarchicalDC_CompilationDebug_tests.cpp
/// @brief Debug test to show GPU kernel compilation errors

#include "Document.h"
#include "EventLogger.h"
#include "ComputeContext.h"
#include "compute/HierarchicalDCProgram.h"
#include "compute/ComputeCore.h"

#define CL_TARGET_OPENCL_VERSION 120
#include <CL/cl.h>

#include <filesystem>
#include <gtest/gtest.h>
#include <iostream>

namespace gladius_tests::hierarchical_dc_debug
{
    using namespace gladius;

    class HierarchicalDCDebug_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_logger = std::make_shared<events::Logger>();
            m_context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);

            if (!m_context->isValid())
            {
                GTEST_SKIP() << "OpenCL context not available";
            }
        }

        std::shared_ptr<ComputeContext> m_context;
        events::SharedLogger m_logger;
    };

    /// @test ShowGPUKernelCompilationErrors
    TEST_F(HierarchicalDCDebug_Test, ShowGPUKernelCompilationErrors)
    {
        auto core = std::make_shared<ComputeCore>(
          m_context, RequiredCapabilities::ComputeOnly, m_logger);
        
        // Load a document to trigger model compilation
        auto document = std::make_shared<Document>(core);
        document->load("testdata/ImplicitGyroid.3mf");
        
        ASSERT_TRUE(core->updateBBox());
        auto const bbox = core->getBoundingBox();
        ASSERT_TRUE(bbox.has_value());

        // Get the hierarchical DC program
        auto * program = core->getProgramManager().getHierarchicalDCProgram();
        ASSERT_NE(program, nullptr);

        std::cout << "\n=== HierarchicalDCProgram Compilation Test ===\n";
        std::cout << "isValid: " << (program->isValid() ? "YES" : "NO") << "\n";
        std::cout << "isCompilationInProgress: " << (program->isCompilationInProgress() ? "YES" : "NO") << "\n";
        std::cout << "compilationSucceeded: " << (program->compilationSucceeded() ? "YES" : "NO") << "\n";
        std::cout << "compilationProgress: " << program->getCompilationProgress() << "\n";

        // Try to ensure it's compiled
        try
        {
            program->waitForCompilation();
            std::cout << "After waitForCompilation:\n";
            std::cout << "  isValid: " << (program->isValid() ? "YES" : "NO") << "\n";
            std::cout << "  compilationSucceeded: " << (program->compilationSucceeded() ? "YES" : "NO") << "\n";
            
            if (!program->isValid())
            {
                std::cout << "Program is NOT valid after waiting for compilation\n";
                
                // Try to recompile and get errors
                std::cout << "Attempting recompile blocking...\n";
                program->recompileBlocking();
                program->waitForCompilation();
                
                std::cout << "After recompile:\n";
                std::cout << "  isValid: " << (program->isValid() ? "YES" : "NO") << "\n";
                std::cout << "  compilationSucceeded: " << (program->compilationSucceeded() ? "YES" : "NO") << "\n";
            }
        }
        catch (std::exception const & ex)
        {
            std::cout << "Exception during compilation: " << ex.what() << "\n";
        }

        std::cout << "\n=== Final Compilation Status ===\n";
        std::cout << "isValid: " << (program->isValid() ? "YES" : "NO") << "\n";
        std::cout << "compilationSucceeded: " << (program->compilationSucceeded() ? "YES" : "NO") << "\n";
        std::cout << "\n";

        // Test passes even if compilation failed - this is just for debugging
        SUCCEED() << "Check console output above for compilation status";
    }

} // namespace gladius_tests::hierarchical_dc_debug
