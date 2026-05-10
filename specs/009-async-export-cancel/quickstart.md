# Quickstart: Async Export Cancellation

**Feature**: 009-async-export-cancel  
**Date**: 2025-01-07

## Overview

This guide provides step-by-step implementation instructions for making export cancellation non-blocking with instant UI feedback.

## Prerequisites

- Gladius development environment set up
- Familiarity with `std::atomic`, `std::future`, and ImGui
- Understanding of current export dialog flow

## Implementation Order

### Phase 1: Core Infrastructure (CancellationToken)

**File**: `gladius/src/io/CancellationToken.h` (NEW)

```cpp
#pragma once

#include <atomic>

namespace gladius::io
{
    /// @brief Thread-safe cooperative cancellation signal
    ///
    /// Lightweight wrapper around atomic bool for communicating cancellation
    /// requests from UI thread to worker threads.
    class CancellationToken
    {
    public:
        /// @brief Request cancellation (called from UI thread)
        void requestCancellation()
        {
            m_cancelled.store(true, std::memory_order_release);
        }

        /// @brief Check if cancellation was requested (called from worker thread)
        [[nodiscard]] bool isCancelled() const
        {
            return m_cancelled.load(std::memory_order_acquire);
        }

        /// @brief Reset for reuse (called before new export)
        void reset()
        {
            m_cancelled.store(false, std::memory_order_release);
        }

    private:
        std::atomic<bool> m_cancelled{false};
    };

} // namespace gladius::io
```

### Phase 2: Extend ExportState

**File**: `gladius/src/ui/ExportState.h` (MODIFY)

Add phase enum and tracking:

```cpp
enum class ExportPhase
{
    Idle,
    Exporting,
    Cancelling
};

class ExportState
{
public:
    void beginExport(std::string description = "Mesh export")
    {
        m_exportDescription = std::move(description);
        m_phase.store(ExportPhase::Exporting);
        m_exportInProgress = true;
    }

    void beginCancellation()
    {
        m_phase.store(ExportPhase::Cancelling);
    }

    void endExport()
    {
        m_phase.store(ExportPhase::Idle);
        m_exportInProgress = false;
        m_exportDescription.clear();
    }

    [[nodiscard]] bool isCancelling() const
    {
        return m_phase.load() == ExportPhase::Cancelling;
    }

    [[nodiscard]] ExportPhase getPhase() const
    {
        return m_phase.load();
    }

    // ... existing methods ...

private:
    std::atomic<ExportPhase> m_phase{ExportPhase::Idle};
    // ... existing members ...
};
```

### Phase 3: Extend IExporter Interface

**File**: `gladius/src/io/IExporter.h` (MODIFY)

Add optional cancellation token support:

```cpp
#include "CancellationToken.h"  // Add include

class IExporter
{
public:
    virtual ~IExporter() = default;

    virtual void beginExport(const std::filesystem::path & fileName,
                             ComputeCore & generator) = 0;
    virtual bool advanceExport(ComputeCore & generator) = 0;
    virtual void finalize() = 0;
    [[nodiscard]] virtual double getProgress() const = 0;

    /// @brief Set cancellation token for cooperative abort
    /// @param token Pointer to token (may be nullptr to disable)
    virtual void setCancellationToken(CancellationToken * token)
    {
        m_cancellationToken = token;
    }

protected:
    /// @brief Check if cancellation was requested
    [[nodiscard]] bool isCancellationRequested() const
    {
        return m_cancellationToken != nullptr && m_cancellationToken->isCancelled();
    }

    CancellationToken * m_cancellationToken{nullptr};
};
```

### Phase 4: Add Cancellation Checks to ManifoldDualContouringStlExporter

**File**: `gladius/src/io/ManifoldDualContouringStlExporter.cpp` (MODIFY)

In `performExport()`, add checks at key points:

```cpp
void ManifoldDualContouringStlExporter::performExport(ComputeCore & generator)
{
    // Early exit check
    if (isCancellationRequested())
    {
        m_state = State::Idle;  // or add State::Cancelled
        return;
    }

    if (m_targetFile.empty())
    {
        throw std::runtime_error("No output filename specified for STL export");
    }

    if (!generator.updateBBox())
    {
        throw std::runtime_error("...");
    }

    // Check after bbox computation
    if (isCancellationRequested())
    {
        m_state = State::Idle;
        return;
    }

    // ... mesh generation ...

    // Check after mesh generation (most expensive step)
    if (isCancellationRequested())
    {
        m_state = State::Idle;
        return;
    }

    // Write file only if not cancelled
    writeMeshToFile(generator, positions, indices, normals);
    
    // If cancelled during write, delete partial file
    if (isCancellationRequested())
    {
        std::filesystem::remove(m_targetFile);
        m_state = State::Idle;
        return;
    }
}
```

