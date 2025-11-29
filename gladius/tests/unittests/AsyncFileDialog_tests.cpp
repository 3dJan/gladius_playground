#include "ui/FileDialogService.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <thread>

namespace gladius::ui::tests
{
    using namespace std::chrono_literals;

    class AsyncFileDialogTest : public ::testing::Test
    {
      protected:
        AsyncFileDialog m_dialog;
        std::atomic<int> m_dialogCallCount{0};
        std::atomic<bool> m_dialogShouldBlock{false};
        std::atomic<bool> m_dialogCanProceed{false};

        void SetUp() override
        {
            m_dialogCallCount = 0;
            m_dialogShouldBlock = false;
            m_dialogCanProceed = false;
        }

        /// @brief Set up a mock dialog that returns immediately with a path
        void setupInstantDialog(std::filesystem::path resultPath)
        {
            m_dialog.setTestDialogFunc(
              [this, resultPath](FilePatterns, std::filesystem::path) -> QueriedFilename {
                  ++m_dialogCallCount;
                  return resultPath;
              });
        }

        /// @brief Set up a mock dialog that returns immediately with nullopt (cancelled)
        void setupCancelledDialog()
        {
            m_dialog.setTestDialogFunc(
              [this](FilePatterns, std::filesystem::path) -> QueriedFilename {
                  ++m_dialogCallCount;
                  return std::nullopt;
              });
        }

        /// @brief Set up a mock dialog that blocks until released
        void setupBlockingDialog(std::filesystem::path resultPath)
        {
            m_dialog.setTestDialogFunc(
              [this, resultPath](FilePatterns, std::filesystem::path) -> QueriedFilename {
                  ++m_dialogCallCount;
                  while (m_dialogShouldBlock && !m_dialogCanProceed)
                  {
                      std::this_thread::sleep_for(1ms);
                  }
                  return resultPath;
              });
        }

        /// @brief Simulate one "frame" of the render loop
        /// @return true if button would be clicked (dialog not active)
        bool simulateFrame(bool userClicksButton)
        {
            bool buttonClicked = false;

            // Check if button is enabled (dialog not active)
            bool const dialogActive = m_dialog.isActive();

            if (!dialogActive && userClicksButton)
            {
                // Button was clicked
                buttonClicked = true;
                m_dialog.saveFile({"*.stl"}, "test.stl");
            }

            // Check for result (AFTER button, as per our fix)
            if (auto result = m_dialog.checkResult())
            {
                // Result consumed
            }

            return buttonClicked;
        }
    };

    TEST_F(AsyncFileDialogTest, InitialState_NotActive)
    {
        EXPECT_FALSE(m_dialog.isActive());
    }

    TEST_F(AsyncFileDialogTest, InitialState_NoResult)
    {
        auto result = m_dialog.checkResult();
        EXPECT_FALSE(result.has_value());
    }

    TEST_F(AsyncFileDialogTest, SaveFile_BecomesActive)
    {
        m_dialogShouldBlock = true;
        setupBlockingDialog("/test/file.stl");

        m_dialog.saveFile({"*.stl"}, "default.stl");

        // Give thread time to start
        std::this_thread::sleep_for(10ms);

        EXPECT_TRUE(m_dialog.isActive());
        EXPECT_EQ(1, m_dialogCallCount.load());

        // Release the dialog
        m_dialogCanProceed = true;
    }

    TEST_F(AsyncFileDialogTest, SaveFile_WhileActive_Ignored)
    {
        m_dialogShouldBlock = true;
        setupBlockingDialog("/test/file.stl");

        m_dialog.saveFile({"*.stl"}, "default.stl");
        std::this_thread::sleep_for(10ms);

        // Try to start another dialog while active
        m_dialog.saveFile({"*.stl"}, "another.stl");
        std::this_thread::sleep_for(10ms);

        // Should still only have one call
        EXPECT_EQ(1, m_dialogCallCount.load());

        m_dialogCanProceed = true;
    }

    TEST_F(AsyncFileDialogTest, CheckResult_ReturnsResultWhenReady)
    {
        setupInstantDialog("/selected/file.stl");

        m_dialog.saveFile({"*.stl"}, "default.stl");

        // Wait for async to complete
        std::this_thread::sleep_for(50ms);

        auto result = m_dialog.checkResult();
        ASSERT_TRUE(result.has_value());
        ASSERT_TRUE(result->has_value());
        EXPECT_EQ("/selected/file.stl", result->value().string());
    }

    TEST_F(AsyncFileDialogTest, CheckResult_ReturnsNulloptForCancelled)
    {
        setupCancelledDialog();

        m_dialog.saveFile({"*.stl"}, "default.stl");
        std::this_thread::sleep_for(50ms);

        auto result = m_dialog.checkResult();
        ASSERT_TRUE(result.has_value());    // We got a result
        EXPECT_FALSE(result->has_value());  // But it's nullopt (cancelled)
    }

