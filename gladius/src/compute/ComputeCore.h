#pragma once

#include <BitmapLayer.h>
#include <ContourExtractor.h>
#include <EventLogger.h>
#include <GLImageBuffer.h>
#include <ImageRGBA.h>
#include <MeshVoxelGridManager.h>
#include <ModelState.h>
#include <RenderProgram.h>
#include <ResourceContext.h>
#include <SlicerProgram.h>
#include <compute/Rendering.h>
#include <compute/types.h>
#include <kernel/types.h>
#include <nodes/BuildParameter.h>
#include <nodes/Model.h>
#include <nodes/nodesfwd.h>
#include <ui/OrbitalCamera.h>

#include <compute/ParameterSignature.h>
#include <compute/ProgramManager.h>
#include <compute/RenderSceneGeneration.h>
#include <compute/RenderSession.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace cl
{
    class CommandQueue;
}

namespace gladius
{
    // Forward declarations of shared pointer types
    using SharedGLImageBuffer = std::shared_ptr<GLImageBuffer>;
    using SharedContourExtractor = std::shared_ptr<ContourExtractor>;
    using SharedSlicerProgram = std::shared_ptr<SlicerProgram>;
    using SharedRenderProgram = std::shared_ptr<RenderProgram>;
    using SharedPrimitives = std::shared_ptr<Primitives>;

    enum class LowResPreviewRenderStatus
    {
        Rendered,
        Skipped,
        Failed
    };

    /**
     * @brief Provenance of a cached legacy OpenCL bounding-box result.
     *
     * This migration-only classification keeps existing bool-returning callers intact while
     * allowing the backend-neutral bounds adapter to reject the historical build-volume sentinel.
     */
    enum class BoundingBoxComputationSource
    {
      None,
      NumericalProbe,
      PrimitiveMetadata,
      BuildVolumeFallback,
    };

    /**
     * @brief Wrapper class for ContourExtractor to maintain backward compatibility
     * with code that expects a reference to ContourExtractor.
     */
    class ContourExtractorWrapper
    {
      public:
        explicit ContourExtractorWrapper(SharedContourExtractor contourExtractor)
            : m_contourExtractor(std::move(contourExtractor))
        {
        }

        // Forward methods to the underlying ContourExtractor
        auto & getContour()
        { return m_contourExtractor->getContour(); }
        const auto & getContour() const
        { return m_contourExtractor->getContour(); }
        auto & getOpenContours()
        { return m_contourExtractor->getOpenContours(); }
        auto & getNormals()
        { return m_contourExtractor->getNormals(); }
        const auto & getNormals() const
        { return m_contourExtractor->getNormals(); }
        auto & getSourceVertices()
        { return m_contourExtractor->getSourceVertices(); }
        const auto & getSourceVertices() const
        { return m_contourExtractor->getSourceVertices(); }
        const auto & getSliceQuality() const
        { return m_contourExtractor->getSliceQuality(); }
        void clear()
        { m_contourExtractor->clear(); }
        void setSimplificationTolerance(float tol)
        { m_contourExtractor->setSimplificationTolerance(tol); }
        void addIsoLineFromMarchingSquare(MarchingSquaresStates & states,
                                          float4 const & clippingArea)
        {
            // Use non-const reference per ContourExtractor::addIsoLineFromMarchingSquare signature
            m_contourExtractor->addIsoLineFromMarchingSquare(states, clippingArea);
        }
        void runPostProcessing()
        { m_contourExtractor->runPostProcessing(); }
        void calcAreas()
        { m_contourExtractor->calcAreas(); }
        void calcSign()
        { m_contourExtractor->calcSign(); }
        PolyLines generateOffsetContours(float offset, const PolyLines & contours) const
        { return m_contourExtractor->generateOffsetContours(offset, contours); }
        // Simple version for backward compatibility
        PolyLines generateOffsetContours(float offset) const
        {
            return m_contourExtractor->generateOffsetContours(offset,
                                                              m_contourExtractor->getContour());
        }