Add a new `State::Cancelled` to distinguish from normal completion if needed.

### Phase 5: Update MeshExportDialog for Non-Blocking Cancel

**File**: `gladius/src/ui/MeshExportDialog.h` (MODIFY)

Add token member:

```cpp
#include "io/CancellationToken.h"

class MeshExportDialog : public BaseExportDialog
{
    // ... existing ...

private:
    io::CancellationToken m_cancellationToken;
};
```

**File**: `gladius/src/ui/MeshExportDialog.cpp` (MODIFY)

Update `startExport()`:

```cpp
void MeshExportDialog::startExport(ComputeCore & core)
{
    m_cancellationToken.reset();  // Clear any previous cancellation
    m_activeExporter->setCancellationToken(&m_cancellationToken);
    // ... rest of existing code ...
}
```

Update `onExportCancelled()`:

```cpp
void MeshExportDialog::onExportCancelled()
{
    // Immediate UI feedback
    if (m_exportState != nullptr)
    {
        m_exportState->beginCancellation();
    }
    m_statusMessage = "Cancelling...";

    // Signal worker thread (non-blocking)
    m_cancellationToken.requestCancellation();

    // Do NOT call finalize() here - let advanceExport handle completion
    // Do NOT call resetState() yet - wait for worker to exit
}
```

Update render loop to handle cancellation completion:

```cpp
void MeshExportDialog::render(ComputeCore & core)
{
    // ... existing code ...

    // Check for completion (including cancellation)
    if (m_exportInProgress && m_activeExporter != nullptr)
    {
        if (!m_activeExporter->advanceExport(core))
        {
            // Export finished (normally or via cancellation)
            bool wasCancelled = m_cancellationToken.isCancelled();
            
            finalizeExport();
            
            if (wasCancelled)
            {
                m_statusMessage = "Export cancelled";
                // Cleanup partial file if it exists
                if (std::filesystem::exists(m_targetFile))
                {
                    std::filesystem::remove(m_targetFile);
                }
            }
            else
            {
                onExportCompleted();
            }
            
            resetExportState();
        }
    }

    // ... render Cancel button ...
    if (m_exportInProgress)
    {
        bool isCancelling = m_exportState && m_exportState->isCancelling();
        
        if (isCancelling)
        {
            ImGui::BeginDisabled();
            ImGui::Button("Cancelling...");
            ImGui::EndDisabled();
        }
        else if (ImGui::Button("Cancel"))
        {
            onExportCancelled();
        }
    }
}
```

## Testing

### Unit Tests

**File**: `gladius/tests/unittests/CancellationToken_tests.cpp` (NEW)

```cpp
#include <gtest/gtest.h>
#include "io/CancellationToken.h"

namespace gladius::io::tests
{

TEST(CancellationToken, InitiallyNotCancelled)
{
    CancellationToken token;
    EXPECT_FALSE(token.isCancelled());
}

TEST(CancellationToken, RequestCancellation_SetsCancelledFlag)
{
    CancellationToken token;
    token.requestCancellation();
    EXPECT_TRUE(token.isCancelled());
}

TEST(CancellationToken, Reset_ClearsCancelledFlag)
{
    CancellationToken token;
    token.requestCancellation();
    token.reset();
    EXPECT_FALSE(token.isCancelled());
}

} // namespace gladius::io::tests
```

### Integration Tests

**File**: `gladius/tests/integrationtests/ExportCancellation_tests.cpp` (NEW)

Test that cancellation:
1. Returns from advanceExport within reasonable time
2. Cleans up partial files
3. Allows immediate restart of new export

## Build & Verify

```bash
# Build
# Use VS Code task: "Build ALL (linux-releaseWithDebug)"

# Run unit tests
cd gladius/out/build/linux-releaseWithDebug/tests/unittests
./gladius_test --gtest_filter=CancellationToken*

# Run integration tests (requires GPU)
GLADIUS_RUN_GPU_TESTS=1 ./gladius_test --gtest_filter=ExportCancellation*
```

## Checklist

- [ ] Create `CancellationToken.h`
- [ ] Extend `ExportState` with Cancelling phase
- [ ] Add `setCancellationToken()` to `IExporter`
- [ ] Add cancellation checks to `ManifoldDualContouringStlExporter::performExport()`
- [ ] Add cancellation checks to `DualContouringStlExporter`
- [ ] Add cancellation checks to `MeshExporter` (if layer-based exports need it)
- [ ] Update `MeshExportDialog` to use non-blocking cancel flow
- [ ] Update Cancel button UI to show "Cancelling..." state
- [ ] Add partial file cleanup
- [ ] Write unit tests for `CancellationToken`
- [ ] Write unit tests for `ExportState` Cancelling phase
- [ ] Write integration tests for cancel flow
- [ ] Manual testing: verify UI responsiveness during cancellation