    TEST_F(AsyncFileDialogTest, CheckResult_OnlyReturnsOnce)
    {
        setupInstantDialog("/selected/file.stl");

        m_dialog.saveFile({"*.stl"}, "default.stl");
        std::this_thread::sleep_for(50ms);

        auto result1 = m_dialog.checkResult();
        auto result2 = m_dialog.checkResult();

        EXPECT_TRUE(result1.has_value());
        EXPECT_FALSE(result2.has_value());  // Second call returns nothing
    }

    TEST_F(AsyncFileDialogTest, IsActive_FalseAfterResultConsumed)
    {
        setupInstantDialog("/selected/file.stl");

        m_dialog.saveFile({"*.stl"}, "default.stl");
        std::this_thread::sleep_for(50ms);

        // Future is ready, but result not consumed - should still be "active"
        EXPECT_TRUE(m_dialog.isActive());

        m_dialog.checkResult();  // Consume result

        EXPECT_FALSE(m_dialog.isActive());  // Now truly inactive
    }

    TEST_F(AsyncFileDialogTest, CanStartNewDialogAfterResultConsumed)
    {
        setupInstantDialog("/selected/file.stl");

        m_dialog.saveFile({"*.stl"}, "first.stl");
        std::this_thread::sleep_for(50ms);
        m_dialog.checkResult();

        // Should be able to start a new dialog
        m_dialog.saveFile({"*.stl"}, "second.stl");
        std::this_thread::sleep_for(50ms);

        EXPECT_EQ(2, m_dialogCallCount.load());
    }

    // This test simulates the problematic scenario:
    // Frame N: Dialog completes, result consumed, button should NOT be clickable
    TEST_F(AsyncFileDialogTest, SimulateRenderLoop_NoDoubleClick)
    {
        setupInstantDialog("/selected/file.stl");

        // Frame 1: User clicks button, dialog starts
        EXPECT_FALSE(m_dialog.isActive());  // Initially not active
        m_dialog.saveFile({"*.stl"}, "test.stl");
        
        // Wait for async to execute
        std::this_thread::sleep_for(50ms);
        EXPECT_EQ(1, m_dialogCallCount.load());

        // Frame 2: Dialog just completed. isActive() should still be true
        // because we haven't consumed the result yet.
        EXPECT_TRUE(m_dialog.isActive());  // Still "active" until result consumed

        // Simulate button check - should be disabled
        bool const dialogActive = m_dialog.isActive();
        if (!dialogActive)
        {
            m_dialog.saveFile({"*.stl"}, "second.stl");  // This should NOT happen
        }
        
        // Now consume the result
        auto result = m_dialog.checkResult();
        EXPECT_TRUE(result.has_value());

        // Should still be only one dialog call
        EXPECT_EQ(1, m_dialogCallCount.load());

        // Frame 3: Now truly inactive, user can click again
        EXPECT_FALSE(m_dialog.isActive());
        m_dialog.saveFile({"*.stl"}, "third.stl");
        
        std::this_thread::sleep_for(50ms);
        EXPECT_EQ(2, m_dialogCallCount.load());
    }

    // This test checks the race condition more carefully
    TEST_F(AsyncFileDialogTest, RaceCondition_IsActiveBeforeCheckResult)
    {
        setupInstantDialog("/selected/file.stl");

        m_dialog.saveFile({"*.stl"}, "test.stl");
        std::this_thread::sleep_for(50ms);

        // At this point, future is ready but not consumed
        // isActive() should return TRUE because we have an unconsumed result
        bool activeBeforeCheck = m_dialog.isActive();
        EXPECT_TRUE(activeBeforeCheck);  // FIXED: Should be true to prevent double-click

        auto result = m_dialog.checkResult();
        EXPECT_TRUE(result.has_value());

        bool activeAfterCheck = m_dialog.isActive();
        EXPECT_FALSE(activeAfterCheck);  // Now inactive after consuming result
    }

    // This test demonstrates the fix for the bug scenario
    TEST_F(AsyncFileDialogTest, BugScenario_ButtonEnabledWhenFutureReady_Fixed)
    {
        setupInstantDialog("/selected/file.stl");

        // Start dialog
        m_dialog.saveFile({"*.stl"}, "test.stl");
        std::this_thread::sleep_for(50ms);

        // Now with the fix, isActive() should return TRUE even when future is ready
        // because we haven't consumed the result yet
        bool const dialogActive = m_dialog.isActive();
        EXPECT_TRUE(dialogActive);  // FIXED: Button should be disabled!

        // If we check the button state correctly, we won't start a new dialog
        if (!dialogActive)
        {
            m_dialog.saveFile({"*.stl"}, "second.stl");
        }

        // Consume the first result
        auto result = m_dialog.checkResult();
        EXPECT_TRUE(result.has_value());

        // Now we should only have one dialog call
        std::this_thread::sleep_for(50ms);
        EXPECT_EQ(1, m_dialogCallCount.load());  // FIXED: Only one dialog!
    }

} // namespace gladius::ui::tests