        // Allow direct access to the underlying shared_ptr when needed
        SharedContourExtractor getSharedPtr() const
        { return m_contourExtractor; }

        // Allow using the wrapper as a ContourExtractor reference
        operator ContourExtractor &()
        { return *m_contourExtractor; }
        operator const ContourExtractor &() const
        { return *m_contourExtractor; }

      private:
        SharedContourExtractor m_contourExtractor;
    };

    /**
     * @brief Wrapper class for ResourceContext to maintain backward compatibility
     * with code that expects a reference to ResourceContext.
     */
    class ResourceContextWrapper
    {
      public:
        explicit ResourceContextWrapper(SharedResources resources)
            : m_resources(std::move(resources))
        {
        } // Forward methods to the underlying ResourceContext
        auto & getRenderingSettings()
        { return m_resources->getRenderingSettings(); }
        auto & getParameterBuffer()
        { return m_resources->getParameterBuffer(); }
        auto & getCommandBuffer()
        { return m_resources->getCommandBuffer(); }
        auto & getPrecompSdfBuffer()
        { return m_resources->getPrecompSdfBuffer(); }
        auto getClippingArea() const
        { return m_resources->getClippingArea(); }
        void setClippingArea(cl_float4 area, float padding = 0.0f)
        { m_resources->setClippingArea(area, padding); }
        MarchingSquaresStates & getMarchingSquareStates()
        { return m_resources->getMarchingSquareStates(); }
        void requestSliceBuffer()
        { m_resources->requestSliceBuffer(); }
        void requestDistanceMaps()
        { m_resources->requestDistanceMaps(); }
        DistanceMipMaps & getDistanceMipMaps()
        { return m_resources->getDistanceMipMaps(); }
        auto getEyePosition() const
        { return m_resources->getEyePosition(); }
        void setEyePosition(cl_float4 position)
        { m_resources->setEyePosition(position); }
        auto getModelViewPerspectiveMat() const
        { return m_resources->getModelViewPerspectiveMat(); }
        void setModelViewPerspectiveMat(cl_float16 mat)
        { m_resources->setModelViewPerspectiveMat(mat); }
        auto & getConvexHullVertices()
        { return m_resources->getConvexHullVertices(); }
        auto & getConvexHullInitialVertices()
        { return m_resources->getConvexHullInitialVertices(); }
        void initConvexHullVertices()
        { m_resources->initConvexHullVertices(); }
        void allocatePreComputedSdf(size_t width = 0, size_t height = 0, size_t depth = 0)
        { m_resources->allocatePreComputedSdf(width, height, depth); }
        void setPreCompSdfBBox(const BoundingBox & box)
        { m_resources->setPreCompSdfBBox(box); }
        void releasePreComputedSdf()
        { m_resources->releasePreComputedSdf(); }
        void clearImageStacks()
        { m_resources->clearImageStacks(); }

        // Allow direct access to the underlying shared_ptr when needed
        SharedResources getSharedPtr() const
        { return m_resources; }

        // Allow using the wrapper as a ResourceContext reference
        operator ResourceContext &()
        { return *m_resources; }
        operator const ResourceContext &() const
        { return *m_resources; }

      private:
        SharedResources m_resources;

        friend class ComputeCore; // Allow ComputeCore to access private members
    };

    /**
     * @brief Wrapper class for ComputeContext to maintain backward compatibility
     * with code that expects a reference to ComputeContext.
     */
    class ComputeContextWrapper
    {
      public:
        explicit ComputeContextWrapper(SharedComputeContext context)
            : m_context(std::move(context))
        {
        } // Forward methods to the underlying ComputeContext
        bool isValid() const
        { return m_context->isValid(); }
        const cl::CommandQueue & GetQueue()
        { return m_context->GetQueue(); }
        OutputMethod outputMethod() const
        { return m_context->outputMethod(); }

        // Allow direct access to the underlying shared_ptr when needed
        SharedComputeContext getSharedPtr() const
        { return m_context; }

