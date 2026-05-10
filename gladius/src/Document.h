#pragma once

#include "BackupManager.h"
#include "BitmapChannel.h"
#include "Mesh.h"
#include "MeshSdfMethod.h"
#include "compute/ComputeCore.h"
#include "io/3mf/Importer3mf.h"
#include "io/3mf/ImageStackCreator.h"
#include "io/3mf/ResourceDependencyGraph.h"
#include "io/SurfaceExtractionOptions.h"
#include "nodes/Assembly.h"
#include "nodes/BuildItem.h"
#include "nodes/IssueList.h"
#include "nodes/Model.h"
#include "ui/GLView.h"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <mutex>
#include <optional>

namespace gladius
{
    /// Tokens for assembly mutex access
    using AssemblyToken = std::lock_guard<std::mutex>;
    using OptionalAssemblyToken = std::optional<AssemblyToken>;

    /// Monotonically increasing counter identifying each structural graph edit.
    /// Incremented on the UI thread; read by background workers to detect staleness.
    using StructuralEditEpoch = std::atomic<uint64_t>;

    /// Controls debounce timing for structural edit dispatch.
    /// All members are accessed on the UI thread only.
    struct StructuralEditDebouncer
    {
        std::atomic<bool> pending{false};
        std::chrono::steady_clock::time_point lastEditTime{};
        std::chrono::milliseconds debounceDelay{50};
    };

    namespace vdb
    {
        struct TriangleMesh;
    }
    class ParameterNotFoundException : public std::exception
    {
      public:
        const char * what() const noexcept override
        {
            return "Parameter could not be found";
        }
    };

    class ParameterAndModelNotFound : public ParameterNotFoundException
    {
      public:
        char const * what() const noexcept override
        {
            return "Parameter could not be found. The model name does not match a model in "
                   "this assembly";
        }
    };

    class ParameterAndNodeNotFound : public ParameterNotFoundException
    {
      public:
        char const * what() const noexcept override
        {
            return "Parameter could not be found. The node name does not match to a node in "
                   "this model.";
        }
    };

    class ParameterCouldNotBeConvertedToFloat : public ParameterNotFoundException
    {
      public:
        char const * what() const noexcept override
        {
            return "The parameter could not be converted to a float";
        }
    };

    class ParameterCouldNotBeConvertedToVector : public ParameterNotFoundException
    {
      public:
        char const * what() const noexcept override
        {
            return "The parameter could not be converted to a Vector";
        }
    };

    class ParameterCouldNotBeConvertedToString : public ParameterNotFoundException
    {
      public:
        char const * what() const noexcept override
        {
            return "The parameter could not be converted to a double";
        }
    };

    class Document
    {
      public:
        /**
         * @brief Waits until the assembly mutex can be locked and returns a token that keeps it
         * locked.
         *
         * The token is an RAII wrapper that automatically releases the mutex when it goes out of
         * scope. Use this method when you need guaranteed access to the assembly and are willing to
         * wait.
         *
         * @return AssemblyToken An RAII token that keeps the mutex locked for its lifetime
         */
        AssemblyToken waitForAssemblyToken() const;

        /**
         * @brief Attempts to acquire a lock on the assembly mutex without waiting.
         *
         * This method tries to lock the mutex without blocking. If the mutex is already locked,
         * it returns an empty optional. Otherwise, it returns an optional containing a token
         * that keeps the mutex locked.
         *
         * @return OptionalAssemblyToken An optional that contains a token if the lock was acquired
         */
        OptionalAssemblyToken requestAssemblyToken() const;

        void resetGeneratorContext();
        explicit Document(std::shared_ptr<ComputeCore> core);
        [[nodiscard]] bool refreshModelIfNoCompilationIsRunning();

        /// Signal that a structural graph edit occurred.
        /// Increments the edit epoch and arms the debouncer for background dispatch.
        void signalStructuralEdit();

        /// @return true if a structural edit is pending dispatch (debouncer armed).
        [[nodiscard]] bool hasStructuralEditPending() const;

        /// Dispatch the background structural update if the debounce window has elapsed.
        /// Call this once per frame from the main loop.
        /// @return true if a compilation was actually launched.
        bool dispatchStructuralUpdateIfReady();

        /// @return Current structural edit epoch value.
        [[nodiscard]] uint64_t structuralEditEpoch() const;

        void load(std::filesystem::path filename);
        void loadNonBlocking(std::filesystem::path filename);
        void merge(std::filesystem::path filename);

        /// Import functions from another file without triggering recompilation.
        /// Use when the caller will create additional nodes before compilation.
        void mergeOnly(std::filesystem::path filename);

