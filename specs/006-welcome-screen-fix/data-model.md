# Data Model: Welcome Screen Improvements

**Feature**: 006-welcome-screen-fix  
**Phase**: 1 - Design  
**Date**: January 3, 2026

## Entity Definitions

### ThumbnailInfo (Modified)

Represents a file's thumbnail state. Extended with async loading support.

```
ThumbnailInfo
├── filePath: filesystem::path         # Path to the 3MF file
├── fileName: string                   # Name of the file (without extension)
├── thumbnailData: vector<uint8>       # Raw PNG data (cleared after texture creation)
├── decodedPixels: vector<uint8>       # Decoded RGBA pixels (NEW - for async loading)
├── hasThumbnail: bool                 # Whether the file has a thumbnail
├── thumbnailLoaded: bool              # Whether the thumbnail data has been loaded
├── textureCreated: bool               # Whether the GL texture has been created (NEW)
├── thumbnailTextureId: GLuint         # OpenGL texture ID
├── thumbnailWidth: uint               # Width of the thumbnail
├── thumbnailHeight: uint              # Height of the thumbnail
├── timestamp: time_t                  # Last modified timestamp
├── fileInfo: ThreemfFileInfo          # Additional file metadata
└── loadState: ThumbnailLoadState      # Current loading state (NEW)
```

### ThumbnailLoadState (New)

Enumeration for thumbnail loading states.

```
ThumbnailLoadState
├── NotStarted     # Load not initiated
├── Loading        # Background load in progress
├── DecodedPending # Decoded, waiting for texture creation
├── Ready          # Texture created, ready to display
└── Failed         # Load failed (show placeholder)
```

### AsyncThumbnailLoader (New)

Component responsible for background thumbnail loading.

```
AsyncThumbnailLoader
├── m_pendingLoads: vector<ThumbnailLoadTask>   # Active load tasks
├── m_maxConcurrentLoads: size_t                # Max parallel loads (default: 4)
├── m_logger: SharedLogger                      # Error reporting
│
├── requestLoad(ThumbnailInfo&): void           # Queue a thumbnail for loading
├── cancelAll(): void                           # Cancel pending loads
├── processPendingTextures(): void              # Create textures (call on main thread)
├── hasPendingWork(): bool                      # Check if work is in progress
└── update(): void                              # Poll futures, update states
```

### ThumbnailLoadTask (New)

Represents a single async load operation.

```
ThumbnailLoadTask
├── info: ThumbnailInfo*                        # Pointer to info being loaded
├── future: future<ThumbnailLoadResult>         # Async operation handle
└── startTime: steady_clock::time_point         # For timeout tracking
```

### ThumbnailLoadResult (New)

Result of a background thumbnail load operation.

```
ThumbnailLoadResult
├── success: bool                      # Whether extraction succeeded
├── decodedPixels: vector<uint8>       # Decoded RGBA pixel data
├── width: uint                        # Image width
├── height: uint                       # Image height
└── errorMessage: string               # Error description if failed
```

### WelcomeScreen (Modified)

Extended with async loader and file selection guard.

```
WelcomeScreen (additions only)
├── m_asyncLoader: unique_ptr<AsyncThumbnailLoader>  # Background loader
├── m_pendingFileOpen: optional<path>                # File to open (race condition fix)
├── m_clickProcessed: bool                           # Prevent double-click handling
│
├── processFileOpen(): optional<path>                # Get and clear pending file
└── hasPendingFileOpen(): bool                       # Check if file open is pending
```

## State Transitions

### Thumbnail Loading State Machine

```
                    ┌─────────────┐
                    │ NotStarted  │
                    └──────┬──────┘
                           │ requestLoad()
                           ▼
                    ┌─────────────┐
              ┌─────│   Loading   │─────┐
              │     └──────┬──────┘     │
              │            │            │
         error│            │ success    │ cancel
              │            ▼            │
              │     ┌─────────────────┐ │
              │     │ DecodedPending  │ │
              │     └────────┬────────┘ │
              │              │          │
              │              │ createTexture()
              │              ▼          │
              │       ┌───────────┐     │
              │       │   Ready   │     │
              │       └───────────┘     │
              │                         │
              ▼                         ▼
        ┌──────────┐            ┌───────────┐
        │  Failed  │            │ NotStarted│
        └──────────┘            └───────────┘
```

### Welcome Screen Close Flow (Fixed)

```
User clicks thumbnail
        │
        ▼
┌───────────────────────┐
│ Store path in         │
│ m_pendingFileOpen     │
└───────────┬───────────┘
            │
            ▼
┌───────────────────────┐
│ Set m_isVisible=false │
│ (single location)     │
└───────────┬───────────┘
            │
            ▼
┌───────────────────────┐
│ MainWindow detects    │
│ welcome screen closed │
└───────────┬───────────┘
            │
            ▼
┌───────────────────────┐
│ Check pendingFileOpen │
│ If set: open(path)    │
│ If empty: do nothing  │
└───────────────────────┘
```

## Relationships

```
MainWindow
    │
    ├── owns ──► WelcomeScreen
    │                │
    │                ├── owns ──► AsyncThumbnailLoader
    │                │                │
    │                │                └── manages ──► ThumbnailLoadTask[]
    │                │
    │                └── contains ──► ThumbnailInfo[]
    │
    └── calls ──► open(path) when pendingFileOpen is set
```

## Validation Rules

1. **ThumbnailLoadState transitions**: Only valid transitions allowed (see state machine)
2. **m_pendingFileOpen**: Must be cleared after being consumed by MainWindow
3. **Texture creation**: Only allowed when loadState == DecodedPending
4. **Concurrent loads**: Never exceed m_maxConcurrentLoads active futures
5. **Click handling**: m_clickProcessed prevents multiple clicks on same frame