        // Allow using the wrapper as a ComputeContext reference
        operator ComputeContext &()
        { return *m_context; }
        operator const ComputeContext &() const
        { return *m_context; }

      private:
        SharedComputeContext m_context;

        friend class ComputeCore; // Allow ComputeCore to access private members
    };

    /**
     * @brief Wrapper class for ModelState to maintain backward compatibility
     * with code that expects a reference to ModelState.
     */
    class ModelStateWrapper
    {
      public:
        explicit ModelStateWrapper(std::shared_ptr<ModelState> modelState)
            : m_modelState(std::move(modelState))
        {
        }

        // Forward methods to the underlying ModelState
        bool isModelUpToDate() const
        { return m_modelState->isModelUpToDate(); }
        void signalCompilationStarted()
        { m_modelState->signalCompilationStarted(); }
        void signalCompilationFinished()
        { m_modelState->signalCompilationFinished(); }

        // Allow direct access to the underlying shared_ptr when needed
        std::shared_ptr<ModelState> getSharedPtr() const
        { return m_modelState; }

        // Allow using the wrapper as a ModelState reference
        operator ModelState &()
        { return *m_modelState; }
        operator const ModelState &() const
        { return *m_modelState; }

      private:
        std::shared_ptr<ModelState> m_modelState;

        friend class ComputeCore; // Allow ComputeCore to access private members
    }; /**
        * @brief Wrapper class for Primitives to maintain backward compatibility
        * with code that expects a reference to Primitives.
        */
    class PrimitivesWrapper
    {
      public:
        explicit PrimitivesWrapper(SharedPrimitives primitives)
            : m_primitives(std::move(primitives))
        {
        }

        // Forward methods to access the underlying Primitives data
        auto & data()
        { return m_primitives->data; }
        const auto & data() const
        { return m_primitives->data; }

        // Allow direct access to the underlying shared_ptr when needed
        SharedPrimitives getSharedPtr() const
        { return m_primitives; }

        // Allow using the wrapper as a Primitives reference
        operator Primitives &()
        { return *m_primitives; }
        operator const Primitives &() const
        { return *m_primitives; }

      private:
        SharedPrimitives m_primitives;

        friend class ComputeCore; // Allow ComputeCore to access private members
    };

    class ComputeCore
    {
      public:
        explicit ComputeCore(SharedComputeContext context,
                             RequiredCapabilities requiredCapabilities,
                             events::SharedLogger logger);

        void init();
        void reset();

        ComputeToken waitForComputeToken();
        OptionalComputeToken requestComputeToken();

        void createBuffer();

        [[nodiscard]] bool renderScene(size_t startLine, size_t endLine);

        /// @brief Render scene to a pure OpenCL image buffer (no GL interop required)
        /// @param commandQueue OpenCL command queue used for dispatch
        /// @param startLine Starting line for rendering
        /// @param endLine Ending line for rendering
        /// @param targetImage Pure CL image buffer to render into (no GL texture)
        /// @return true if rendering succeeded, false otherwise
        /// @note This method is safe to call from worker threads as it doesn't use GL
        [[nodiscard]] bool renderSceneComputeOnly(cl::CommandQueue const & commandQueue,
                                                  size_t startLine,
                                                  size_t endLine,
                                                  ImageRGBA & targetImage,
                                                  cl::Event * completionEvent = nullptr);

        /// @brief Render scene to a pure OpenCL image buffer with caller-provided settings.
        /// @param commandQueue OpenCL command queue used for dispatch
        /// @param startLine Starting line for rendering
        /// @param endLine Ending line for rendering
        /// @param targetImage Pure CL image buffer to render into (no GL texture)
        /// @param settings Rendering settings copy used for this dispatch
        /// @param completionEvent Optional event for async completion tracking
        /// @return true if rendering was enqueued successfully, false otherwise
        /// @thread Any thread with compute context access
        [[nodiscard]] bool
        renderSceneComputeOnlyWithSettings(cl::CommandQueue const & commandQueue,
                                           size_t startLine,
                                           size_t endLine,
                                           ImageRGBA & targetImage,
                                           RenderingSettings settings,
                                           cl::Event * completionEvent = nullptr);

