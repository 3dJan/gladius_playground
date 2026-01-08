# Quickstart: Async Shell-Based Color Export

**Feature**: 001-async-shell-export  
**Date**: 2026-01-07  

## Overview

This document provides implementation patterns for the async shell export feature. The design follows the existing `ManifoldDualContouringStlExporter` pattern.

## ShellExporter Class

### Header: `gladius/src/io/ShellExporter.h`

```cpp
#pragma once

#include "IExporter.h"
#include "3mf/FilamentOpticalProperties.h"
#include "3mf/FrontlitThicknessSolver.h"
#include "SurfaceExtractionOptions.h"
#include "../EventLogger.h"

#include <atomic>
#include <filesystem>
#include <future>
#include <string>
#include <vector>

namespace gladius
{
    class ComputeCore;
    class Document;
}

namespace gladius::io
{
    /// @brief Configuration for shell-based color export
    struct ShellExportConfig
    {
        FilamentStack filamentStack;                      ///< Ordered materials (bottom to top)
        std::vector<std::vector<float>> precomputedLuts;  ///< Thickness LUTs per layer
        int lutResolution = 16;                           ///< LUT grid resolution
        ThicknessConstraints thicknessConstraints;        ///< Thickness limits
        ManifoldDualContouringOptions mdcOptions;         ///< Mesh extraction options
    };

    /// @brief Async exporter for shell-based color 3MF export
    ///
    /// Implements IExporter interface for background shell generation with
    /// per-shell progress reporting and cooperative cancellation.
    class ShellExporter : public IExporter
    {
      public:
        ShellExporter();
        explicit ShellExporter(events::SharedLogger logger);

        /// @brief Configure export parameters
        void setConfig(ShellExportConfig config);

        /// @brief Set the document for metadata and thumbnail
        void setDocument(Document const* doc);

        // IExporter interface
        void beginExport(std::filesystem::path const& fileName, ComputeCore& generator) override;
        bool advanceExport(ComputeCore& generator) override;
        void finalize() override;
        [[nodiscard]] double getProgress() const override;

        /// @brief Check if export ended with error
        [[nodiscard]] bool hasError() const;

        /// @brief Get error message if hasError() returns true
        [[nodiscard]] std::string const& errorMessage() const;

        /// @brief Get current status message (e.g., "Generating shell 2/5...")
        [[nodiscard]] std::string getStatusMessage() const;

      private:
        enum class State
        {
            Idle,
            Running,
            Completed,
            Failed,
            Cancelled
        };

        void performExport(ComputeCore& generator);

        events::SharedLogger m_logger;
        ShellExportConfig m_config;
        Document const* m_document = nullptr;
        std::filesystem::path m_targetFile;
        
        std::atomic<State> m_state{State::Idle};
        std::atomic<double> m_progress{0.0};
        mutable std::mutex m_statusMutex;
        std::string m_statusMessage;
        std::string m_errorMessage;
        
        std::future<void> m_exportFuture;
    };

} // namespace gladius::io
```

### Implementation: `gladius/src/io/ShellExporter.cpp`