        /// Merge a library file and resolve the best matching imported function.
        /// Does NOT trigger recompilation — the caller is expected to create a
        /// FunctionCall node and let the flag-driven mechanism handle it.
        /// @param filename  Path to the .3mf library file.
        /// @param targetFunctionName  Display name to match (empty = first new).
        /// @return FunctionMatch with the resolved function (id==0 on failure).
        [[nodiscard]] nodes::FunctionMatch
        mergeAndResolve(std::filesystem::path filename,
                        std::string const & targetFunctionName);

        /**
         * @brief Check if a file is currently being loaded asynchronously
         * @return true if a file load is in progress
         */
        [[nodiscard]] bool isLoadingInProgress() const;

        /**
         * @brief Get the last loading error message if any
         * @return Error message or empty string if no error
         */
        [[nodiscard]] std::string getLoadingError() const;
        void saveAs(std::filesystem::path filename, bool writeThumbnail = true);

        void newModel();
        void newEmptyModel();
        void newFromTemplate();

        /// @return true if the parameter values were successfully pushed to the GPU
        bool updateParameter();
        void updateParameterRegistration();
        void updatePayload();
        void refreshModelBlocking();

        void exportAsStl(std::filesystem::path const & filename);
        void exportAsStl(std::filesystem::path const & filename,
                         io::StlExportOptions const & options);

        void markFileAsChanged();

        /// @brief Check if the file has unsaved changes.
        [[nodiscard]] bool isFileChanged() const { return m_fileChanged; }

        void invalidatePrimitiveData();
        nodes::SharedAssembly getAssembly() const;
        nodes::SharedAssembly getFlatAssembly() const;

        /**
         * @brief Get the current assembly filename
         * @return The current assembly filename if available, empty optional otherwise
         */
        std::optional<std::filesystem::path> getCurrentAssemblyFilename() const;

        float getFloatParameter(ResourceId modelId,
                                std::string const & nodeName,
                                std::string const & parameterName);

        void setFloatParameter(ResourceId modelId,
                               std::string const & nodeName,
                               std::string const & parameterName,
                               float value);

        std::string & getStringParameter(ResourceId modelId,
                                         std::string const & nodeName,
                                         std::string const & parameterName);

        void setStringParameter(ResourceId modelId,
                                std::string const & nodeName,
                                std::string const & parameterName,
                                std::string const & value);

        nodes::float3 & getVector3fParameter(ResourceId modelId,
                                             std::string const & nodeName,
                                             std::string const & parameterName);

        void setVector3fParameter(ResourceId modelId,
                                  std::string const & nodeName,
                                  std::string const & parameterName,
                                  nodes::float3 const & value);

        [[nodiscard]] PolyLines generateContour(float z,
                                                nodes::SliceParameter const & sliceParameter) const;

        [[nodiscard]] BoundingBox computeBoundingBox() const;

        [[nodiscard]] Mesh generateMesh() const;

        [[nodiscard]] BitmapChannels & getBitmapChannels();

        [[nodiscard]] nodes::GeneratorContext & getGeneratorContext();

        [[nodiscard]] SharedComputeContext getComputeContext() const;

        [[nodiscard]] events::SharedLogger getSharedLogger() const;

        [[nodiscard]] std::shared_ptr<ComputeCore> getCore();

        void set3mfModel(Lib3MF::PModel model);

        [[nodiscard]] Lib3MF::PModel get3mfModel() const;

        nodes::Model & createNewFunction();
        nodes::Model & createLevelsetFunction(std::string const & name);
        nodes::Model & copyFunction(nodes::Model const & sourceModel, std::string const & name);
        nodes::Model & wrapExistingFunction(nodes::Model & sourceModel, std::string const & name);

        void injectSmoothingKernel(std::string const & kernel);

        nodes::BuildItems::iterator addBuildItem(nodes::BuildItem && item);

        [[nodiscard]] nodes::BuildItems const & getBuildItems() const;
        void clearBuildItems();

        void replaceMeshResource(ResourceKey const & key, SharedMesh mesh);

        std::optional<ResourceKey> addMeshResource(std::filesystem::path const & filename);
        ResourceKey addMeshResource(vdb::TriangleMesh && mesh, std::string const & name);

        void deleteResource(ResourceId id);
        void deleteResource(ResourceKey key);

        void deleteFunction(ResourceId id);

        ResourceManager & getResourceManager();
        ResourceManager const & getResourceManager() const;

        void addBoundingBoxAsMesh();
        void addCustomBoxMesh(float width,
                              float height,
                              float depth,
                              float startX = 0.0f,
                              float startY = 0.0f,
                              float startZ = 0.0f);

        void addMeshAsBeamLattice(std::filesystem::path const & stlFilename, float beamRadius);

        ResourceKey addImageStackResource(std::filesystem::path const & path);