        /// @brief Render the current low-resolution preview when the precomputed SDF is ready.
        /// @return Rendered when a preview frame was produced, Skipped when the SDF is not ready,
        /// or Failed on an execution/precondition error.
        [[nodiscard]] LowResPreviewRenderStatus renderLowResPreview() const;

        /**
         * @brief Starts asynchronous low-resolution preview render (non-blocking).
         *
         * Unlike renderLowResPreview(), this method does not call glFinish() and returns
         * an OpenCL event for async completion tracking. The caller is responsible for
         * synchronization and texture binding.
         *
         * @param queue OpenCL command queue to use for async execution
         * @param targetImage Pure CL image buffer to render into (no GL texture)
         * @return OpenCL event that signals when rendering is complete
         *
         * @thread Any thread with compute context access
         * @note Does not call bind() on result image - caller must handle GL texture update
         */
        [[nodiscard]] cl::Event renderLowResPreviewAsync(cl::CommandQueue const & queue,
                                                         ImageRGBA & targetImage) const;

        /**
         * @brief Renders low-res preview and outputs distance traveled to distance init buffer.
         *
         * This variant writes the ray-marched distance to the internal distance init buffer
         * which can be used to accelerate subsequent HQ renders via distance initialization.
         *
         * @param queue OpenCL command queue to use for async execution
         * @param targetImage Pure CL image buffer to render into
         * @return OpenCL event that signals when rendering is complete
         *
         * @thread Any thread with compute context access
         * @see renderSceneWithDistanceInit for using the distance buffer
         */
        [[nodiscard]] cl::Event
        renderLowResPreviewWithDistanceOutputAsync(cl::CommandQueue const & queue,
                                                   ImageRGBA & targetImage) const;

        /**
         * @brief Renders scene using distance initialization from a previous low-res pass.
         *
         * Uses the internal distance init buffer to skip early ray marching steps,
         * improving HQ render performance by 20-30%.
         *
         * @param commandQueue OpenCL command queue used for dispatch
         * @param startLine Starting line for rendering
         * @param endLine Ending line for rendering
         * @param targetImage Pure CL image buffer to render into
         * @param completionEvent Optional event for async completion tracking
         * @return true if rendering succeeded, false otherwise
         *
         * @pre Distance init buffer must be populated via
         * renderLowResPreviewWithDistanceOutputAsync
         * @thread Any thread with compute context access
         */
        [[nodiscard]] bool renderSceneWithDistanceInit(cl::CommandQueue const & commandQueue,
                                                       size_t startLine,
                                                       size_t endLine,
                                                       ImageRGBA & targetImage,
                                                       cl::Event * completionEvent = nullptr);

        /**
         * @brief Returns true if the distance init buffer is valid and can be used for HQ
         * rendering.
         * @return true if distance buffer has been populated and matches current parameters
         */
        [[nodiscard]] bool isDistanceInitBufferValid() const;

        /**
         * @brief Invalidates the distance init buffer, forcing next HQ render to use standard path.
         */
        void invalidateDistanceInitBuffer();

        /**
         * @brief Marks the distance init buffer as valid after successful population.
         * @note Call this after renderLowResPreviewWithDistanceOutputAsync completes
         */
        void setDistanceInitBufferValid();

        /**
         * @brief Render scene with metrics collection for performance analysis (T033/SC-002).
         *
         * Renders the scene while collecting ray marching metrics (total rays, total steps,
         * cache hits, non-converged rays). Use readMetricsBuffer() after completion to retrieve.
         *
         * @param commandQueue OpenCL command queue used for dispatch
         * @param startLine Starting line for rendering
         * @param endLine Ending line for rendering
         * @param targetImage Pure CL image buffer to render into
         * @param completionEvent Optional event for async completion tracking
         * @return true if rendering succeeded, false otherwise
         */
        [[nodiscard]] bool renderSceneWithMetrics(cl::CommandQueue const & commandQueue,
                                                  size_t startLine,
                                                  size_t endLine,
                                                  ImageRGBA & targetImage,
                                                  cl::Event * completionEvent = nullptr);

