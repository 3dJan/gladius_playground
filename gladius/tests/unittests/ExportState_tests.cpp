#include "ui/ExportState.h"
#include <gtest/gtest.h>
#include <thread>

namespace gladius::ui::tests
{
    /// @brief Test fixture for ExportState tests
    class ExportStateTest : public ::testing::Test
    {
      protected:
        ExportState m_state;
    };

    TEST_F(ExportStateTest, InitialState_IsNotExporting)
    {
        EXPECT_FALSE(m_state.isExportInProgress());
        EXPECT_TRUE(m_state.getExportDescription().empty());
    }

    TEST_F(ExportStateTest, BeginExport_SetsExportInProgress)
    {
        m_state.beginExport("Test export");

        EXPECT_TRUE(m_state.isExportInProgress());
        EXPECT_EQ(m_state.getExportDescription(), "Test export");
    }

    TEST_F(ExportStateTest, BeginExport_WithDefaultDescription_UsesDefault)
    {
        m_state.beginExport();

        EXPECT_TRUE(m_state.isExportInProgress());
        EXPECT_EQ(m_state.getExportDescription(), "Mesh export");
    }

    TEST_F(ExportStateTest, EndExport_ClearsExportState)
    {
        m_state.beginExport("Test export");
        m_state.endExport();

        EXPECT_FALSE(m_state.isExportInProgress());
        EXPECT_TRUE(m_state.getExportDescription().empty());
    }

    TEST_F(ExportStateTest, MultipleBeginEndCycles_WorkCorrectly)
    {
        for (int i = 0; i < 3; ++i)
        {
            m_state.beginExport("Cycle " + std::to_string(i));
            EXPECT_TRUE(m_state.isExportInProgress());
            EXPECT_EQ(m_state.getExportDescription(), "Cycle " + std::to_string(i));

            m_state.endExport();
            EXPECT_FALSE(m_state.isExportInProgress());
        }
    }

    /// @brief Test fixture for ExportGuard tests
    class ExportGuardTest : public ::testing::Test
    {
      protected:
        ExportState m_state;
    };

    TEST_F(ExportGuardTest, Guard_SetsExportOnConstruction)
    {
        {
            ExportGuard guard(m_state, "Guard test");
            EXPECT_TRUE(m_state.isExportInProgress());
            EXPECT_EQ(m_state.getExportDescription(), "Guard test");
        }
    }

    TEST_F(ExportGuardTest, Guard_ClearsExportOnDestruction)
    {
        {
            ExportGuard guard(m_state, "Guard test");
        }
        EXPECT_FALSE(m_state.isExportInProgress());
        EXPECT_TRUE(m_state.getExportDescription().empty());
    }

    TEST_F(ExportGuardTest, Guard_WithDefaultDescription_UsesDefault)
    {
        {
            ExportGuard guard(m_state);
            EXPECT_EQ(m_state.getExportDescription(), "Mesh export");
        }
    }

    TEST_F(ExportGuardTest, Guard_ClearsStateOnException)
    {
        try
        {
            ExportGuard guard(m_state, "Exception test");
            EXPECT_TRUE(m_state.isExportInProgress());
            throw std::runtime_error("Test exception");
        }
        catch (std::runtime_error const &)
        {
            // Expected
        }
        EXPECT_FALSE(m_state.isExportInProgress());
    }

    /// @brief Thread safety tests for ExportState
    class ExportStateThreadTest : public ::testing::Test
    {
      protected:
        ExportState m_state;
    };

    TEST_F(ExportStateThreadTest, IsExportInProgress_IsThreadSafe)
    {
        // Start export in main thread
        m_state.beginExport("Thread test");

        // Check from another thread
        bool observedState = false;
        std::thread t([&]() { observedState = m_state.isExportInProgress(); });
        t.join();

        EXPECT_TRUE(observedState);
        m_state.endExport();
    }

    TEST_F(ExportStateThreadTest, ConcurrentReadsDuringExport_AreConsistent)
    {
        m_state.beginExport("Concurrent test");

        std::vector<std::thread> threads;
        std::atomic<int> trueCount{0};
        int const numThreads = 10;
        int const checksPerThread = 100;

        for (int i = 0; i < numThreads; ++i)
        {
            threads.emplace_back([&]() {
                for (int j = 0; j < checksPerThread; ++j)
                {
                    if (m_state.isExportInProgress())
                    {
                        ++trueCount;
                    }
                }
            });
        }

        for (auto & t : threads)
        {
            t.join();
        }

        // All reads should have observed true since export is in progress
        EXPECT_EQ(trueCount.load(), numThreads * checksPerThread);
        m_state.endExport();
    }

    /// @brief Test fixture for ExportPhase tests
    class ExportPhaseTest : public ::testing::Test
    {
      protected:
        ExportState m_state;
    };

    TEST_F(ExportPhaseTest, InitialState_PhaseIsIdle)
    {
        EXPECT_EQ(m_state.getPhase(), ExportPhase::Idle);
        EXPECT_FALSE(m_state.isCancelling());
    }

    TEST_F(ExportPhaseTest, BeginExport_SetsPhaseToExporting)
    {
        m_state.beginExport("Test");

        EXPECT_EQ(m_state.getPhase(), ExportPhase::Exporting);
        EXPECT_FALSE(m_state.isCancelling());
    }

    TEST_F(ExportPhaseTest, RequestCancellation_SetsPhaseToCancelling)
    {
        m_state.beginExport("Test");
        m_state.requestCancellation();

        EXPECT_EQ(m_state.getPhase(), ExportPhase::Cancelling);
        EXPECT_TRUE(m_state.isCancelling());
    }

    TEST_F(ExportPhaseTest, RequestCancellation_WhenIdle_DoesNothing)
    {
        m_state.requestCancellation();

        EXPECT_EQ(m_state.getPhase(), ExportPhase::Idle);
        EXPECT_FALSE(m_state.isCancelling());
    }

    TEST_F(ExportPhaseTest, EndExport_ResetsPhaseToIdle)
    {
        m_state.beginExport("Test");
        m_state.requestCancellation();
        m_state.endExport();

        EXPECT_EQ(m_state.getPhase(), ExportPhase::Idle);
        EXPECT_FALSE(m_state.isCancelling());
    }

    TEST_F(ExportPhaseTest, MultipleRequestCancellation_RemainsInCancelling)
    {
        m_state.beginExport("Test");
        m_state.requestCancellation();
        m_state.requestCancellation();
        m_state.requestCancellation();

        EXPECT_EQ(m_state.getPhase(), ExportPhase::Cancelling);
    }

    TEST_F(ExportPhaseTest, Phase_ThreadSafetyWithCancellation)
    {
        m_state.beginExport("Thread test");

        std::atomic<bool> sawCancelling{false};
        std::thread observer([&]() {
            for (int i = 0; i < 1000; ++i)
            {
                if (m_state.getPhase() == ExportPhase::Cancelling)
                {
                    sawCancelling = true;
                    break;
                }
                std::this_thread::yield();
            }
        });

        std::this_thread::sleep_for(std::chrono::microseconds(100));
        m_state.requestCancellation();

        observer.join();
        EXPECT_TRUE(sawCancelling);
        m_state.endExport();
    }

} // namespace gladius::ui::tests
