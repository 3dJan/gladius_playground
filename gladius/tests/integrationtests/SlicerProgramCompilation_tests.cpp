/// @file SlicerProgramCompilation_tests.cpp
/// @brief Regression tests for dynamically compiled slicer model helpers.

#include "ComputeContext.h"
#include "EventLogger.h"
#include "compute/ComputeCore.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>
#include <sstream>
#include <string>
#include <thread>

namespace gladius_tests::slicer_program_compilation
{
    using namespace gladius;

    class SlicerProgramCompilation_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_logger = std::make_shared<events::Logger>(events::OutputMode::Silent);
            m_context = std::make_shared<ComputeContext>(EnableGLOutput::disabled);

            if (!m_context->isValid())
            {
                GTEST_SKIP() << "OpenCL context not available";
            }
        }

        [[nodiscard]] std::string collectLogMessages() const
        {
            std::ostringstream messages;
            for (auto const & event : *m_logger)
            {
                messages << event.getMessage() << '\n';
            }
            return messages.str();
        }

        std::shared_ptr<ComputeContext> m_context;
        events::SharedLogger m_logger;
    };

    /// @test RecompileBlocking_WithVectorModHelper_DoesNotReportMissingGlslMod3f
    TEST_F(SlicerProgramCompilation_Test,
           RecompileBlocking_WithVectorModHelper_DoesNotReportMissingGlslMod3f)
    {
        auto core = std::make_shared<ComputeCore>(m_context, RequiredCapabilities::ComputeOnly, m_logger);
        auto slicerProgram = core->getSlicerProgram();
        ASSERT_NE(slicerProgram, nullptr);

        m_logger->clear();
        slicerProgram->setModelKernel(R"CLC(
    float4 model(float3 pos, PAYLOAD_ARGS)
    {
        float3 const repeated = glsl_mod3f(pos + (float3)(4.0f, 5.0f, 6.0f),
                                           (float3)(2.0f, 3.0f, 4.0f));
        float const distance = length(repeated - (float3)(1.0f, 1.5f, 2.0f)) - 0.5f;
        return (float4)(0.0f, 0.0f, 0.0f, distance);
    }
    )CLC");

        ASSERT_NO_THROW(slicerProgram->recompileBlocking());
        EXPECT_TRUE(slicerProgram->isValid());

        auto const logMessages = collectLogMessages();
        EXPECT_EQ(logMessages.find("glsl_mod3f"), std::string::npos) << logMessages;
        EXPECT_EQ(logMessages.find("use of undeclared identifier"), std::string::npos)
          << logMessages;
    }

    /// @test IsSlicingInProgress_WithComputeMutexHeld_DoesNotBlockUiThread
    TEST_F(SlicerProgramCompilation_Test,
           IsSlicingInProgress_WithComputeMutexHeld_DoesNotBlockUiThread)
    {
        using namespace std::chrono_literals;

        auto core = std::make_shared<ComputeCore>(m_context, RequiredCapabilities::ComputeOnly, m_logger);

        std::promise<void> lockAcquiredPromise;
        auto lockAcquiredFuture = lockAcquiredPromise.get_future();
        std::promise<void> releasePromise;
        auto releaseFuture = releasePromise.get_future();

        std::thread lockHolder(
          [&]()
          {
              auto computeToken = core->waitForComputeToken();
              lockAcquiredPromise.set_value();
              releaseFuture.wait();
          });

        auto const lockWaitResult = lockAcquiredFuture.wait_for(1s);
        if (lockWaitResult != std::future_status::ready)
        {
            releasePromise.set_value();
            lockHolder.join();
            FAIL() << "Timed out waiting for helper thread to acquire the compute mutex";
        }

        auto statusFuture = std::async(std::launch::async,
                                       [&]() { return core->isSlicingInProgress(); });
        auto const statusWaitResult = statusFuture.wait_for(100ms);

        releasePromise.set_value();
        lockHolder.join();

        ASSERT_EQ(statusWaitResult, std::future_status::ready)
          << "isSlicingInProgress() must not wait for ComputeCore::m_computeMutex";
        EXPECT_FALSE(statusFuture.get());
    }

    /// @test GetContour_WithComputeMutexHeld_DoesNotBlockUiThread
    TEST_F(SlicerProgramCompilation_Test, GetContour_WithComputeMutexHeld_DoesNotBlockUiThread)
    {
        using namespace std::chrono_literals;

        auto core = std::make_shared<ComputeCore>(m_context, RequiredCapabilities::ComputeOnly, m_logger);

        std::promise<void> lockAcquiredPromise;
        auto lockAcquiredFuture = lockAcquiredPromise.get_future();
        std::promise<void> releasePromise;
        auto releaseFuture = releasePromise.get_future();

        std::thread lockHolder(
          [&]()
          {
              auto computeToken = core->waitForComputeToken();
              lockAcquiredPromise.set_value();
              releaseFuture.wait();
          });

        auto const lockWaitResult = lockAcquiredFuture.wait_for(1s);
        if (lockWaitResult != std::future_status::ready)
        {
            releasePromise.set_value();
            lockHolder.join();
            FAIL() << "Timed out waiting for helper thread to acquire the compute mutex";
        }

        auto contourFuture = std::async(std::launch::async, [&]() { return core->getContour(); });
        auto const contourWaitResult = contourFuture.wait_for(100ms);

        releasePromise.set_value();
        lockHolder.join();

        ASSERT_EQ(contourWaitResult, std::future_status::ready)
          << "getContour() must not wait for ComputeCore::m_computeMutex";
        EXPECT_NE(contourFuture.get(), nullptr);
    }
} // namespace gladius_tests::slicer_program_compilation