        /// @brief Clear metrics buffer before starting a metrics collection pass
        void clearMetricsBuffer();

        /// @brief Read ray marching metrics after a renderSceneWithMetrics pass completes
        [[nodiscard]] RayMarchMetrics readMetricsBuffer() const;

        bool precomputeSdfForWholeBuildPlatform();
        void precomputeSdfForBBox(const BoundingBox & boundingBox);

        /**
         * @brief Asynchronously precompute SDF for the whole build platform (non-blocking).
         * @param queue OpenCL command queue to use for async execution
         * @return Event that can be waited on to track SDF completion
         */
        [[nodiscard]] cl::Event precomputeSdfAsync(cl::CommandQueue const & queue);

        /// @brief Prepares the compute core for thumbnail generation in headless mode
        /// @return true if preparation succeeded, false otherwise
        bool prepareImageRendering();
        [[nodiscard]] SharedGLImageBuffer getResultImage() const;

        /// @brief Returns the low-resolution preview image buffer.
        /// @return Shared pointer to the low-res preview image, may be null if not initialized.
        [[nodiscard]] SharedGLImageBuffer getLowResPreviewImage() const;

        [[nodiscard]] SharedContourExtractor getContour() const;

        [[nodiscard]] cl_float getSliceHeight() const;

        void setSliceHeight(cl_float z_mm);

        [[nodiscard]] SharedSlicerProgram getSlicerProgram() const;
        [[nodiscard]] SharedRenderProgram getBestRenderProgram() const;
        [[nodiscard]] std::optional<SharedRenderProgram> tryGetBestRenderProgram() const;
        [[nodiscard]] RenderBackend getSelectedRenderBackend() const;
        [[nodiscard]] std::optional<RenderBackend> tryGetSelectedRenderBackend() const;
        [[nodiscard]] SharedRenderProgram getPreviewRenderProgram() const;
        [[nodiscard]] SharedRenderProgram getOptimzedRenderProgram() const;

        bool setScreenResolution(size_t width, size_t height);
        bool setLowResPreviewResolution(size_t width, size_t height);
        [[nodiscard]] std::pair<size_t, size_t> getLowResPreviewResolution() const;
        [[nodiscard]] SharedPrimitives getPrimitives() const;

        [[nodiscard]] SharedResources getResourceContext() const;

        void generateSdfSlice() const;
        [[nodiscard]] std::optional<BoundingBox> getBoundingBox() const;
        [[nodiscard]] BoundingBoxComputationSource
        getBoundingBoxComputationSource() const noexcept;
        void updateClippingAreaWithPadding() const;
        void updateClippingAreaToBoundingBox() const;
        [[nodiscard]] bool isVdbRequired() const;

        [[nodiscard]] bool isAnyCompilationInProgress() const;

        /// Non-blocking check for compilation progress using atomic flags only.
        /// Safe to call from any thread without risk of blocking on mutex.
        [[nodiscard]] bool isAnyCompilationInProgressNonBlocking() const noexcept;

        bool updateBBox();
        void updateBBoxOrThrow();

        void refreshProgram(nodes::SharedAssembly assembly);
        void tryRefreshProgramProtected(nodes::SharedAssembly assembly);

        /// Return whether the currently selected render program can produce frames.
        /// Unlike isRendererReady(), this intentionally ignores broader model/SDF refresh state
        /// so the UI can keep using the command-stream preview while background work continues.
        [[nodiscard]] bool isRenderProgramReady() const;

        /// Non-blocking variant of isRenderProgramReady(). Returns std::nullopt if background
        /// loading/compilation currently owns the program-manager locks.
        [[nodiscard]] std::optional<bool> tryIsRenderProgramReady() const;

        [[nodiscard]] bool isRendererReady() const;

        [[nodiscard]] SharedComputeContext getComputeContext() const;

        /// @brief Retain the current scene and copy its camera/settings for one render session.
        [[nodiscard]] SharedRenderSession createRenderSession() const;