        /// Import image stack with padding support - returns ImportResult for notification handling
        io::ImportResult addImageStackResourceWithPadding(std::filesystem::path const & path);

        // syncing of the 3MF model with the document

        /**
         * @brief Updates the 3MF model with the current state of the document.
         *
         */
        void update3mfModel();

        /**
         * @brief Updates the document from the 3MF model.
         *
         */
        void updateDocumentFrom3mfModel(bool skipImplicitFunctions = false);

        /**
         * @brief Checks if a resource can be safely deleted, without dependencies.
         * @param key The key of the resource to check.
         * @return Result containing removal possibility and dependent items.
         */
        gladius::io::CanResourceBeRemovedResult isItSafeToDeleteResource(ResourceKey key);

        /**
         * @brief Removes all resources that are not used by any build item.
         *
         * This method identifies resources that are not directly or indirectly referenced
         * by any build item and removes them from the model. It uses the ResourceDependencyGraph
         * to find unused resources and safely delete them.
         *
         * @return The number of resources that were removed
         */
        std::size_t removeUnusedResources();

        /**
         * @brief Updates the resource dependency graph and finds all unused resources.
         *
         * This method is a public version that updates the dependency graph and returns
         * unused resources without deleting them.
         *
         * @return Vector of resource pointers that can be safely removed.
         */
        std::vector<Lib3MF::PResource> findUnusedResources();

        /**
         * @brief Get the ResourceDependencyGraph object
         *
         * @return A pointer to the resource dependency graph (nullptr if not available)
         */
        [[nodiscard]] const gladius::io::ResourceDependencyGraph *
        getResourceDependencyGraph() const;

        /**
         * @brief Rebuilds the dependency graph for the current 3MF model
         *
         * Creates a new ResourceDependencyGraph and builds the graph for the currently loaded 3MF
         * model. This is used to track dependencies between resources for safe resource deletion.
         */
        void rebuildResourceDependencyGraph();

        /**
         * @brief Mark validation as needing to be re-run.
         *
         * Called when the graph structure changes (nodes added/removed, connections changed).
         * The next call to validateAssemblyIfDirty() will re-run validation.
         */
        void markValidationDirty();

        /**
         * @brief Validate the assembly only if marked dirty.
         *
         * Efficient method for UI use - only re-validates when the graph has changed.
         * Clears the dirty flag after validation.
         *
         * @param context The validation context (Interactive, FileLoad, or Api)
         * @return True if the assembly is valid, false otherwise
         */
        bool validateAssemblyIfDirty(nodes::ValidationContext context = nodes::ValidationContext::Interactive);

        /**
         * @brief Validates the current assembly
         *
         * Validates the assembly using the nodes::Validator.
         * Populates the issue list with any validation errors.
         * When called during file loading or API operations, also logs events.
         *
         * @param context The validation context (Interactive, FileLoad, or Api)
         * @return True if the assembly is valid, false otherwise
         */
        bool validateAssembly(nodes::ValidationContext context = nodes::ValidationContext::Interactive);

        /**
         * @brief Get the issue list containing validation errors and warnings
         *
         * @return Reference to the issue list
         */
        nodes::IssueList& getIssueList();

        /**
         * @brief Get the issue list containing validation errors and warnings (const version)
         *
         * @return Const reference to the issue list
         */
        nodes::IssueList const& getIssueList() const;

        /**
         * @brief Get the backup manager instance
         *
         * @return BackupManager& Reference to the backup manager
         */
        BackupManager & getBackupManager();

        /**
         * @brief Get the backup manager instance (const version)
         *
         * @return const BackupManager& Const reference to the backup manager
         */
        const BackupManager & getBackupManager() const;

        /**
         * @brief Set whether the application is running in UI mode
         *
         * This determines whether automatic backups should be created.
         * When false (API/library mode), no backups are created.
         *
         * @param uiMode True if UI is active, false for API/library usage
         */
        void setUiMode(bool uiMode);

        /**
         * @brief Check if the application is running in UI mode
         *
         * @return true if UI mode is active, false otherwise
         */
        bool isUiMode() const;

        /// Set the mesh-repair configuration that any subsequent 3MF import will
        /// apply to triangle meshes before BVH construction. Defaults to all-disabled.
        void setMeshRepairConfig(mesh_repair::MeshRepairConfig const & cfg)
        {
            m_meshRepairConfig = cfg;
        }

        [[nodiscard]] mesh_repair::MeshRepairConfig const & getMeshRepairConfig() const noexcept
        {
            return m_meshRepairConfig;
        }

        /// Set the mesh-SDF evaluation configuration that subsequent 3MF imports
        /// apply while constructing spatial mesh resources.
        void setMeshSdfEvaluationConfig(MeshSdfEvaluationConfig const & cfg)
        {
            m_meshSdfEvaluationConfig = cfg;
        }

