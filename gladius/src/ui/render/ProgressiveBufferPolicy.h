#pragma once

#include <cstdint>

namespace gladius::ui::async_rendering
{
    /// Snapshot of the currently held progressive high-quality accumulation buffer.
    struct ProgressiveBufferState
    {
        bool hasImage{false};       ///< A progressive buffer with a backing image exists.
        uint64_t epoch{0};          ///< Scene/model-topology generation the buffer was seeded for.
        uint64_t viewEpoch{0};      ///< Camera/parameter generation the buffer content reflects.
        uint64_t paramGeneration{0}; ///< GPU parameter-buffer generation the buffer's HQ lines used.
        uint32_t width{0};          ///< Allocated buffer width in pixels.
        uint32_t height{0};         ///< Allocated buffer height in pixels.
    };

    /// Target of the high-quality render job about to be scheduled.
    struct ProgressiveJobTarget
    {
        uint64_t epoch{0};          ///< Scene/model-topology generation of the new job.
        uint64_t viewEpoch{0};      ///< Camera/parameter generation of the new job.
        uint64_t paramGeneration{0}; ///< GPU parameter-buffer generation the new job will render with.
        uint32_t width{0};          ///< Requested render width in pixels.
        uint32_t height{0};         ///< Requested render height in pixels.
    };

    /**
     * @brief Decides whether an existing progressive HQ buffer may be continued or reused
     *        without re-seeding for a new render job.
     *
     * A progressive buffer is filled top-to-bottom over several frames; the not-yet-rendered
     * region keeps whatever content it was last seeded with. Continuing (rendering from a
     * non-zero start line) or reusing it without re-seeding is only safe when its content
     * matches the new job in every dimension that affects the displayed pixels:
     *
     *  - @b scene/model-topology epoch: structural model changes invalidate all pixels.
     *  - @b view epoch: a camera move OR a parameter change bumps the view epoch. Critically,
     *    a parameter change bumps @e only the view epoch (not the scene epoch) yet changes the
     *    model content of every pixel. If the view epoch is ignored, a parameter-changed job
     *    reuses a buffer whose un-overwritten region still shows the previous parameters,
     *    producing a torn frame: fresh-model top band, stale-model bottom band.
     *  - @b parameter generation: the GPU parameter buffer is uploaded asynchronously and
     *    independently of the view epoch. A single fill carries one view epoch, but if a
     *    parameter upload lands between two chunks the upper band is rendered with the old
     *    parameters and the lower band with the new ones -- a torn frame the view epoch alone
     *    cannot detect. Requiring an equal parameter generation forces a re-seed in that case.
     *  - @b size: the buffer must be at least as large as the requested render target.
     *
     * @return true if the buffer is safe to continue/reuse as-is; false if it must be
     *         re-seeded (or a fresh buffer acquired) so every line is rendered for the new state.
     */
    [[nodiscard]] constexpr bool isProgressiveBufferContinuable(
      ProgressiveBufferState const & buffer, ProgressiveJobTarget const & job) noexcept
    {
        return buffer.hasImage && buffer.epoch == job.epoch &&
               buffer.viewEpoch == job.viewEpoch &&
               buffer.paramGeneration == job.paramGeneration && buffer.width >= job.width &&
               buffer.height >= job.height;
    }
}