        /// @brief Retain the current scene only when it represents the requested document revision.
        /// @return A session for the exact revision, or std::nullopt for an untagged/mismatched
        /// generation.
        [[nodiscard]] std::optional<SharedRenderSession>
        createRenderSession(RenderSceneRevision revision) const;

        /// @brief Publish a fully materialized render generation.
        ///
        /// The generation becomes the source for new render sessions and the legacy aliases
        /// exposed by ComputeCore are rebound as one operation. Existing sessions retain their
        /// previous generation through their shared ownership. The caller must hold the compute
        /// resource/build barrier while materializing the generation.
        void publishRenderSceneGeneration(SharedRenderSceneGeneration generation);

        void compileSlicerProgramBlocking();

        void logMsg(std::string msg) const;
        [[nodiscard]] std::string getProgramStateSummary() const;

        void computeVertexNormals(Mesh & mesh) const;

        void recompileIfRequired();
        void recompileBlockingNoLock();

        /// Check if OpenCL program compilation is currently in progress
        [[nodiscard]] bool isCompilationInProgress() const;

        void resetBoundingBox();

        /// Mark the bounding box as stale (parameter changed) without clearing it.
        /// The cached bbox will be reused with extra margin until recomputed.
        void markBoundingBoxStale();

        /// Check if the bounding box is stale (parameter changed since last bbox computation)
        [[nodiscard]] bool isBoundingBoxStale() const;

        /// Clear the stale flag and reset the bounding box so it will be fully recomputed
        void recomputeStaleBoundingBox();

        BitmapLayer generateDownSkinMap(float z_mm, Vector2 pixelSize_mm);
        BitmapLayer generateUpSkinMap(float z_mm, Vector2 pixelSize_mm);

        void setComputeContext(std::shared_ptr<ComputeContext> context);

        bool requestContourUpdate(nodes::SliceParameter sliceParameter);

        /// Invalidate cached contour height so the next requestContourUpdate triggers a
        /// recomputation.
        void invalidateContourCache();

        bool isSlicingInProgress() const;

        std::mutex & getContourExtractorMutex();

        void invalidatePreCompSdf(std::string_view reason = {});
        void setSdfValid(bool valid);
        [[nodiscard]] bool isSdfValid() const;
        [[nodiscard]] ApproximationMode getLastUsedApproximation() const;
        [[nodiscard]] ApproximationMode getLastUsedPreviewApproximation() const;
        [[nodiscard]] ApproximationMode getLastUsedHQApproximation() const;
        [[nodiscard]] events::SharedLogger getSharedLogger() const;

        [[nodiscard]] CodeGenerator getCodeGenerator() const;

        void setCodeGenerator(CodeGenerator generator);

        void setOptimizedRenderCompilationDeferred(bool deferred);
        [[nodiscard]] bool isOptimizedRenderCompilationDeferred() const;
        void setSlicerCompilationDeferred(bool deferred);
        [[nodiscard]] bool isSlicerCompilationDeferred() const;

        [[nodiscard]] std::shared_ptr<ModelState> getMeshResourceState() const;

        PlainImage createThumbnail();
        PlainImage createThumbnailPng();
        void saveThumbnail(std::filesystem::path const & filename);

        void applyCamera(ui::OrbitalCamera const & camera);

        void injectSmoothingKernel(std::string const & kernel);

        bool isBusy() const;

        [[nodiscard]] bool tryToupdateParameter(nodes::Assembly & assembly);
        [[nodiscard]] bool updateParameterBlocking(nodes::Assembly & assembly);

        /// @brief Monotonic counter of GPU parameter-buffer uploads that actually changed the
        /// model parameters. Incremented inside updateParameterBlocking() right after the new
        /// values are written to the parameter buffer. Progressive HQ rendering uses this to
        /// detect a parameter upload that lands mid-fill (which would otherwise leave the upper
        /// band rendered with the old parameters and the lower band with the new ones).
        /// @return The current parameter generation.
        [[nodiscard]] std::uint64_t getParameterGeneration() const
        { return m_parameterGeneration.load(std::memory_order_acquire); }