```cpp
#include "ShellExporter.h"

#include "3mf/ShellGenerator.h"
#include "3mf/MeshWriter3mf.h"
#include "ComputeContext.h"
#include "ComputeCore.h"
#include "Document.h"
#include "Mesh.h"

#include <fmt/format.h>

namespace gladius::io
{
    ShellExporter::ShellExporter() = default;

    ShellExporter::ShellExporter(events::SharedLogger logger)
        : m_logger(std::move(logger))
    {
    }

    void ShellExporter::setConfig(ShellExportConfig config)
    {
        m_config = std::move(config);
    }

    void ShellExporter::setDocument(Document const* doc)
    {
        m_document = doc;
    }

    void ShellExporter::beginExport(std::filesystem::path const& fileName, ComputeCore& generator)
    {
        if (m_config.filamentStack.empty())
        {
            throw std::runtime_error("Shell export requires at least one material");
        }

        m_targetFile = fileName;
        m_state.store(State::Running, std::memory_order_release);
        m_progress.store(0.0, std::memory_order_release);
        m_errorMessage.clear();

        {
            std::lock_guard lock(m_statusMutex);
            m_statusMessage = "Starting shell export...";
        }

        m_exportFuture = std::async(std::launch::async, [this, &generator] {
            try
            {
                performExport(generator);
                if (m_state.load(std::memory_order_acquire) == State::Running)
                {
                    m_state.store(State::Completed, std::memory_order_release);
                }
            }
            catch (std::exception const& e)
            {
                m_errorMessage = e.what();
                m_state.store(State::Failed, std::memory_order_release);
            }
        });
    }

    bool ShellExporter::advanceExport(ComputeCore& /*generator*/)
    {
        State const state = m_state.load(std::memory_order_acquire);
        return state == State::Running;
    }

    void ShellExporter::finalize()
    {
        if (m_exportFuture.valid())
        {
            m_exportFuture.wait();
        }
        m_state.store(State::Idle, std::memory_order_release);
    }

    double ShellExporter::getProgress() const
    {
        return m_progress.load(std::memory_order_acquire);
    }

    bool ShellExporter::hasError() const
    {
        return m_state.load(std::memory_order_acquire) == State::Failed;
    }

    std::string const& ShellExporter::errorMessage() const
    {
        return m_errorMessage;
    }

    std::string ShellExporter::getStatusMessage() const
    {
        std::lock_guard lock(m_statusMutex);
        return m_statusMessage;
    }

    void ShellExporter::performExport(ComputeCore& generator)
    {
        // Phase 1: Initialization (0% - 5%)
        m_progress.store(0.02, std::memory_order_release);

        if (isCancellationRequested())
        {
            m_state.store(State::Cancelled, std::memory_order_release);
            return;
        }

        generator.updateBBox();

        // Build thickness solution from LUTs
        std::size_t const numLayers = m_config.filamentStack.size();
        ThicknessSolution solution(numLayers);
        for (std::size_t i = 0; i < numLayers; ++i)
        {
            solution.thicknesses[i] = m_config.thicknessConstraints.minThickness;
        }

        m_progress.store(0.05, std::memory_order_release);

        // Phase 2: Shell generation (5% - 85%)
        {
            std::lock_guard lock(m_statusMutex);
            m_statusMessage = fmt::format("Generating {} shells...", numLayers);
        }

        ShellGenerator shellGenerator(generator, *const_cast<Document*>(m_document));
        auto shells = shellGenerator.generateShells(
            m_config.filamentStack,
            solution,
            m_config.mdcOptions,
            m_config.lutResolution,
            m_config.thicknessConstraints,
            &m_config.precomputedLuts);

        if (isCancellationRequested())
        {
            m_state.store(State::Cancelled, std::memory_order_release);
            return;
        }

        if (shells.empty())
        {
            throw std::runtime_error("Shell generation produced no meshes");
        }

        m_progress.store(0.85, std::memory_order_release);

        // Phase 3: Mesh construction (85% - 95%)
        {
            std::lock_guard lock(m_statusMutex);
            m_statusMessage = "Building mesh data...";
        }

        ComputeContext* context = generator.getComputeContext().get();
        std::vector<std::tuple<std::shared_ptr<Mesh>, std::string, Eigen::Vector3f>> meshesWithColors;
        meshesWithColors.reserve(shells.size());

        for (auto const& shell : shells)
        {
            if (isCancellationRequested())
            {
                m_state.store(State::Cancelled, std::memory_order_release);
                return;
            }

            auto mesh = std::make_shared<Mesh>(*context);

            for (std::size_t idx = 0; idx + 2 < shell.indices.size(); idx += 3)
            {
                auto const i0 = shell.indices[idx + 0];
                auto const i1 = shell.indices[idx + 1];
                auto const i2 = shell.indices[idx + 2];

                if (i0 >= shell.vertices.size() || i1 >= shell.vertices.size() || 
                    i2 >= shell.vertices.size())
                {
                    continue;
                }

                mesh->addFace(shell.vertices[i0], shell.vertices[i1], shell.vertices[i2]);
            }

            mesh->write();
            std::string name = fmt::format("Shell_L{}_{}", shell.layerIndex, shell.filamentName);

            Eigen::Vector3f color{1.0F, 1.0F, 1.0F};
            if (static_cast<std::size_t>(shell.layerIndex) < m_config.filamentStack.size())
            {
                color = m_config.filamentStack[shell.layerIndex].reflectanceColor;
            }

            meshesWithColors.emplace_back(std::move(mesh), std::move(name), color);
        }

        m_progress.store(0.95, std::memory_order_release);

        // Phase 4: Write 3MF (95% - 100%)
        {
            std::lock_guard lock(m_statusMutex);
            m_statusMessage = "Writing 3MF file...";
        }

        MeshWriter3mf writer(m_logger);
        writer.exportMeshesWithMaterialColors(m_targetFile, meshesWithColors, m_document, true);

        m_progress.store(1.0, std::memory_order_release);

        {
            std::lock_guard lock(m_statusMutex);
            m_statusMessage = "Export complete";
        }

        if (m_logger)
        {
            m_logger->addEvent({
                fmt::format("Exported {} shell meshes to {}", shells.size(), m_targetFile.string()),
                events::Severity::Info
            });
        }
    }

} // namespace gladius::io
```

