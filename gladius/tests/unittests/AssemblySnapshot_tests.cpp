#include <nodes/Assembly.h>
#include <nodes/Model.h>
#include <nodes/nodesfwd.h>

#include <gtest/gtest.h>
#include <memory>

namespace gladius_tests
{
    using namespace gladius;
    using namespace gladius::nodes;

    class AssemblySnapshotTest : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            m_original = std::make_shared<Assembly>();
            m_original->assemblyModel()->createValidVoid();
        }

        static auto constexpr UNUSED_MODEL_ID_A = 999;
        static auto constexpr UNUSED_MODEL_ID_B = 998;
        SharedAssembly m_original;
    };

    TEST_F(AssemblySnapshotTest, DeepCopy_ProducesIndependentSnapshot)
    {
        // Act: deep copy the assembly.
        auto snapshot = std::make_shared<Assembly>(*m_original);

        // Verify: both have the same number of models initially.
        EXPECT_EQ(snapshot->getFunctions().size(), m_original->getFunctions().size());
    }

    TEST_F(AssemblySnapshotTest, ModifyOriginal_SnapshotUnaffected)
    {
        auto snapshot = std::make_shared<Assembly>(*m_original);
        auto const snapshotModelCount = snapshot->getFunctions().size();

        // Add a new model to the original.
        m_original->addModelIfNotExisting(UNUSED_MODEL_ID_A);

        // The snapshot must NOT see the new model.
        EXPECT_EQ(snapshot->getFunctions().size(), snapshotModelCount);
        EXPECT_GT(m_original->getFunctions().size(), snapshotModelCount);
    }

    TEST_F(AssemblySnapshotTest, ModifySnapshot_OriginalUnaffected)
    {
        auto snapshot = std::make_shared<Assembly>(*m_original);
        auto const originalModelCount = m_original->getFunctions().size();

        // Add a new model to the snapshot.
        snapshot->addModelIfNotExisting(UNUSED_MODEL_ID_B);

        // The original must NOT see the new model.
        EXPECT_EQ(m_original->getFunctions().size(), originalModelCount);
        EXPECT_GT(snapshot->getFunctions().size(), originalModelCount);
    }

} // namespace gladius_tests