        /// Check if parameter structure matches compiled signature (fast path possible)
        [[nodiscard]] bool isParameterSignatureCompatible(nodes::Assembly const & assembly) const;

        /// Get the compiled parameter signature from program manager
        [[nodiscard]] ParameterSignature const & getCompiledParameterSignature() const;

        void setPreCompSdfSize(size_t size);

        /// Build voxel acceleration grids for spatial mesh resources
        /// @param buildParams Vector of build parameters from
        /// ResourceManager::collectVoxelGridBuildParams()
        /// @return Number of grids successfully built
        size_t buildMeshVoxelGrids(std::vector<MeshVoxelGridBuildParams> const & buildParams);

        /// Build Fast-Winding-Number aggregate buffers for spatial mesh resources.
        /// This is a prerequisite for FWN rendering and sign-cache construction.
        /// @param buildParams Vector of build parameters from
        /// ResourceManager::collectFwnAggregateBuildParams()
        /// @return Number of aggregate buffers successfully built
        size_t buildMeshFwnAggregates(std::vector<MeshFwnAggregateBuildParams> const & buildParams);

        /// Queue bounded coarse FWN sign-cache build steps for spatial mesh resources.
        /// The kernels and final ready-offset patch are queued without waiting;
        /// until the ready patch executes, the render kernel sees offset 0 and
        /// falls back to full FWN.
        /// @param buildParams Vector of build parameters from
        /// ResourceManager::collectSignCacheBuildParams()
        /// @return Number of sign-cache build steps successfully queued
        size_t buildMeshSignCaches(std::vector<MeshSignCacheBuildParams> const & buildParams);

        void adoptVertexOfMeshToSurface(VertexBuffer & vertices);

        void setAutoUpdateBoundingBox(bool autoUpdateBoundingBox);
        [[nodiscard]] bool isAutoUpdateBoundingBoxEnabled() const;

        /// @brief Check if SDF precomputation is currently in progress
        /// @return True if SDF computation is running asynchronously
        [[nodiscard]] bool isSdfComputationInProgress() const noexcept;

        /// @brief Check if bounding box computation is currently in progress
        /// @return True if bounding box calculation is running
        [[nodiscard]] bool isBoundingBoxComputationInProgress() const noexcept;

        /// Get the program manager for direct access to specialized programs
        [[nodiscard]] ProgramManager & getProgramManager();
        [[nodiscard]] ProgramManager const & getProgramManager() const;

      private:
        bool updateBoundingBoxFast();
        [[nodiscard]] static bool isBoundingBoxMeaningful(BoundingBox const & box);
        [[nodiscard]] std::optional<BoundingBox> computeBoundingBoxFromPrimitives() const;
        [[nodiscard]] bool ensureSlicerProgramReady();
        void throwIfNoOpenGL() const;
        [[nodiscard]] events::Logger & getLogger() const;

        cl_int2 determineBufferSize(float2 pixelSize_mm) const;
        void reinitIfNecssary();
        [[nodiscard]] bool requiresNanoVdbLocked() const;

        [[nodiscard]] int layerNumber() const;

        void renderResultImageInterOp(DistanceMap & sourceImage, GLImageBuffer & targetImage) const;
        void renderResultImageReadPixel(DistanceMap & sourceImage,
                                        GLImageBuffer & targetImage) const;
        void renderImage(DistanceMap & sourceImage) const;
        void generateContours(nodes::SliceParameter sliceParameter);
        void generateContourInternal(nodes::SliceParameter const & sliceParameter);
        void generateContourMarchingSquare(nodes::SliceParameter const & sliceParameter);
        void generateContourQuadtree(nodes::SliceParameter const & sliceParameter);

        struct OptimizedSourceGenerationResult
        {
            std::uint64_t generation = 0u;
            std::string source;
            ParameterSignature parameterSignature;
            std::string errorMessage;
        };