## MeshExportDialog Integration

### Changes to `MeshExportDialog.h`

Add member:
```cpp
#include "io/ShellExporter.h"

// In private section:
io::ShellExporter m_shellExporter;
```

### Changes to `MeshExportDialog::exportShellsTo3mf()`

Replace entire function body with:
```cpp
void MeshExportDialog::exportShellsTo3mf(ComputeCore & core)
{
    if (m_document == nullptr)
    {
        throw std::runtime_error("Document is required for shell export");
    }

    auto stack = m_colorToThicknessDialog.getFilamentStack();
    if (stack.empty())
    {
        throw std::runtime_error("No materials defined for shell export");
    }

    int const lutResolution = m_colorToThicknessDialog.getLutResolution();
    auto const & precomputedLuts = m_colorToThicknessDialog.getPrecomputedLuts();

    // Build config
    io::ShellExportConfig config;
    config.filamentStack = std::move(stack);
    config.precomputedLuts = precomputedLuts;
    config.lutResolution = lutResolution;
    config.thicknessConstraints = m_colorToThicknessDialog.getConstraints();
    
    // Copy MDC options
    config.mdcOptions.qualityPreset = m_manifoldQualityPreset;
    config.mdcOptions.applyPreset();
    config.mdcOptions.enableGpu = m_manifoldEnableGpu;
    config.mdcOptions.enableCpuFallback = m_manifoldAllowCpuFallback;
    config.mdcOptions.enableCaching = m_manifoldEnableCaching;
    config.mdcOptions.isoValue = m_manifoldIsoValue;
    config.mdcOptions.maxDepth = m_manifoldMaxDepth;
    config.mdcOptions.minFeatureSize = m_manifoldMinFeatureSize;
    config.mdcOptions.enableChunking = m_manifoldEnableChunking;
    config.mdcOptions.enableHierarchicalOctree = m_manifoldEnableHierarchicalOctree;
    config.mdcOptions.enableSharpFeaturePostProcess = m_manifoldEnableSharpFeaturePostProcess;
    config.mdcOptions.sharpFeatureAngleThreshold = m_manifoldSharpFeatureAngleThreshold;
    config.mdcOptions.subdivisionIterations = m_manifoldSubdivisionIterations;
    config.mdcOptions.projectToSurface = m_manifoldProjectToSurface;
    config.mdcOptions.simplificationMethod = static_cast<io::SimplificationMethod>(m_manifoldSimplificationMethod);
    config.mdcOptions.enableSimplification = (m_manifoldSimplificationMethod != 0);
    config.mdcOptions.simplificationMaxSdfError = m_manifoldSimplificationMaxSdfError;
    config.mdcOptions.simplificationSdfWeight = m_manifoldSimplificationSdfWeight;
    config.mdcOptions.simplificationNormalWeight = m_manifoldSimplificationNormalWeight;

    m_shellExporter.setConfig(std::move(config));
    m_shellExporter.setDocument(m_document);
    m_shellExporter.setCancellationToken(&m_cancellationToken);
    m_shellExporter.beginExport(m_targetFile, core);
    m_activeExporter = &m_shellExporter;
}
```

## Test Skeleton

### File: `gladius/tests/unittests/io/ShellExporter_Test.cpp`

```cpp
#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "io/ShellExporter.h"

namespace gladius::io::tests
{
    class ShellExporter_Test : public ::testing::Test
    {
      protected:
        void SetUp() override
        {
            // Setup test fixtures
        }
    };

    TEST_F(ShellExporter_Test, BeginExport_WithEmptyStack_ThrowsException)
    {
        ShellExporter exporter;
        ShellExportConfig config;
        config.filamentStack.clear();
        exporter.setConfig(config);

        // Would need mock ComputeCore
        // EXPECT_THROW(exporter.beginExport(...), std::runtime_error);
        GTEST_SKIP() << "Requires mock ComputeCore";
    }

    TEST_F(ShellExporter_Test, GetProgress_AfterConstruction_ReturnsZero)
    {
        ShellExporter exporter;
        EXPECT_DOUBLE_EQ(0.0, exporter.getProgress());
    }

    TEST_F(ShellExporter_Test, HasError_AfterConstruction_ReturnsFalse)
    {
        ShellExporter exporter;
        EXPECT_FALSE(exporter.hasError());
    }

} // namespace gladius::io::tests
```

## CMake Integration

Add to `gladius/src/CMakeLists.txt`:
```cmake
# In the source list for gladius_lib or similar target
io/ShellExporter.cpp
io/ShellExporter.h
```

Add to `gladius/tests/unittests/CMakeLists.txt`:
```cmake
io/ShellExporter_Test.cpp
```
