#include "ui/render/DisplayFrameSelector.h"
#include "ui/render/ProgressiveBufferPolicy.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace gladius::ui::async_rendering::tests
{
    namespace
    {
        /// Sentinel for a line whose content is undefined (freshly acquired buffer, never written).
        constexpr int64_t kGarbage = -1;

        /// Continuation policy the simulation drives.
        ///   SceneEpochOnly      = pre-fix: reuse keyed on scene epoch + size only.
        ///   SceneAndView        = the committed fix: drives the real isProgressiveBufferContinuable
        ///                         (scene + view epoch + size). Stamps the buffer view epoch at seed.
        ///   SceneViewAndParam   = candidate fix: SceneAndView PLUS the GPU parameter-upload
        ///                         generation, so an async parameter upload mid-fill restarts the
        ///                         fill instead of leaving a half-old / half-new buffer.
        enum class Policy
        {
            SceneEpochOnly,
            SceneAndView,
            SceneViewAndParam
        };

        char const * policyName(Policy p)
        {
            switch (p)
            {
            case Policy::SceneEpochOnly:
                return "SceneEpochOnly(pre-fix)";
            case Policy::SceneAndView:
                return "SceneAndView(committed-fix)";
            case Policy::SceneViewAndParam:
                return "SceneViewAndParam(candidate-fix)";
            }
            return "?";
        }

        /// The actual model state a single rendered line represents: the view (camera/parameter
        /// epoch) AND the GPU parameter-buffer generation that was uploaded when the line rendered.
        /// Two lines that differ in EITHER field show different content => a torn frame.
        struct LineStamp
        {
            int64_t viewEpoch{kGarbage};
            int64_t paramGen{kGarbage};

            bool operator<(LineStamp const & o) const
            {
                return viewEpoch < o.viewEpoch ||
                       (viewEpoch == o.viewEpoch && paramGen < o.paramGen);
            }

            bool isGarbage() const
            {
                return viewEpoch == kGarbage || paramGen == kGarbage;
            }
        };

        /// Mirrors a progressive / front image buffer plus its display-gating metadata.
        struct MockImage
        {
            bool valid{false};
            uint64_t epoch{0};          ///< m_asyncProgressiveEpoch (scene epoch)
            uint64_t viewEpoch{0};      ///< m_asyncProgressiveViewEpoch (display gating)
            uint64_t renderParamGen{0}; ///< GPU param generation this buffer's HQ lines target
            uint32_t width{0};
            uint32_t height{0};
            std::vector<LineStamp> lines; ///< per-line content stamp

            void allocate(uint32_t w, uint32_t h, LineStamp fill)
            {
                width = w;
                height = h;
                lines.assign(h, fill);
                valid = true;
            }

            void clear()
            {
                *this = MockImage{};
            }
        };

        std::set<LineStamp> distinctStamps(MockImage const & img)
        {
            return std::set<LineStamp>(img.lines.begin(), img.lines.end());
        }

        bool hasGarbage(MockImage const & img)
        {
            return std::any_of(
              img.lines.begin(), img.lines.end(), [](LineStamp const & s) { return s.isGarbage(); });
        }

        struct Job
        {
            bool active{false};
            uint64_t epoch{0};
            uint64_t viewEpoch{0};
            uint64_t paramGen{0};
            uint32_t width{0};
            uint32_t height{0};
            size_t startLine{0};
            size_t stepSize{0};
        };

        struct Result
        {
            bool present{false};
            uint64_t epoch{0};
            uint64_t viewEpoch{0};
            uint32_t width{0};
            uint32_t height{0};
            size_t completedLine{0};
            bool completedFrame{false};
        };

        struct DisplayObservation
        {
            DisplayFrameSource source{DisplayFrameSource::None};
            std::set<LineStamp> shown;
            bool torn{false};
        };

        /// GPU-free model of the RenderWindow progressive-HQ state machine, extended with a GPU
        /// parameter-buffer generation that is uploaded ASYNCHRONOUSLY and independently of the
        /// view epoch. The async worker runs at the end of tick N; its result is consumed at the
        /// start of tick N+1 (one-frame latency), so resizes / parameter uploads can interleave
        /// between a chunk being scheduled and its result being processed.
        class RenderWindowSim
        {
        public:
            RenderWindowSim(Policy policy, uint32_t width, uint32_t height, size_t chunkLines)
                : m_policy(policy)
                , m_resultWidth(width)
                , m_resultHeight(height)
                , m_chunkLines(chunkLines)
            {
            }

            /// invalidateViewDueToParameterChange(): bumps ONLY the view epoch and records that a
            /// NEW parameter generation has been requested. Crucially, the GPU parameter buffer is
            /// NOT updated here — the upload happens later/asynchronously (deliverParameterUpload).
            void parameterChange()
            {
                ++m_viewEpoch;
                ++m_pendingParamGen;
                m_currentLine = 0;
                m_isRendering = false;
            }

            /// The async parameter upload finally lands in *m_primitives (Document::updateParameter
            /// -> tryToupdateParameter -> updateParameterBlocking). From now on, HQ chunks render
            /// against the new parameters. This is decoupled in time from parameterChange().
            /// When bumpViewEpoch is true it models the candidate minimal fix: bumping the view
            /// epoch at the moment the upload completes (not at request time) so any in-flight
            /// fill that started against the old parameters becomes view-outdated and restarts.
            void deliverParameterUpload(bool bumpViewEpoch)
            {
                m_gpuParamGen = m_pendingParamGen;
                if (bumpViewEpoch)
                {
                    ++m_viewEpoch;
                    m_currentLine = 0;
                    m_isRendering = false;
                }
            }

            /// A low-res preview for the current view/param resamples into the result image.
            void lowResPreviewArrives()
            {
                m_resultStamp = LineStamp{static_cast<int64_t>(m_viewEpoch),
                                          static_cast<int64_t>(m_gpuParamGen)};
                m_lastLowResViewEpoch = m_viewEpoch;
            }

            void requestResize(uint32_t w, uint32_t h)
            {
                m_desiredWidth = w;
                m_desiredHeight = h;
            }

            DisplayObservation tick()
            {
                processResults();
                resizeBarrier();
                scheduleChunkIfNeeded();
                runWorker();
                return observeDisplay();
            }

        private:
            bool continuable(MockImage const & buf, Job const & job) const
            {
                bool baseOk = false;
                if (m_policy == Policy::SceneEpochOnly)
                {
                    baseOk = buf.valid && buf.epoch == job.epoch && buf.width >= job.width &&
                             buf.height >= job.height;
                }
                else
                {
                    // Drive the REAL production predicate for the epoch/view/size key.
                    ProgressiveBufferState const state{.hasImage = buf.valid,
                                                       .epoch = buf.epoch,
                                                       .viewEpoch = buf.viewEpoch,
                                                       .width = buf.width,
                                                       .height = buf.height};
                    ProgressiveJobTarget const target{.epoch = job.epoch,
                                                      .viewEpoch = job.viewEpoch,
                                                      .width = job.width,
                                                      .height = job.height};
                    baseOk = isProgressiveBufferContinuable(state, target);
                }
                if (m_policy == Policy::SceneViewAndParam)
                {
                    // Candidate fix: also require the buffer's HQ lines to target the parameter
                    // generation currently uploaded to the GPU. If an upload landed since the
                    // buffer was seeded, the buffer is no longer continuable and must restart.
                    baseOk = baseOk && buf.renderParamGen == job.paramGen;
                }
                return baseOk;
            }

            void scheduleChunkIfNeeded()
            {
                if (m_returnedEarlyThisTick || m_jobInFlight)
                {
                    return;
                }
                if (!m_isRendering && !m_dirty)
                {
                    return;
                }

                Job job{};
                job.active = true;
                job.epoch = m_sceneEpoch;
                job.viewEpoch = m_viewEpoch;
                job.paramGen = m_gpuParamGen; // chunks render against the CURRENTLY uploaded params
                job.width = m_resultWidth;
                job.height = m_resultHeight;

                bool const seeded = continuable(m_prog, job);
                if (m_currentLine > 0 && !seeded)
                {
                    m_currentLine = 0;
                }
                job.startLine = std::min(m_currentLine, static_cast<size_t>(job.height));
                job.stepSize = std::max<size_t>(
                  1, std::min(m_chunkLines, static_cast<size_t>(job.height) - job.startLine));

                if (job.startLine == 0 && !seeded)
                {
                    // Seed: resample the low-res result image across the whole buffer. If that
                    // low-res preview is for an older view than the job, the seed band would mix a
                    // stale view with the fresh HQ top, so HQ display is suppressed until a fully
                    // view-consistent frame completes (mirrors the seedViewIsStale guard).
                    bool const seedViewIsStale = m_lastLowResViewEpoch != job.viewEpoch;
                    if (seedViewIsStale)
                    {
                        m_suppressHQDisplay = true;
                    }
                    m_prog.allocate(job.width, job.height, m_resultStamp);
                    m_prog.epoch = job.epoch;
                    m_prog.renderParamGen = job.paramGen;
                    if (m_policy != Policy::SceneEpochOnly)
                    {
                        m_prog.viewEpoch = job.viewEpoch; // committed fix stamps view epoch at seed
                    }
                }

                m_dirty = false;
                m_isRendering = true;
                m_jobInFlight = true;
                m_inFlightEpoch = job.epoch;
                m_pendingJob = job;
            }

            void runWorker()
            {
                if (!m_pendingJob.active)
                {
                    return;
                }
                Job const job = m_pendingJob;
                m_pendingJob = Job{};

                if (!continuable(m_prog, job))
                {
                    // Worker acquire-fresh branch: brand-new UNSEEDED buffer (garbage lines).
                    m_prog.allocate(job.width, job.height, LineStamp{});
                    m_prog.epoch = job.epoch;
                    m_prog.viewEpoch = job.viewEpoch;
                    m_prog.renderParamGen = job.paramGen;
                }

                // HQ chunk renders [startLine, endLine) against the GPU's CURRENT parameter buffer.
                // renderSceneComputeOnly does NOT re-upload parameters, so the stamp is the live
                // gpu generation — which may differ from the generation that seeded the buffer.
                size_t const endLine =
                  std::min(job.startLine + job.stepSize, static_cast<size_t>(job.height));
                LineStamp const hqStamp{static_cast<int64_t>(job.viewEpoch),
                                        static_cast<int64_t>(m_gpuParamGen)};
                for (size_t y = job.startLine; y < endLine; ++y)
                {
                    m_prog.lines[y] = hqStamp;
                }

                Result r{};
                r.present = true;
                r.epoch = job.epoch;
                r.viewEpoch = job.viewEpoch;
                r.width = job.width;
                r.height = job.height;
                r.completedLine = endLine;
                r.completedFrame = endLine >= static_cast<size_t>(job.height);

                if (r.completedFrame)
                {
                    m_ready = m_prog;
                    m_ready.epoch = job.epoch;
                    m_ready.viewEpoch = job.viewEpoch;
                    m_prog.clear();
                }
                m_pendingResult = r;
            }

            void processResults()
            {
                m_returnedEarlyThisTick = false;
                if (!m_pendingResult.present)
                {
                    return;
                }
                Result const r = m_pendingResult;
                m_pendingResult = Result{};

                bool const isOutdated = r.epoch < m_sceneEpoch;
                bool const isViewOutdated = r.viewEpoch < m_viewEpoch;
                bool const isSizeOutdated = r.width != m_resultWidth || r.height != m_resultHeight;
                bool const discardForStaleView = isSizeOutdated || isViewOutdated;

                if (r.completedFrame)
                {
                    if (!(isOutdated || discardForStaleView))
                    {
                        m_front = m_ready;
                        m_frontPresented = true;
                    }
                    m_ready.clear();
                    if (!isOutdated && !discardForStaleView)
                    {
                        m_isRendering = false;
                        m_currentLine = 0;
                        m_suppressHQDisplay = false;
                    }
                }
                else
                {
                    if (!isOutdated && !isViewOutdated && !isSizeOutdated)
                    {
                        m_currentLine = std::min(r.completedLine, static_cast<size_t>(r.height));
                    }
                    if ((isViewOutdated || isSizeOutdated) && m_prog.valid &&
                        m_prog.viewEpoch == r.viewEpoch)
                    {
                        m_prog.clear();
                        m_isRendering = false;
                    }
                }

                if (r.epoch == m_inFlightEpoch)
                {
                    m_jobInFlight = false;
                    m_inFlightEpoch = 0;
                    if (isOutdated || isViewOutdated || isSizeOutdated)
                    {
                        m_isRendering = false;
                        m_dirty = true;
                    }
                }
            }

            void resizeBarrier()
            {
                bool const resizeRequired =
                  m_desiredWidth != 0 &&
                  (m_desiredWidth != m_resultWidth || m_desiredHeight != m_resultHeight);
                if (!resizeRequired)
                {
                    return;
                }
                if (m_jobInFlight)
                {
                    m_dirty = true;
                    m_isRendering = false;
                    m_returnedEarlyThisTick = true;
                    return;
                }
                if (m_prog.valid)
                {
                    m_prog.clear();
                    m_currentLine = 0;
                    m_isRendering = false;
                }
                m_resultWidth = m_desiredWidth;
                m_resultHeight = m_desiredHeight;
                ++m_sceneEpoch; // invalidateView -> notifyAsyncEpochIncrement bumps BOTH epochs
                ++m_viewEpoch;
                m_currentLine = 0;
                m_isRendering = false;
                m_dirty = true;
                m_desiredWidth = 0;
                m_desiredHeight = 0;
            }

            DisplayObservation observeDisplay()
            {
                DisplayFrameBufferState const frontState{
                  .hasImage = m_front.valid, .epoch = m_front.epoch, .viewEpoch = m_front.viewEpoch};
                DisplayFrameBufferState const progState{
                  .hasImage = m_prog.valid, .epoch = m_prog.epoch, .viewEpoch = m_prog.viewEpoch};

                std::optional<PresentedFrame> presented;
                if (m_frontPresented)
                {
                    presented = PresentedFrame{};
                    presented->source = FramePresentationSource::ProgressiveHighQuality;
                    presented->quality = FramePresentationQuality::FullQuality;
                }

                DisplayFrameSelectionInput const input{.frontBuffer = frontState,
                                                       .progressiveBuffer = progState,
                                                       .currentEpoch = m_sceneEpoch,
                                                       .currentViewEpoch = m_viewEpoch,
                                                       .exactRealtimeInteraction = false,
                                                       .exactRealtimeJobInFlight = false,
                                                       .isRendering = m_isRendering,
                                                       .isMoving = false,
                                                       .fullFrameRenderJobInFlight = false,
                                                       .suppressHqDisplay = m_suppressHQDisplay,
                                                       .resultImageAvailable = true,
                                                       .presentedFrame = presented};

                DisplayObservation obs{};
                obs.source = selectDisplayFrameSource(input);

                MockImage const * shown = nullptr;
                if (obs.source == DisplayFrameSource::FrontBuffer)
                {
                    shown = &m_front;
                }
                else if (obs.source == DisplayFrameSource::ProgressiveBuffer)
                {
                    shown = &m_prog;
                }

                if (shown != nullptr && shown->valid)
                {
                    obs.shown = distinctStamps(*shown);
                    obs.torn = obs.shown.size() > 1 || hasGarbage(*shown);
                }
                else
                {
                    obs.shown = {m_resultStamp}; // result image is a single uniform low-res frame
                }
                return obs;
            }

            Policy m_policy;
            uint32_t m_resultWidth{0};
            uint32_t m_resultHeight{0};
            size_t m_chunkLines{0};

            uint64_t m_sceneEpoch{1};
            uint64_t m_viewEpoch{1};
            uint64_t m_gpuParamGen{1};     ///< params currently uploaded to *m_primitives
            uint64_t m_pendingParamGen{1}; ///< params requested but not yet uploaded
            size_t m_currentLine{0};
            bool m_isRendering{false};
            bool m_dirty{false};
            bool m_suppressHQDisplay{false};
            bool m_returnedEarlyThisTick{false};

            LineStamp m_resultStamp{1, 1};
            uint64_t m_lastLowResViewEpoch{1};

            MockImage m_prog{};
            MockImage m_front{};
            MockImage m_ready{};
            bool m_frontPresented{false};

            bool m_jobInFlight{false};
            uint64_t m_inFlightEpoch{0};
            Job m_pendingJob{};
            Result m_pendingResult{};

            uint32_t m_desiredWidth{0};
            uint32_t m_desiredHeight{0};
        };

        std::string stampsToString(std::set<LineStamp> const & stamps)
        {
            std::ostringstream oss;
            oss << "{";
            bool first = true;
            for (auto const & s : stamps)
            {
                if (!first)
                {
                    oss << " ";
                }
                oss << "v" << s.viewEpoch << "p" << s.paramGen;
                first = false;
            }
            oss << "}";
            return oss.str();
        }

        char const * sourceName(DisplayFrameSource s)
        {
            switch (s)
            {
            case DisplayFrameSource::FrontBuffer:
                return "Front";
            case DisplayFrameSource::ProgressiveBuffer:
                return "Progressive";
            case DisplayFrameSource::ResultImage:
                return "Result";
            default:
                return "None";
            }
        }

        /// Runs the param-change-then-resize scenario with an async parameter upload that lands
        /// mid-fill, and returns true if ANY displayed frame is torn (mixed model state on screen).
        bool runParamUploadMidFillScenario(Policy policy, bool verbose, bool bumpViewOnUpload = false)
        {
            RenderWindowSim sim(policy, /*w*/ 100, /*h*/ 100, /*chunk*/ 20);

            // 1) Reach a clean steady state at view 1 / param 1.
            sim.lowResPreviewArrives();
            for (int i = 0; i < 12; ++i)
            {
                sim.tick();
            }

            // 2) Change a model parameter: bumps the view epoch and REQUESTS a new param gen, but
            //    the GPU parameter buffer is not uploaded yet.
            sim.parameterChange();

            // 3) Immediately resize: forces a fresh full HQ fill against the still-stale GPU params.
            sim.requestResize(140, 140);

            bool tornEverSeen = false;
            int const uploadAtTick = 3; // the async parameter upload lands a few frames into the fill
            for (int t = 0; t < 16; ++t)
            {
                if (t == uploadAtTick)
                {
                    sim.deliverParameterUpload(bumpViewOnUpload); // GPU params change mid-fill
                    sim.lowResPreviewArrives();                    // a fresh low-res preview arrives
                }
                DisplayObservation const obs = sim.tick();
                tornEverSeen = tornEverSeen || obs.torn;
                if (verbose)
                {
                    std::cout << "  t" << t << " src=" << sourceName(obs.source)
                              << " shown=" << stampsToString(obs.shown)
                              << (obs.torn ? "  <<< TORN" : "") << "\n";
                }
            }
            return tornEverSeen;
        }
    } // namespace

    /// Observation harness: print the per-tick display trace for all three policies so the real
    /// behavior is visible. The parameter upload lands mid-fill (decoupled from the view epoch).
    TEST(ProgressiveTearReproduction, Observe_ParamUploadMidFill_AllPolicies)
    {
        for (Policy policy : {Policy::SceneEpochOnly, Policy::SceneAndView, Policy::SceneViewAndParam})
        {
            std::cout << "\n=== " << policyName(policy) << " ===\n";
            bool const torn = runParamUploadMidFillScenario(policy, /*verbose*/ true);
            std::cout << "  => tornEverSeen=" << (torn ? "true" : "false") << "\n";
        }
    }

    /// The committed view-epoch fix is INSUFFICIENT for a parameter upload that lands mid-fill:
    /// the whole fill carries a uniform view epoch, so the epoch guards pass while the buffer
    /// holds two different parameter generations -> a torn frame is displayed.
    TEST(ProgressiveTearReproduction, ParamUploadMidFill_SceneAndView_StillTears)
    {
        EXPECT_TRUE(runParamUploadMidFillScenario(Policy::SceneAndView, /*verbose*/ false))
          << "Expected the committed (view-epoch only) fix to STILL show a torn frame when a "
             "parameter upload lands mid-fill — the tear is in the parameter dimension, which the "
             "view epoch does not capture.";
    }

    /// Tying buffer continuation to the GPU parameter-upload generation restarts the fill when an
    /// upload lands mid-fill, so the displayed frame is never torn.
    TEST(ProgressiveTearReproduction, ParamUploadMidFill_SceneViewAndParam_NeverTears)
    {
        EXPECT_FALSE(runParamUploadMidFillScenario(Policy::SceneViewAndParam, /*verbose*/ false))
          << "A parameter-generation-aware continuation predicate should restart the fill on a "
             "mid-fill upload so no torn frame is ever displayed.";
    }

    /// Minimal candidate fix reusing the EXISTING view-epoch machinery (the committed fix): bump
    /// the view epoch when the parameter upload actually completes, instead of only at request
    /// time. The in-flight fill that started against the old parameters then becomes view-outdated
    /// and restarts, so the completed frame is param-consistent and never torn.
    TEST(ProgressiveTearReproduction, ParamUploadMidFill_SceneAndView_BumpViewOnUpload_NeverTears)
    {
        EXPECT_FALSE(runParamUploadMidFillScenario(
          Policy::SceneAndView, /*verbose*/ false, /*bumpViewOnUpload*/ true))
          << "Bumping the view epoch when the parameter upload lands should restart the in-flight "
             "fill via the existing view-outdated machinery, eliminating the parameter tear.";
    }

    // ---------------------------------------------------------------------------------------------
    // Reproduction of the SECOND reported regression: "after resizing the HQ rendering stays at 0%".
    //
    // hqProgressiveRenderProgress() (RenderWindow.cpp) reports the HQ progress bar as:
    //   active   = m_renderWindowState.isRendering && !m_renderWindowState.isMoving && height > 0
    //   fraction = clamp(currentLine / height)
    // So the bar shows a STUCK 0% whenever, after a resize, one of these holds persistently:
    //   (A) isRendering never flips back to true (HQ never re-scheduled),           OR
    //   (B) isMoving never clears (HQ reporting permanently gated off),              OR
    //   (C) currentLine is pinned at 0 by a perpetual re-seed loop (continuation
    //       predicate keeps failing, so every chunk restarts at line 0).
    //
    // This second simulation models the renderAsync ordering around a resize, the invalidateView()
    // side effects, the low-res settle that is supposed to flip isRendering back on, and the NEW
    // parameter-generation continuation key (m_asyncProgressiveParamGeneration) — including the fact
    // that the resize barrier resets the scene/view epoch but does NOT reset the parameter
    // generation stamp (the omission at RenderWindow.cpp L2176/2786/2912/2929/407). The parameter
    // generation stamp is modelled as a SEPARATE value from the buffer (mirroring the real atomic),
    // so it can drift out of sync with the buffer it is supposed to describe.
    namespace
    {
        /// Snapshot of hqProgressiveRenderProgress() for a given tick.
        struct HqProgress
        {
            bool active{false};
            double fraction{0.0};
            size_t currentLine{0};
            size_t height{0};
        };

        /// Why HQ could not resume — used purely to make failing reproductions self-describing.
        enum class HqStallReason
        {
            None,
            NotRendering,  ///< isRendering == false (HQ never re-scheduled)
            Moving,        ///< isMoving == true (progress reporting gated off)
            ReseedLoop     ///< currentLine pinned at 0 by repeated continuation failure
        };

        char const * stallReasonName(HqStallReason r)
        {
            switch (r)
            {
            case HqStallReason::None:
                return "None";
            case HqStallReason::NotRendering:
                return "NotRendering";
            case HqStallReason::Moving:
                return "Moving(isMoving stuck true)";
            case HqStallReason::ReseedLoop:
                return "ReseedLoop(currentLine pinned at 0)";
            }
            return "?";
        }

        /// GPU-free model of RenderWindow's resize -> low-res settle -> progressive-HQ resume path,
        /// with the parameter-generation continuation key. One-frame worker latency: a chunk
        /// scheduled in tick N produces its result at the start of tick N+1.
        class ResizeHqProgressSim
        {
        public:
            ResizeHqProgressSim(uint32_t width, uint32_t height, size_t chunkLines)
                : m_width(width)
                , m_height(height)
                , m_chunkLines(chunkLines)
            {
                // Arm the first low-res settle so the warm-up reaches a completed HQ frame.
                m_forceLowRes = true;
                m_dirty = true;
            }

            /// Reset the observation latches AFTER the warm-up so the trace only reflects what
            /// happens from the resize onward (frontPresented() is otherwise sticky).
            void beginObservation()
            {
                m_frontPresented = false;
            }

            /// If true, the resize barrier ALSO resets the parameter-generation stamp (the candidate
            /// fix). Default false models the SHIPPED code, where the stamp is left stale on resize.
            void setResetParamGenOnResize(bool enable)
            {
                m_resetParamGenOnResize = enable;
            }

            /// Number of upcoming UI-side seeds that should fail (models tryGetBestRenderProgram()
            /// returning nullopt while the optimized render program is still compiling). A failed
            /// seed leaves the buffer unseeded AND does not store the parameter-generation stamp.
            void setSeedFailures(int count)
            {
                m_seedFailuresRemaining = count;
            }

            /// Models the static settle never scheduling HQ again (e.g. an orphaned in-flight task
            /// in RenderUpdateCoordinator), so the low-res settle never flips isRendering back on.
            void setSettleNeverResumesHq(bool enable)
            {
                m_settleNeverResumesHq = enable;
            }

            /// invalidateViewDueToParameterChange(): bumps the view epoch and REQUESTS a new
            /// parameter generation. The GPU parameter buffer is uploaded later (deliverParamUpload).
            void parameterChange()
            {
                ++m_viewEpoch;
                ++m_pendingParamGen;
                invalidateView();
            }

            /// The async parameter upload reaches *m_primitives; updateParameterBlocking increments
            /// the parameter generation (only on an ACTUAL change).
            void deliverParamUpload()
            {
                if (m_gpuParamGen != m_pendingParamGen)
                {
                    m_gpuParamGen = m_pendingParamGen;
                }
            }

            void requestResize(uint32_t w, uint32_t h)
            {
                m_desiredWidth = w;
                m_desiredHeight = h;
            }

            HqProgress tick()
            {
                updateMovingFromCamera(); // render() updates isMoving every frame BEFORE renderAsync
                processResults();
                if (resizeBarrier())
                {
                    return hqProgress(); // deferred-resize early return mirrors renderAsync
                }
                lowResSettle();
                scheduleChunkIfNeeded();
                runWorker();
                return hqProgress();
            }

            HqProgress hqProgress() const
            {
                HqProgress p{};
                p.height = m_height;
                p.currentLine = m_currentLine;
                if (!m_isRendering || m_isMoving || m_height == 0)
                {
                    return p;
                }
                p.active = true;
                p.fraction = std::min(1.0,
                                      static_cast<double>(m_currentLine) /
                                        static_cast<double>(m_height));
                return p;
            }

            bool frontPresented() const
            {
                return m_frontPresented;
            }

        private:
            void invalidateView()
            {
                m_dirty = true;
                m_isMoving = true; // invalidateView() sets isMoving = true (RenderWindow.cpp L1241)
                m_cameraIdleFrames = 0;
                m_currentLine = 0;
                m_isRendering = false;
                m_forceLowRes = true;
                // invalidateView() disarms HQ: the progressive fill is only re-scheduled after the
                // forced low-res preview runs and the static catch-up re-arms it. This mirrors the
                // RenderUpdateCoordinator gate — HQ chunks are not emitted from a bare dirty flag.
                m_hqArmed = false;
            }

            /// Mirrors RenderWindow.cpp L1100-1104: every frame, with no actual camera motion, the
            /// idle counter advances and isMoving stays true only for a short debounce window.
            void updateMovingFromCamera()
            {
                ++m_cameraIdleFrames;
                m_isMoving = m_cameraIdleFrames < kMovingDebounceFrames;
            }

            ProgressiveBufferState bufferState() const
            {
                return ProgressiveBufferState{.hasImage = m_prog.valid,
                                              .epoch = m_prog.epoch,
                                              .viewEpoch = m_prog.viewEpoch,
                                              .paramGeneration = m_progParamGenStamp,
                                              .width = m_prog.width,
                                              .height = m_prog.height};
            }

            bool continuable(Job const & job) const
            {
                return isProgressiveBufferContinuable(
                  bufferState(),
                  ProgressiveJobTarget{.epoch = job.epoch,
                                       .viewEpoch = job.viewEpoch,
                                       .paramGeneration = job.paramGen,
                                       .width = job.width,
                                       .height = job.height});
            }

            /// The low-res preview that invalidateView() forced. In the real pipeline this preview
            /// (and the static catch-up it triggers) is what flips isRendering back on so the
            /// progressive HQ fill resumes. If the static settle is broken, HQ never resumes.
            void lowResSettle()
            {
                if (!m_forceLowRes || m_jobInFlight)
                {
                    return;
                }
                m_forceLowRes = false;
                m_resultStamp = LineStamp{static_cast<int64_t>(m_viewEpoch),
                                          static_cast<int64_t>(m_gpuParamGen)};
                m_lastLowResViewEpoch = m_viewEpoch;
                if (!m_settleNeverResumesHq)
                {
                    m_hqArmed = true; // static catch-up arms the progressive HQ fill
                }
            }

            void scheduleChunkIfNeeded()
            {
                if (m_jobInFlight || m_isMoving)
                {
                    return;
                }
                // HQ chunks are only emitted while the fill is armed by the static catch-up.
                // A bare dirty flag is NOT enough — that models the coordinator gate so an
                // orphaned in-flight task (settle never re-arms) keeps HQ from ever scheduling.
                if (!m_hqArmed)
                {
                    return;
                }

                Job job{};
                job.active = true;
                job.epoch = m_sceneEpoch;
                job.viewEpoch = m_viewEpoch;
                job.paramGen = m_gpuParamGen;
                job.width = m_width;
                job.height = m_height;

                bool const seeded = continuable(job);
                if (m_currentLine > 0 && !seeded)
                {
                    m_currentLine = 0;
                }
                job.startLine = std::min(m_currentLine, static_cast<size_t>(job.height));
                job.stepSize = std::max<size_t>(
                  1, std::min(m_chunkLines, static_cast<size_t>(job.height) - job.startLine));

                if (job.startLine == 0 && !seeded)
                {
                    bool const seedFails = m_seedFailuresRemaining > 0;
                    if (seedFails)
                    {
                        --m_seedFailuresRemaining;
                        // Seed failed (no render program): buffer NOT seeded, stamp NOT stored.
                        // isRendering is still set below, so the bar is active but currentLine
                        // cannot advance until a seed finally succeeds.
                    }
                    else
                    {
                        m_prog.allocate(job.width, job.height, m_resultStamp);
                        m_prog.epoch = job.epoch;
                        m_prog.viewEpoch = job.viewEpoch;
                        m_progParamGenStamp = job.paramGen; // seed stores stamp ONLY on success
                    }
                }

                m_dirty = false;
                m_isRendering = true;
                m_jobInFlight = true;
                m_inFlightEpoch = job.epoch;
                m_pendingJob = job;
            }

            void runWorker()
            {
                if (!m_pendingJob.active)
                {
                    return;
                }
                Job const job = m_pendingJob;
                m_pendingJob = Job{};

                if (m_seedFailuresRemaining > 0 && !m_prog.valid)
                {
                    // Render program still unavailable: the worker cannot acquire/seed a buffer
                    // either, so this chunk renders nothing. isRendering stays true (bar active),
                    // but currentLine is pinned at 0 — reproduction of 'active but stuck at 0%'.
                    Result none{};
                    none.present = true;
                    none.epoch = job.epoch;
                    none.viewEpoch = job.viewEpoch;
                    none.width = job.width;
                    none.height = job.height;
                    none.completedLine = job.startLine; // no progress
                    none.completedFrame = false;
                    m_pendingResult = none;
                    return;
                }

                if (!continuable(job))
                {
                    // Worker acquire-fresh branch re-stamps the parameter generation.
                    m_prog.allocate(job.width, job.height, LineStamp{});
                    m_prog.epoch = job.epoch;
                    m_prog.viewEpoch = job.viewEpoch;
                    m_progParamGenStamp = job.paramGen;
                }
                else if (!m_prog.valid)
                {
                    // UI seed failed but predicate says "continuable" because the stale stamp still
                    // matches: there is no buffer to render into, so this chunk produces nothing.
                    Result none{};
                    none.present = true;
                    none.epoch = job.epoch;
                    none.viewEpoch = job.viewEpoch;
                    none.width = job.width;
                    none.height = job.height;
                    none.completedLine = job.startLine; // no progress
                    none.completedFrame = false;
                    m_pendingResult = none;
                    return;
                }

                size_t const endLine =
                  std::min(job.startLine + job.stepSize, static_cast<size_t>(job.height));
                LineStamp const hqStamp{static_cast<int64_t>(job.viewEpoch),
                                        static_cast<int64_t>(m_gpuParamGen)};
                for (size_t y = job.startLine; y < endLine && y < m_prog.lines.size(); ++y)
                {
                    m_prog.lines[y] = hqStamp;
                }

                Result r{};
                r.present = true;
                r.epoch = job.epoch;
                r.viewEpoch = job.viewEpoch;
                r.width = job.width;
                r.height = job.height;
                r.completedLine = endLine;
                r.completedFrame = endLine >= static_cast<size_t>(job.height);
                if (r.completedFrame)
                {
                    m_ready = m_prog;
                    m_prog.clear();
                }
                m_pendingResult = r;
            }

            void processResults()
            {
                if (!m_pendingResult.present)
                {
                    return;
                }
                Result const r = m_pendingResult;
                m_pendingResult = Result{};

                bool const isOutdated = r.epoch < m_sceneEpoch;
                bool const isViewOutdated = r.viewEpoch < m_viewEpoch;
                bool const isSizeOutdated = r.width != m_width || r.height != m_height;
                bool const discard = isOutdated || isViewOutdated || isSizeOutdated;

                if (r.completedFrame)
                {
                    if (!discard)
                    {
                        m_front = m_ready;
                        m_frontPresented = true;
                        m_isRendering = false;
                        m_currentLine = 0;
                        m_hqArmed = false; // a completed frame disarms until the next settle
                    }
                    m_ready.clear();
                }
                else if (!discard)
                {
                    m_currentLine = std::min(r.completedLine, static_cast<size_t>(r.height));
                }

                if (r.epoch == m_inFlightEpoch)
                {
                    m_jobInFlight = false;
                    m_inFlightEpoch = 0;
                    if (discard)
                    {
                        m_isRendering = false;
                        m_dirty = true;
                    }
                }
            }

            /// Returns true if a deferred-resize early-return happened this tick.
            bool resizeBarrier()
            {
                bool const resizeRequired =
                  m_desiredWidth != 0 &&
                  (m_desiredWidth != m_width || m_desiredHeight != m_height);
                if (!resizeRequired)
                {
                    return false;
                }
                if (m_jobInFlight)
                {
                    // HQ render in flight: defer, drop to low-res, and early-return.
                    m_dirty = true;
                    m_forceLowRes = true;
                    m_isRendering = false;
                    return true;
                }
                if (m_prog.valid)
                {
                    m_prog.clear();
                }
                m_width = m_desiredWidth;
                m_height = m_desiredHeight;
                m_desiredWidth = 0;
                m_desiredHeight = 0;
                // Resize releases the buffer and resets scene/view epoch, but the parameter
                // generation stamp is left stale unless the candidate fix is enabled.
                ++m_sceneEpoch;
                ++m_viewEpoch;
                if (m_resetParamGenOnResize)
                {
                    m_progParamGenStamp = 0;
                }
                invalidateView();
                return false;
            }

            uint32_t m_width{0};
            uint32_t m_height{0};
            size_t m_chunkLines{0};

            static constexpr int kMovingDebounceFrames = 3;

            uint64_t m_sceneEpoch{1};
            uint64_t m_viewEpoch{1};
            uint64_t m_gpuParamGen{1};
            uint64_t m_pendingParamGen{1};
            uint64_t m_progParamGenStamp{0}; ///< models the SEPARATE m_asyncProgressiveParamGeneration

            size_t m_currentLine{0};
            bool m_isRendering{false};
            bool m_isMoving{false};
            bool m_dirty{false};
            bool m_forceLowRes{false};
            int m_cameraIdleFrames{kMovingDebounceFrames};

            bool m_resetParamGenOnResize{false};
            int m_seedFailuresRemaining{0};
            bool m_settleNeverResumesHq{false};
            bool m_hqArmed{false};

            LineStamp m_resultStamp{1, 1};
            uint64_t m_lastLowResViewEpoch{1};

            MockImage m_prog{};
            MockImage m_front{};
            MockImage m_ready{};
            bool m_frontPresented{false};

            bool m_jobInFlight{false};
            uint64_t m_inFlightEpoch{0};
            Job m_pendingJob{};
            Result m_pendingResult{};

            uint32_t m_desiredWidth{0};
            uint32_t m_desiredHeight{0};
        };

        /// Summary of an HQ-progress run: peak fraction reached and whether HQ ever completed.
        struct HqRunSummary
        {
            double maxFraction{0.0};
            bool everActive{false};
            bool completed{false};
            HqStallReason stallReason{HqStallReason::None};
            int ticksStuckAtZero{0}; ///< consecutive trailing ticks with fraction==0 (active or not)
        };

        /// Drive the sim forward, recording the HQ-progress trace. `setup` runs against the sim
        /// BEFORE the observation loop (e.g. to request a resize / change a parameter).
        template <typename SetupFn>
        HqRunSummary observeHqAfter(ResizeHqProgressSim & sim,
                                    int ticks,
                                    bool verbose,
                                    SetupFn && setup)
        {
            // Warm up to a clean steady state with a completed HQ frame.
            for (int i = 0; i < 12; ++i)
            {
                sim.tick();
            }
            sim.tick(); // one extra so any pending result is consumed

            sim.beginObservation(); // forget the warm-up's completed frame; observe from here on
            setup(sim);

            HqRunSummary s{};
            int trailingZero = 0;
            for (int t = 0; t < ticks; ++t)
            {
                HqProgress const p = sim.tick();
                s.maxFraction = std::max(s.maxFraction, p.fraction);
                s.everActive = s.everActive || p.active;
                s.completed = s.completed || sim.frontPresented();
                if (p.fraction == 0.0)
                {
                    ++trailingZero;
                }
                else
                {
                    trailingZero = 0;
                }
                if (verbose)
                {
                    std::cout << "  t" << t << " active=" << (p.active ? "1" : "0")
                              << " frac=" << p.fraction << " line=" << p.currentLine << "/"
                              << p.height << (sim.frontPresented() ? " [HQ done]" : "") << "\n";
                }
            }
            s.ticksStuckAtZero = trailingZero;

            if (!s.completed)
            {
                if (!s.everActive)
                {
                    s.stallReason = HqStallReason::NotRendering;
                }
                else if (s.maxFraction == 0.0)
                {
                    s.stallReason = HqStallReason::ReseedLoop;
                }
            }
            return s;
        }
    } // namespace

    /// Control: a plain resize (no parameter change, render program available) must let the HQ
    /// progressive fill resume and run to completion — progress climbs above 0 and reaches 100%.
    TEST(ResizeHqProgress, PlainResize_HqResumesAndCompletes)
    {
        ResizeHqProgressSim sim(/*w*/ 100, /*h*/ 100, /*chunk*/ 25);
        HqRunSummary const s = observeHqAfter(
          sim, /*ticks*/ 40, /*verbose*/ false, [](ResizeHqProgressSim & x) { x.requestResize(140, 140); });

        EXPECT_TRUE(s.completed) << "Plain resize should let HQ run to completion (front presented).";
        EXPECT_GT(s.maxFraction, 0.0) << "HQ progress must climb above 0% after a plain resize.";
    }

    /// Observation harness — prints the HQ-progress trace for the resize-then-parameter-upload
    /// case so the stuck-at-0% behaviour is visible in the test log.
    TEST(ResizeHqProgress, Observe_ParamChangeThenResize_Trace)
    {
        std::cout << "\n=== ParamChange -> Resize -> mid-fill param upload (shipped, stamp NOT reset) ===\n";
        ResizeHqProgressSim sim(/*w*/ 100, /*h*/ 100, /*chunk*/ 20);
        HqRunSummary const s = observeHqAfter(
          sim, /*ticks*/ 24, /*verbose*/ true,
          [](ResizeHqProgressSim & x)
          {
              x.parameterChange();
              x.requestResize(140, 140);
          });
        std::cout << "  => maxFraction=" << s.maxFraction << " completed=" << (s.completed ? "1" : "0")
                  << " stall=" << stallReasonName(s.stallReason) << "\n";
    }

    /// REPRODUCTION: if the optimized render program never becomes available again after the
    /// resize (tryGetBestRenderProgram() keeps returning nullopt), neither the UI seed nor the
    /// worker can acquire a buffer to render into. isRendering is set (the bar is ACTIVE), but
    /// currentLine cannot advance, so the HQ bar sits at 0% indefinitely — a direct reproduction
    /// of 'after resizing the HQ rendering stays at 0%'.
    TEST(ResizeHqProgress, RenderProgramUnavailableAfterResize_BarActiveButStuckAtZero)
    {
        ResizeHqProgressSim sim(/*w*/ 100, /*h*/ 100, /*chunk*/ 20);
        sim.setSeedFailures(1000); // render program never becomes available during the window
        HqRunSummary const s = observeHqAfter(
          sim, /*ticks*/ 30, /*verbose*/ false, [](ResizeHqProgressSim & x) { x.requestResize(140, 140); });

        EXPECT_FALSE(s.completed) << "With the render program unavailable, HQ cannot complete.";
        EXPECT_TRUE(s.everActive)
          << "The HQ bar is ACTIVE (isRendering set) yet makes no progress.";
        EXPECT_EQ(s.maxFraction, 0.0)
          << "HQ progress is pinned at 0% — reproduction of the 'stays at 0% after resize' report.";
        EXPECT_GT(s.ticksStuckAtZero, 0) << "The 0% state must persist (not a one-frame blip).";
    }

    /// REPRODUCTION: if the static settle never re-arms HQ after the resize (orphaned in-flight
    /// coordinator task class), isRendering never returns to true, so the HQ bar reports inactive
    /// forever — another way the bar 'stays at 0%'.
    TEST(ResizeHqProgress, SettleNeverResumesHq_StuckInactive)
    {
        ResizeHqProgressSim sim(/*w*/ 100, /*h*/ 100, /*chunk*/ 25);
        sim.setSettleNeverResumesHq(true);
        HqRunSummary const s = observeHqAfter(
          sim, /*ticks*/ 30, /*verbose*/ false, [](ResizeHqProgressSim & x) { x.requestResize(140, 140); });

        EXPECT_FALSE(s.completed) << "HQ never completes if the settle never re-arms it.";
        EXPECT_FALSE(s.everActive)
          << "The HQ bar never becomes active — reproduction of the perpetual 0% after resize.";
    }
}