        [[nodiscard]] std::uint64_t beginOptimizedSourceGeneration();
        [[nodiscard]] bool isOptimizedSourceGenerationCurrent(std::uint64_t generation) const;
        void startOptimizedSourceGeneration(nodes::SharedAssembly assembly,
                                            ParameterSignature parameterSignature,
                                            std::uint64_t generation);
        void pollOptimizedSourceGeneration();

        mutable std::recursive_mutex m_computeMutex; // TODO: replace with std::mutex

        // Keep the OpenCL context before every member that owns buffers/images referencing it.
        // Members are destroyed in reverse declaration order; the context must therefore be
        // declared first so GPU resources can release themselves while the context is still alive.
        SharedComputeContext m_ComputeContext;

        /// Owns the context-bound scene resources and programs. The aliases below preserve the
        /// existing ComputeCore API while the render-session refactor is completed.
        SharedRenderSceneGeneration m_sceneState;

        SharedContourExtractor m_contour;
        SharedResources m_resources;
        mutable std::atomic<SharedGLImageBuffer> m_resultImage;
        mutable std::atomic<SharedGLImageBuffer> m_lowResPreviewImage;
        std::shared_ptr<ImageRGBA> m_thumbnailImage;
        std::shared_ptr<ImageRGBA> m_thumbnailImageHighRes;
        SharedPrimitives m_primitives;

        double layerThickness_mm = 0.05;
        cl_float m_sliceHeight_mm{0.0f};

        std::atomic<cl_float> m_lastContourSliceHeight_mm{0.0f};
        std::atomic<bool> m_lastContourUseAdaptive{true};
        std::atomic<float> m_lastContourMinFeatureSize_mm{0.2f};

        std::optional<BoundingBox> m_boundingBox{};
        std::atomic<BoundingBoxComputationSource> m_boundingBoxSource{
          BoundingBoxComputationSource::None};
        std::atomic<bool> m_boundingBoxStale{false};
        bool m_isComputationTimeLoggingEnabled = false;

        RequiredCapabilities m_capabilities = RequiredCapabilities::OpenGLInterop;
        events::SharedLogger m_eventLogger;

        std::shared_ptr<ModelState> m_meshResourceState;

        mutable std::future<void> m_sliceFuture;
        mutable std::mutex m_sliceFutureMutex;
        std::mutex m_contourExtractorMutex;
        std::atomic_bool m_slicingInProgress{false};

        std::atomic_bool m_precompSdfIsValid{false};
        size_t m_preCompSdfSize = 128u;

        bool m_autoUpdateBoundingBox = true;

        /// @brief Tracks whether SDF precomputation is currently running
        std::atomic<bool> m_sdfComputationInProgress{false};

        /// @brief Tracks whether bounding box computation is currently running
        std::atomic<bool> m_boundingBoxComputationInProgress{false};

        /// @brief Tracks whether the distance init buffer contains valid data
        /// @note Invalidated on parameter changes, camera changes, or resolution changes
        std::atomic_bool m_distanceInitBufferValid{false};

        /// @brief Tracks last used approximation modes for UI status display only
        /// @note Mutable because these are purely diagnostic and set during const render methods
        /// @{
        mutable ApproximationMode m_lastUsedApproximation = AM_FULL_MODEL;
        mutable ApproximationMode m_lastUsedPreviewApproximation = AM_FULL_MODEL;
        mutable ApproximationMode m_lastUsedHQApproximation = AM_FULL_MODEL;
        /// @}

        CodeGenerator m_codeGenerator = CodeGenerator::Automatic;

        SharedKernelReplacements m_kernelReplacements;

        std::atomic<std::uint64_t> m_optimizedSourceGenerationEpoch{0u};
        std::mutex m_optimizedSourceGenerationMutex;
        std::vector<std::future<OptimizedSourceGenerationResult>> m_optimizedSourceGenerationJobs;

        /// Monotonic generation of GPU parameter-buffer uploads that changed the parameters.
        std::atomic<std::uint64_t> m_parameterGeneration{1u};

        ProgramManager * m_programs;
    };
}
