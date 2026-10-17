# Building Gladius on Linux

This guide walks you through building Gladius from source on a Linux system.
It is written for developers who may be unfamiliar with C++ build tooling —
prior knowledge of Python or another language is enough to follow along.

> **Note:** Gladius requires an OpenCL-capable GPU or a CPU-based OpenCL runtime.
> It is tested on **Debian 12 (Bookworm)** and **Ubuntu 22.04 / 24.04**.

---

## Table of Contents

1. [Install system packages](#1-install-system-packages)
2. [Choose and install an OpenCL runtime](#2-choose-and-install-an-opencl-runtime)
3. [vcpkg (vendored submodule)](#3-vcpkg-vendored-submodule)
4. [Environment variables (optional)](#4-environment-variables-optional)
5. [Clone the repository](#5-clone-the-repository)
6. [Configure and build](#6-configure-and-build)
7. [Verify the build](#7-verify-the-build)
8. [Getting started in VS Code](#8-getting-started-in-vs-code)
9. [Troubleshooting](#9-troubleshooting)

---

## 1. Install system packages

Gladius uses **CMake**, **Ninja**, and **Clang** as its build toolchain.
The CMake presets hardcode `/usr/bin/clang` and `/usr/bin/clang++` — if your
system only has GCC, you will need to install Clang (see troubleshooting).

vcpkg (the dependency manager, set up in step 3) handles almost all C++
libraries automatically. The packages below are the ones vcpkg cannot provide
because they are OS-level or GPU-driver-level components.

### Debian / Ubuntu

```bash
sudo apt update
sudo apt install -y \
    git curl zip unzip tar pkg-config \
    cmake ninja-build \
    clang \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev \
    libgl-dev \
    libomp-dev \
    opencl-headers ocl-icd-opencl-dev
```

> **What each group does:**
> - `git curl zip unzip tar pkg-config` — tools vcpkg needs to download and unpack its dependency sources
> - `cmake ninja-build clang` — the build toolchain
> - `libx11-dev libxrandr-*` — X11 windowing headers. Required at **build time** even on Wayland hosts because `CMakeLists.txt` calls `find_package(X11 REQUIRED)` unconditionally. At runtime GLFW uses whichever backend the display server provides (X11 or Wayland).
> - `libgl-dev` — OpenGL development headers
> - `libomp-dev` — OpenMP parallel processing (required by Gladius)
> - `opencl-headers ocl-icd-opencl-dev` — OpenCL API headers and the ICD loader (the glue layer between application and GPU driver)

### Fedora / RHEL

```bash
sudo dnf install -y \
    git curl zip unzip tar pkgconf-pkg-config \
    cmake ninja-build \
    clang \
    libX11-devel libXrandr-devel libXinerama-devel libXcursor-devel libXi-devel \
    mesa-libGL-devel \
    libomp-devel \
    opencl-headers ocl-icd-devel
```

---

## 2. Choose and install an OpenCL runtime

The packages above install the OpenCL *headers* and the ICD *loader*
(a dispatcher library). You still need an OpenCL *runtime* that talks to
your actual hardware. Pick the one that matches your GPU:

### NVIDIA GPU

Install the CUDA toolkit, which bundles an OpenCL runtime:

```bash
# Follow the official guide for your distro:
# https://developer.nvidia.com/cuda-downloads
# After installation, verify:
clinfo | grep "Platform Name"
```

### AMD GPU (discrete, RDNA/GCN)

Install ROCm:

```bash
# Follow: https://rocm.docs.amd.com/en/latest/deploy/linux/
# Quick check after install:
clinfo | grep "Platform Name"
```

### Intel GPU / integrated graphics

```bash
sudo apt install -y intel-opencl-icd
# or for newer Intel Arc / Xe hardware:
# sudo apt install -y intel-level-zero-gpu
```

### No discrete GPU / software fallback options

If you are working on a system without a supported GPU (e.g. a CI server or
a VM), two CPU-based OpenCL runtimes let you compile and run basic tests:

**Mesa Rusticl** (Mesa ≥ 23.1, recommended for integrated Intel/AMD graphics):

```bash
sudo apt install -y mesa-opencl-icd
# Enable Rusticl (needed on most distros):
export RUSTICL_ENABLE=softpipe   # or: llvmpipe, iris, radeonsi, etc.
```

> **Rusticl caveat:** Some Mesa versions have a known hang or crash when
> Gladius's command-stream preview compiles certain OpenCL programs. If the
> application hangs on startup, try setting `MESA_LOADER_DRIVER_OVERRIDE=softpipe`
> or switching to POCL below.

**POCL** (Portable Computing Language — pure software, any CPU):

```bash
sudo apt install -y pocl-opencl-icd
```

> POCL is the most portable option and works in any VM. It is slower than
> hardware runtimes and not suitable for interactive rendering, but it is
> sufficient for building and running unit tests.

**Verify your OpenCL setup** before continuing:

```bash
# Install clinfo if not already present:
sudo apt install -y clinfo
clinfo | head -20
```

You should see at least one platform and one device listed. If the output is
empty, your runtime is not installed or not visible to the ICD loader.

---

## 3. vcpkg (vendored submodule)

vcpkg is the package manager that downloads and builds all C++ dependencies
(ImGui, OpenVDB, Eigen3, lib3mf, Tracy, etc.). It is vendored in this repository
as a git submodule at `vcpkg/`, pinned to the same commit as the
`builtin-baseline` in `gladius/vcpkg.json`. **The first build will take 30–60
minutes** because vcpkg compiles everything from source — this is a one-time cost.

After cloning the repository (step 5), initialise and bootstrap the submodule:

```bash
git submodule update --init --recursive vcpkg

# Bootstrap the vcpkg binary (downloads a prebuilt vcpkg executable):
./vcpkg/bootstrap-vcpkg.sh
```

> **Using your own vcpkg instead:** if you prefer a separate checkout, set
> `VCPKG_ROOT` to its path (step 4). An explicit `VCPKG_ROOT` always takes
> precedence over the vendored submodule.

---

## 4. Environment variables (optional)

Nothing has to be exported for a default build: `gladius/CMakeLists.txt` falls
back to the vendored `vcpkg/` submodule when `VCPKG_ROOT` is unset.

Set `VCPKG_ROOT` only if you want CMake to use a different vcpkg checkout. Add
it to your shell's startup file (`~/.bashrc`, `~/.zshrc`, etc.) so it persists
across sessions:

```bash
export VCPKG_ROOT="$HOME/vcpkg"
# Reload the current shell:
source ~/.bashrc   # or: source ~/.zshrc
```

Verify it is set:

```bash
echo $VCPKG_ROOT
# Expected output: /home/<you>/vcpkg
```

> **Why this matters:** if neither `VCPKG_ROOT` nor the vendored submodule is
> available, CMake will not find any of the third-party libraries and the
> configure step will fail with errors like `Could not find a package
> configuration file provided by "fmt"`.

---

## 5. Clone the repository

```bash
git clone --recurse-submodules https://github.com/3MFConsortium/gladius.git
cd gladius

# Or, if you already cloned without submodules:
git submodule update --init --recursive
```

> **What the submodules provide:**
> - `vcpkg` — the pinned vcpkg package manager used by the build (see step 3)
> - `gladius/components/IconFontCppHeaders` — Font Awesome icon constants for the UI
> - `gladius/components/psimpl` — polyline simplification
>
> If you forget this step you will see build errors like
> `fatal error: IconsFontAwesome5.h: No such file or directory`, or CMake will
> report that it cannot find the vcpkg toolchain file.

---

## 6. Configure and build

All build configuration is encoded in **CMake presets** defined in
`gladius/CMakePresets.json`. The recommended preset for development is
`linux-releaseWithDebug` — it compiles an optimised binary that still
contains full debug symbols, matching what the VS Code tasks use.

```bash
cd gladius/gladius   # Note: the source tree is inside the repo subdirectory

# Configure (downloads and builds all vcpkg dependencies on first run):
cmake --preset linux-releaseWithDebug

# Build (use as many parallel jobs as you have CPU cores):
cmake --build out/build/linux-releaseWithDebug --parallel $(nproc)
```

> **First-time configure:** vcpkg will now download and compile around 25
> libraries. This takes **30–60 minutes** on a typical machine. Subsequent
> builds reuse the vcpkg binary cache and take only seconds for unchanged
> dependencies.

> **`nproc`** prints the number of logical CPU cores. You can also pass a
> fixed number like `--parallel 8`.

The build output lives under:

```
gladius/out/build/linux-releaseWithDebug/
```

Key binaries:

| Path | What it is |
|---|---|
| `src/gladius` | Main GUI application |
| `src/gladiusmcp` | MCP server binary (agent control interface) |
| `tests/unittests/gladius_test` | Unit + integration test suite |

---

## 7. Verify the build

Run the fast unit tests (no GPU required):

```bash
cd gladius/gladius
ctest --preset UnitTests --output-on-failure
```

Run the full test suite (GPU required):

```bash
GLADIUS_RUN_GPU_TESTS=1 ctest --preset ReleaseWithDebug --output-on-failure
```

If you do not have a GPU, use the no-OpenCL preset instead:

```bash
cmake --preset linux-releaseWithDebug-noOpenCL
cmake --build out/build/linux-releaseWithDebug-noOpenCL --parallel $(nproc)
ctest --preset ReleaseWithDebug-noOpenCL --output-on-failure
```

---

## 8. Getting started in VS Code

VS Code is the recommended IDE because the repository ships pre-configured
tasks and launch configurations for it.

### Install VS Code extensions

Open VS Code in the repository root, then install:

- **C/C++** (`ms-vscode.cpptools`) — debugger and IntelliSense
- **CMake Tools** (`ms-vscode.cmake-tools`) — configure/build/test integration
- **clangd** (`llvm-vs-code-extensions.vscode-clangd`) — fast code completion and diagnostics (recommended over the built-in IntelliSense for this project)

### Select the CMake preset

1. Open the Command Palette (`Ctrl+Shift+P`)
2. Run **CMake: Select Configure Preset** → choose `linux-releaseWithDebug`
3. Run **CMake: Configure**

After configuration, the **Build** button in the VS Code status bar will build
the project using Ninja.

### Use the pre-configured tasks

The repository includes VS Code tasks for the most common operations. Open
them via **Terminal → Run Task**:

| Task | What it does |
|---|---|
| Build ALL (linux-releaseWithDebug) | Full rebuild |
| Build incremental | Incremental build (fast) |
| Run Gladius Tests (ReleaseWithDebug, summary) | Run the full test suite |

### Debugging

The repository includes a launch configuration (`cppdbg: gladius`). Press
**F5** or use the Run & Debug panel to start a debug session with LLDB/GDB.

---

## 9. Troubleshooting

### `clang: command not found` or wrong Clang path

The preset hardcodes `/usr/bin/clang` and `/usr/bin/clang++`. If Clang is
installed at a different path (common on some distros where `clang` defaults
to a version-suffixed binary like `clang-16`):

```bash
# Check what is at /usr/bin/clang:
ls -la /usr/bin/clang*

# If only clang-16 exists, create a symlink or install the unversioned package:
sudo apt install -y clang   # installs the default version with the unversioned name
# or manually symlink:
sudo update-alternatives --install /usr/bin/clang clang /usr/bin/clang-16 100
sudo update-alternatives --install /usr/bin/clang++ clang++ /usr/bin/clang++-16 100
```

### vcpkg is not being used (`Could not find a package configuration file`)

```
CMake Error: Could not find a package configuration file provided by "fmt"
```

CMake uses `$VCPKG_ROOT` when it is set, and falls back to the vendored `vcpkg/`
submodule otherwise. Check both:

```bash
echo $VCPKG_ROOT                                  # either empty, or your vcpkg path
ls vcpkg/scripts/buildsystems/vcpkg.cmake         # the vendored fallback
```

If the submodule directory is empty, initialise it (`git submodule update --init --recursive vcpkg`)
and bootstrap it (`./vcpkg/bootstrap-vcpkg.sh`). If `VCPKG_ROOT` points somewhere
unexpected, unset it to go back to the vendored copy.

### vcpkg fails during dependency compilation

vcpkg prints detailed logs to:
```
~/.cache/vcpkg/buildtrees/<package>/
```

Common causes:
- Missing system headers (re-run the `apt install` command from step 1)
- Disk space: a full build of all dependencies uses ~10 GB of temporary space

### OpenCL device not found at runtime

```
No OpenCL platform found
```

1. Run `clinfo` — if it shows no devices, the runtime is not installed or not
   registered with the ICD loader.
2. Check that your runtime's `.icd` file is present:
   ```bash
   ls /etc/OpenCL/vendors/
   ```
   Each installed runtime should have a file here (e.g. `nvidia.icd`,
   `intel.icd`, `rusticl.icd`).
3. For Rusticl, ensure you exported `RUSTICL_ENABLE=<driver>`.

### lib3mf shared library not found at runtime

```
error while loading shared libraries: lib3mf.so.2: cannot open shared object file
```

The build system sets up the RPATH so that binaries in the build tree find
the vcpkg-built `lib3mf.so.2` automatically. If this error still appears,
check that you are running the binary from the build tree and not a copy
placed elsewhere. For installed packages the library is bundled alongside
the executable in `/opt/gladius/`.

### Build hangs / crashes with Rusticl

Some Mesa Rusticl builds hang when Gladius's command-stream preview compiles
a particular OpenCL program. Workarounds:

```bash
# Try the software pipe (slower, but more stable):
export RUSTICL_ENABLE=softpipe

# Or switch to POCL entirely for local development:
sudo apt install -y pocl-opencl-icd
```

### GCC instead of Clang

The project is not tested with GCC on Linux. If you need to use GCC, copy
the `linux-releaseWithDebug` preset in `CMakePresets.json`, add it to a local
`CMakeUserPresets.json` (which is git-ignored), and override
`CMAKE_C_COMPILER` and `CMAKE_CXX_COMPILER`. Note that GCC may produce
different warnings or errors and is not supported by the maintainers.