        [[nodiscard]] MeshSdfEvaluationConfig const & getMeshSdfEvaluationConfig() const noexcept
        {
            return m_meshSdfEvaluationConfig;
        }

        /// Runtime NanoVDB build policy derived from the current caller context and compute
        /// device limits.
        [[nodiscard]] NanoVdbBuildPolicy getNanoVdbBuildPolicy() const;

        /// Summarize NanoVDB build issues on currently loaded mesh resources, if any.
        [[nodiscard]] NanoVdbBuildIssueSummary getNanoVdbBuildIssueSummary() const;

        /// Queue applying the mesh-SDF evaluation configuration to existing mesh resources.
        /// Heavy resource rebuild/upload work is folded into the debounced background refresh so
        /// the UI can keep showing the current preview.
        /// @return Number of mesh resources that need a background update.
        std::size_t queueMeshSdfEvaluationConfigUpdate(MeshSdfEvaluationConfig const & cfg);

        void updateFlatAssembly();

      private:
        enum class RefreshMode
        {
            Normal,
            InteractiveFirst
        };

        [[nodiscard]] nodes::VariantParameter &
        findParameterOrThrow(ResourceId modelId,
                             std::string const & nodeName,
                             std::string const & parameterName);

        void loadImpl(const std::filesystem::path & filename);
        void mergeImpl(const std::filesystem::path & filename);
        [[nodiscard]] bool refreshModelAsync();
        void loadAllMeshResources();
        void refreshWorker(RefreshMode refreshMode = RefreshMode::Normal);
        [[nodiscard]] std::optional<MeshSdfEvaluationConfig> takePendingMeshSdfEvaluationConfig();
        std::size_t applyMeshSdfEvaluationConfigToResources(MeshSdfEvaluationConfig const & cfg);

        /// Dispatch a structural update via the existing refresh pipeline.
        /// @return true if a compilation was launched, false if one was already running.
        bool dispatchStructuralUpdate();

        void updateMemoryOffsets();

        void saveBackup();

        std::unique_ptr<nodes::GeneratorContext> m_generatorContext;
        nodes::SharedAssembly m_assembly;
        nodes::SharedAssembly m_flatAssembly;

        std::filesystem::path m_modelFileName;
        std::optional<std::filesystem::path> m_currentAssemblyFileName;
        std::shared_ptr<ComputeCore> m_core;
        bool m_fileChanged{false};
        std::atomic<bool> m_parameterDirty{false};
        std::atomic<bool> m_contoursDirty{false};

        /// Mesh repair configuration applied at 3MF import time.
        mesh_repair::MeshRepairConfig m_meshRepairConfig{};

        /// Mesh SDF evaluation configuration applied to newly imported spatial meshes.
        MeshSdfEvaluationConfig m_meshSdfEvaluationConfig{};

        std::atomic<NanoVdbFailurePolicy> m_nanovdbFailurePolicy{NanoVdbFailurePolicy::Degrade};

        mutable std::mutex m_pendingMeshSdfEvaluationConfigMutex;
        std::optional<MeshSdfEvaluationConfig> m_pendingMeshSdfEvaluationConfig;

        bool m_primitiveDateNeedsUpdate{true};

        BitmapChannels m_channels;

        Lib3MF::PModel m_3mfmodel;

        std::future<void> m_futureModelRefresh;
        std::future<void> m_futureFileLoad;
        std::atomic<bool> m_isLoading{false};
        mutable std::mutex m_loadingErrorMutex;
        std::string m_loadingError;

        nodes::BuildItems m_buildItems;

        // last backup time
        std::chrono::time_point<std::chrono::system_clock> m_lastBackupTime;

        /// Dependency graph for resource removal checks
        std::unique_ptr<gladius::io::ResourceDependencyGraph> m_resourceDependencyGraph;

        /// Mutex for protecting m_assembly
        mutable std::mutex m_assemblyMutex;

        /// Backup manager for handling file backups
        BackupManager m_backupManager;

        /// Flag to track if UI mode is active (determines if backups should be created)
        bool m_uiMode = false;

        /// Flag to track if validation needs to be re-run
        std::atomic<bool> m_validationDirty{true};

        /// Issue list containing validation errors and warnings
        nodes::IssueList m_issueList;

        /// Structural edit epoch — monotonically increasing on each structural graph edit.
        StructuralEditEpoch m_structuralEditEpoch{0};

        /// Debouncer controlling when the background structural update is dispatched.
        StructuralEditDebouncer m_structuralDebouncer;
    };

    using SharedDocument = std::shared_ptr<Document>;
}
