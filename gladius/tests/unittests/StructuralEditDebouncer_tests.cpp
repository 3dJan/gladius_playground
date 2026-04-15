#include <Document.h>

#include <chrono>
#include <gtest/gtest.h>
#include <thread>

namespace gladius_tests
{
    using namespace gladius;

    class StructuralEditDebouncerTest : public ::testing::Test
    {
      protected:
        StructuralEditDebouncer m_debouncer;
    };

    TEST_F(StructuralEditDebouncerTest, DefaultState_NotPending)
    {
        EXPECT_FALSE(m_debouncer.pending.load());
    }

    TEST_F(StructuralEditDebouncerTest, DefaultDebounceDelay_Is50ms)
    {
        EXPECT_EQ(m_debouncer.debounceDelay, std::chrono::milliseconds(50));
    }

    TEST_F(StructuralEditDebouncerTest, ArmDebouncer_SetsPendingAndTime)
    {
        auto const before = std::chrono::steady_clock::now();
        m_debouncer.pending.store(true);
        m_debouncer.lastEditTime = std::chrono::steady_clock::now();
        auto const after = std::chrono::steady_clock::now();

        EXPECT_TRUE(m_debouncer.pending.load());
        EXPECT_GE(m_debouncer.lastEditTime, before);
        EXPECT_LE(m_debouncer.lastEditTime, after);
    }

    TEST_F(StructuralEditDebouncerTest, DebounceNotElapsed_StaysPending)
    {
        m_debouncer.pending.store(true);
        m_debouncer.lastEditTime = std::chrono::steady_clock::now();
        m_debouncer.debounceDelay = std::chrono::milliseconds(100);

        // Immediately after — not enough time has passed.
        auto const now = std::chrono::steady_clock::now();
        auto const elapsed = now - m_debouncer.lastEditTime;
        EXPECT_LT(elapsed, m_debouncer.debounceDelay);
    }

    TEST_F(StructuralEditDebouncerTest, DebounceElapsed_CanFire)
    {
        m_debouncer.pending.store(true);
        m_debouncer.lastEditTime = std::chrono::steady_clock::now();
        m_debouncer.debounceDelay = std::chrono::milliseconds(1);

        // Wait well beyond the debounce window for CI reliability.
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto const now = std::chrono::steady_clock::now();
        auto const elapsed = now - m_debouncer.lastEditTime;
        EXPECT_GE(elapsed, m_debouncer.debounceDelay);
    }

    TEST_F(StructuralEditDebouncerTest, RearmDuringDebounce_ExtendsWindow)
    {
        m_debouncer.pending.store(true);
        m_debouncer.debounceDelay = std::chrono::milliseconds(200);
        m_debouncer.lastEditTime = std::chrono::steady_clock::now();

        // Simulate a second edit arriving 20ms later — re-arms the debouncer.
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        auto const rearmTime = std::chrono::steady_clock::now();
        m_debouncer.lastEditTime = rearmTime;

        // After 30ms more (50ms total since first, 30ms since re-arm) — not ready.
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        auto const now = std::chrono::steady_clock::now();
        auto const elapsed = now - m_debouncer.lastEditTime;
        EXPECT_LT(elapsed, m_debouncer.debounceDelay);
    }

    class StructuralEditEpochTest : public ::testing::Test
    {
      protected:
        StructuralEditEpoch m_epoch{0};
    };

    TEST_F(StructuralEditEpochTest, InitialValue_IsZero)
    {
        EXPECT_EQ(m_epoch.load(), 0u);
    }

    TEST_F(StructuralEditEpochTest, IncrementOnce_IsOne)
    {
        m_epoch.fetch_add(1, std::memory_order_relaxed);
        EXPECT_EQ(m_epoch.load(), 1u);
    }

    TEST_F(StructuralEditEpochTest, MultipleIncrements_AreMonotonic)
    {
        for (uint64_t i = 0; i < 10; ++i)
        {
            m_epoch.fetch_add(1, std::memory_order_relaxed);
        }
        EXPECT_EQ(m_epoch.load(), 10u);
    }

    TEST_F(StructuralEditEpochTest, StalenessDetection_EpochMismatch)
    {
        auto const captured = m_epoch.load(std::memory_order_relaxed);
        m_epoch.fetch_add(1, std::memory_order_relaxed);
        EXPECT_NE(m_epoch.load(std::memory_order_relaxed), captured);
    }

    TEST_F(StructuralEditEpochTest, StalenessDetection_EpochMatch)
    {
        m_epoch.fetch_add(1, std::memory_order_relaxed);
        auto const captured = m_epoch.load(std::memory_order_relaxed);
        EXPECT_EQ(m_epoch.load(std::memory_order_relaxed), captured);
    }

} // namespace gladius_tests
