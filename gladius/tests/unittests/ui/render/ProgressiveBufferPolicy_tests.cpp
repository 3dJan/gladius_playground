#include "ui/render/ProgressiveBufferPolicy.h"

#include <gtest/gtest.h>

namespace gladius::ui::async_rendering::tests
{
    using async_rendering::isProgressiveBufferContinuable;
    using async_rendering::ProgressiveBufferState;
    using async_rendering::ProgressiveJobTarget;

    /// A buffer matching the job in scene epoch, view epoch and size may be continued.
    TEST(ProgressiveBufferPolicy_IsContinuable, MatchingEpochViewAndSize_ReturnsTrue)
    {
        ProgressiveBufferState const buffer{
          .hasImage = true, .epoch = 5, .viewEpoch = 9, .width = 800, .height = 600};
        ProgressiveJobTarget const job{.epoch = 5, .viewEpoch = 9, .width = 800, .height = 600};

        EXPECT_TRUE(isProgressiveBufferContinuable(buffer, job));
    }

    /// A larger buffer still fits a smaller job target.
    TEST(ProgressiveBufferPolicy_IsContinuable, LargerBuffer_FitsSmallerTarget_ReturnsTrue)
    {
        ProgressiveBufferState const buffer{
          .hasImage = true, .epoch = 5, .viewEpoch = 9, .width = 1024, .height = 768};
        ProgressiveJobTarget const job{.epoch = 5, .viewEpoch = 9, .width = 800, .height = 600};

        EXPECT_TRUE(isProgressiveBufferContinuable(buffer, job));
    }

    /// No backing image — nothing to continue.
    TEST(ProgressiveBufferPolicy_IsContinuable, NoImage_ReturnsFalse)
    {
        ProgressiveBufferState const buffer{
          .hasImage = false, .epoch = 5, .viewEpoch = 9, .width = 800, .height = 600};
        ProgressiveJobTarget const job{.epoch = 5, .viewEpoch = 9, .width = 800, .height = 600};

        EXPECT_FALSE(isProgressiveBufferContinuable(buffer, job));
    }

    /// A structural/scene-epoch change invalidates the buffer.
    TEST(ProgressiveBufferPolicy_IsContinuable, SceneEpochChanged_ReturnsFalse)
    {
        ProgressiveBufferState const buffer{
          .hasImage = true, .epoch = 5, .viewEpoch = 9, .width = 800, .height = 600};
        ProgressiveJobTarget const job{.epoch = 6, .viewEpoch = 9, .width = 800, .height = 600};

        EXPECT_FALSE(isProgressiveBufferContinuable(buffer, job));
    }

    /// A smaller buffer cannot satisfy a larger render target (e.g. after a window enlarge).
    TEST(ProgressiveBufferPolicy_IsContinuable, SmallerBuffer_ThanTarget_ReturnsFalse)
    {
        ProgressiveBufferState const buffer{
          .hasImage = true, .epoch = 5, .viewEpoch = 9, .width = 640, .height = 480};
        ProgressiveJobTarget const job{.epoch = 5, .viewEpoch = 9, .width = 800, .height = 600};

        EXPECT_FALSE(isProgressiveBufferContinuable(buffer, job));
    }

    /// REGRESSION (torn frame / stale-model bottom band): a parameter change bumps ONLY the view
    /// epoch (RenderWindow::invalidateViewDueToParameterChange does m_asyncViewEpoch++ and does
    /// NOT bump the scene epoch). The previously-seeded progressive buffer therefore still carries
    /// the SAME scene epoch but an OLDER view epoch. It must NOT be continued/reused — otherwise
    /// its not-yet-overwritten region keeps the previous parameters while the top is re-rendered
    /// with the new parameters, producing the reported fresh-model-top / stale-model-bottom tear.
    TEST(ProgressiveBufferPolicy_IsContinuable, ParameterChange_BumpsOnlyViewEpoch_ReturnsFalse)
    {
        // Buffer was seeded under scene epoch 5 / view epoch 9.
        ProgressiveBufferState const buffer{
          .hasImage = true, .epoch = 5, .viewEpoch = 9, .width = 800, .height = 600};

        // Parameter change keeps the scene epoch (5) but advances the view epoch (9 -> 10).
        ProgressiveJobTarget const jobAfterParameterChange{
          .epoch = 5, .viewEpoch = 10, .width = 800, .height = 600};

        EXPECT_FALSE(isProgressiveBufferContinuable(buffer, jobAfterParameterChange))
          << "A parameter-changed job must re-seed the progressive buffer; reusing the stale-view "
             "buffer causes the torn fresh-top / stale-bottom HQ frame.";
    }

    /// REGRESSION (param change followed by resize): the resize re-seeds at the new scene epoch,
    /// but if a leftover buffer at the new size somehow still carries an older view epoch it must
    /// be rejected too. View epoch is the deciding factor independent of size.
    TEST(ProgressiveBufferPolicy_IsContinuable, NewSceneEpochButStaleViewEpoch_ReturnsFalse)
    {
        ProgressiveBufferState const buffer{
          .hasImage = true, .epoch = 6, .viewEpoch = 9, .width = 800, .height = 600};
        ProgressiveJobTarget const job{.epoch = 6, .viewEpoch = 11, .width = 800, .height = 600};

        EXPECT_FALSE(isProgressiveBufferContinuable(buffer, job));
    }

    /// REGRESSION (parameter upload lands mid-fill): the GPU parameter buffer is uploaded
    /// asynchronously and independently of the view epoch, so a single progressive fill carries
    /// one uniform view epoch even when an upload changed the parameters between two chunks. The
    /// epoch/view key alone cannot detect that, leaving the upper band rendered with the old
    /// parameters and the lower band with the new ones — the reported tear. A change in the GPU
    /// parameter generation must therefore reject continuation so the buffer is re-seeded.
    TEST(ProgressiveBufferPolicy_IsContinuable, ParameterGenerationChanged_ReturnsFalse)
    {
        ProgressiveBufferState const buffer{.hasImage = true,
                                            .epoch = 5,
                                            .viewEpoch = 9,
                                            .paramGeneration = 3,
                                            .width = 800,
                                            .height = 600};
        ProgressiveJobTarget const job{
          .epoch = 5, .viewEpoch = 9, .paramGeneration = 4, .width = 800, .height = 600};

        EXPECT_FALSE(isProgressiveBufferContinuable(buffer, job))
          << "A parameter upload that advanced the GPU parameter generation mid-fill must re-seed "
             "the progressive buffer; reusing it produces the old-params-top / new-params-bottom "
             "tear that carries a uniform view epoch.";
    }

    /// A matching parameter generation (alongside matching epoch/view/size) still continues.
    TEST(ProgressiveBufferPolicy_IsContinuable, MatchingParameterGeneration_ReturnsTrue)
    {
        ProgressiveBufferState const buffer{.hasImage = true,
                                            .epoch = 5,
                                            .viewEpoch = 9,
                                            .paramGeneration = 7,
                                            .width = 800,
                                            .height = 600};
        ProgressiveJobTarget const job{
          .epoch = 5, .viewEpoch = 9, .paramGeneration = 7, .width = 800, .height = 600};

        EXPECT_TRUE(isProgressiveBufferContinuable(buffer, job));
    }
}
