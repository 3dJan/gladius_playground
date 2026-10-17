# Building Gladius on Windows

This guide walks you through building Gladius from source on Windows.
It is written for developers who may be unfamiliar with C++ build tooling —
prior knowledge of Python or another language is enough to follow along.

> **Note:** Gladius requires an OpenCL-capable GPU and OpenGL.
> It is tested on **Windows 10** and **Windows 11** (64-bit only).

---

## Table of Contents

1. [Install prerequisites](#1-install-prerequisites)
2. [vcpkg (vendored submodule)](#2-vcpkg-vendored-submodule)
3. [Environment variables (optional)](#3-environment-variables-optional)
4. [Clone the repository](#4-clone-the-repository)
5. [Configure and build](#5-configure-and-build)
6. [Verify the build](#6-verify-the-build)
7. [Getting started in VS Code](#7-getting-started-in-vs-code)
8. [Getting started in Visual Studio](#8-getting-started-in-visual-studio)
9. [Troubleshooting](#9-troubleshooting)

---

## 1. Install prerequisites

### Visual Studio 2022 or newer (required)

Gladius is built with the **MSVC v143** compiler toolset. Visual Studio 2022
ships it by default. Newer releases (for example Visual Studio 2026 / "18")
also work, but you must make sure the **MSVC v143** toolset component is
installed alongside the newest one — the custom vcpkg triplet
(`x64-windows-gladius.cmake`) pins `VCPKG_PLATFORM_TOOLSET v143`.

You can use either the full IDE or just the standalone Build Tools.

**Option A — Full Visual Studio** (includes the IDE):

Download from <https://visualstudio.microsoft.com/vs/>

During installation, select the workload:

- **Desktop development with C++**

This automatically includes MSVC, CMake, Ninja, and the Windows SDK. On
Visual Studio 2026 or newer, open the **Individual components** tab of the
installer and also enable **MSVC v143 - VS 2022 C++ x64/x86 build tools**.

**Option B — Build Tools only** (smaller, no IDE, VS Code is your editor):

Download "Build Tools for Visual Studio" from
<https://visualstudio.microsoft.com/visual-cpp-build-tools/>

Select the same workload: **Desktop development with C++** (plus the
MSVC v143 individual component on VS 2026 or newer).

> **Why v143 specifically?** The custom vcpkg triplet (`x64-windows-gladius.cmake`)
> hard-codes `VCPKG_PLATFORM_TOOLSET v143`. Using Visual Studio 2019 (v142)
> will cause vcpkg to fail when building dependencies with a toolset mismatch error.

### CMake and Ninja (if not using the Visual Studio installer)

If you chose Build Tools above, CMake and Ninja are included. If for any
reason they are missing:

```
winget install Kitware.CMake
winget install Ninja-build.Ninja
```

### Git

```
winget install Git.Git
```

After installation, open a new terminal so `git` is on your PATH.

### OpenCL

No separate OpenCL SDK install is needed: the OpenCL headers and ICD loader
come from the `opencl` port in the vcpkg manifest
(`gladius/vcpkg.json`), so `find_package(OpenCL)` resolves inside the vcpkg
install tree automatically.

At runtime, your GPU driver provides the actual OpenCL implementation. If no
OpenCL-capable device is available, configure with the
`x64-release-debug-noOpenCL` preset to skip the OpenCL tests.

---

## 2. vcpkg (vendored submodule)

vcpkg is the package manager that downloads and builds all C++ dependencies.
It is **vendored as a git submodule** at the repository root (`vcpkg/`), pinned
to the same commit as `builtin-baseline` in `gladius/vcpkg.json`, so there is
nothing to install separately — initialising the submodules (section 4) is
enough.

The CMake presets point at `../vcpkg/scripts/buildsystems/vcpkg.cmake`, and
`gladius/CMakeLists.txt` falls back to that submodule whenever `VCPKG_ROOT` is
not set.

**The first build will take 30–60 minutes** because vcpkg compiles all
dependencies from source — this is a one-time cost.

> **Bootstrap:** the submodule does not contain the `vcpkg.exe` binary (it is
> git-ignored). If `vcpkg\vcpkg.exe` is missing, build it once:
>
> ```powershell
> .\vcpkg\bootstrap-vcpkg.bat
> ```
>
> CMake invokes this for you if the submodule is present but not bootstrapped,
> so this is only needed if you want to run `vcpkg` from the command line.

---

## 3. Environment variables (optional)

Nothing has to be set for the default workflow — the presets already point at
the vendored submodule.

Set `VCPKG_ROOT` only if you want CMake to use a **different** vcpkg checkout
(a shared install, a newer baseline, a corporate mirror). An existing
`VCPKG_ROOT` always wins over the submodule:

```powershell
[System.Environment]::SetEnvironmentVariable("VCPKG_ROOT", "C:\vcpkg", "Machine")
```

> You must open a **new** terminal (and fully restart VS Code) after setting
> this for it to take effect.

**Verify:**

```powershell
echo $env:VCPKG_ROOT   # empty is fine — the submodule is used
```

> **Why this matters:** If the vcpkg submodule is not initialised *and*
> `VCPKG_ROOT` is unset, CMake cannot locate any third-party library and the
> configure step fails with errors like
> `Could not find a package configuration file provided by "fmt"`.

---

## 4. Clone the repository

Open a terminal (**Git Bash**, **PowerShell**, or **Developer Command Prompt**):

```bash
git clone --recurse-submodules https://github.com/3MFConsortium/gladius.git
cd gladius

# ...or, if you cloned without --recurse-submodules:
git submodule update --init --recursive
```

> **What the submodules provide:**
> - `vcpkg` — the pinned package manager used by the build
> - `gladius/components/IconFontCppHeaders` — Font Awesome icon constants for the UI
> - `gladius/components/psimpl` — polyline simplification
>
> If you forget this step you will see build errors like
> `fatal error: IconsFontAwesome5.h: No such file or directory`, or a configure
> error about the missing vcpkg toolchain file.

---

## 5. Configure and build

All build configuration is encoded in **CMake presets** defined in
`gladius/CMakePresets.json`. The recommended preset for development is
`x64-release-debug` — it compiles an optimised binary that still contains
full debug symbols.

### Option A: VS Code (recommended — see section 7)

Install VS Code and the CMake Tools extension, then select the
`x64-release-debug` preset from the status bar. See section 7 for details.

### Option B: Developer Command Prompt

Open the **"x64 Native Tools Command Prompt for VS"** from the Start menu
(this sets up MSVC compiler paths). Then:

```cmd
cd gladius\gladius

rem Configure (downloads and builds all vcpkg dependencies on first run):
cmake --preset x64-release-debug

rem Build:
cmake --build out\build\x64-release-debug --parallel 8
```

> **First-time configure:** vcpkg will now download and compile around 25
> libraries. This takes **30–60 minutes** on a typical machine. Subsequent
> builds reuse the vcpkg binary cache and are much faster.

The build output lives under:

```
gladius\out\build\x64-release-debug\
```

Key binaries:

| Path | What it is |
|---|---|
| `src\gladius.exe` | Main GUI application |
| `src\gladiusmcp.exe` | MCP server binary (agent control interface) |
| `tests\unittests\gladius_test.exe` | Unit + integration test suite |

---

## 6. Verify the build

The `ctest --preset ...` presets in `CMakePresets.json` point at the Linux
build trees, so on Windows run CTest directly from the build directory:

```cmd
cd gladius\gladius\out\build\x64-release-debug
ctest -L unit --output-on-failure
```

CTest labels come from `gtest_discover_tests`, so you can select a subset:

| Command | What it runs |
|---|---|
| `ctest -L unit --output-on-failure` | Fast unit tests (no GPU required) |
| `ctest -L integration --output-on-failure` | Integration tests (set `GLADIUS_RUN_GPU_TESTS=1` to include GPU paths) |
| `ctest -L api --output-on-failure` | API tests (MCP, external interfaces) |
| `ctest --output-on-failure` | Everything |

> Note: Windows has no `ReleaseWithDebug-noOpenCL` CTest preset. Use the
> `x64-release-debug-noOpenCL` *configure* preset instead, which sets
> `ENABLE_OPENCL_TESTS=OFF`.

---

## 7. Getting started in VS Code

VS Code is the recommended IDE because the repository ships pre-configured
tasks and launch configurations.

### Install VS Code

Download from <https://code.visualstudio.com/>

### Install VS Code extensions

Open VS Code in the repository root, then install:

- **C/C++** (`ms-vscode.cpptools`) — debugger and IntelliSense
- **CMake Tools** (`ms-vscode.cmake-tools`) — configure/build/test integration

### Select the kit and preset

1. Open the Command Palette (`Ctrl+Shift+P`)
2. Run **CMake: Select a Kit** → choose a **Visual Studio** kit with the
   **MSVC v143** toolset (for example "Visual Studio Build Tools 2022 (Release) - amd64",
   or the v143 kit offered by a newer Visual Studio)
3. Run **CMake: Select Configure Preset** → choose `x64-release-debug`
4. Run **CMake: Configure**

> **Kit vs Preset:** The "kit" tells CMake Tools which compiler to use (MSVC).
> The "preset" tells it which build flags and vcpkg triplet to use. Both are
> required.

After configuration, the **Build** button in the VS Code status bar builds the
project using Ninja.

### Use the pre-configured tasks

Open them via **Terminal → Run Task**. The Windows tasks are not pre-configured
in the repository's `.vscode/tasks.json` (which targets Linux), but you can
use CMake Tools' built-in Build and Test commands from the status bar or
Command Palette.

---

## 8. Getting started in Visual Studio

If you prefer the full Visual Studio IDE:

1. Open Visual Studio
2. Choose **Open a local folder** and select the `gladius/gladius` folder
3. Visual Studio detects `CMakePresets.json` automatically
4. In the configuration dropdown at the top, select **x64-release-debug**
5. **Build → Build All** (or press `Ctrl+Shift+B`)

Visual Studio will run the CMake configure step (including vcpkg dependency
download) and then build the project.

---

## 9. Troubleshooting

### Third-party packages not found

```
CMake Error: Could not find a package configuration file provided by "fmt"
```

The vcpkg submodule is almost certainly not initialised:

```powershell
git submodule update --init --recursive
.\vcpkg\bootstrap-vcpkg.bat
```

If you intentionally use a separate vcpkg checkout via `VCPKG_ROOT`:
1. Make sure you set it as a **system** variable (not just user-level) and
   opened a **new** terminal after setting it.
2. If using VS Code, restart VS Code entirely after setting the environment
   variable — it inherits env vars at launch time.

### MSVC toolset version mismatch

```
error MSB8020: The build tools for v142 cannot be found.
```

The custom triplet pins `VCPKG_PLATFORM_TOOLSET v143`, so the v143 toolset has
to be installed. Fix:

1. Open the Visual Studio Installer → **Modify**
2. Ensure the **Desktop development with C++** workload is installed, and on
   Visual Studio 2026 or newer also add the individual component
   **MSVC v143 - VS 2022 C++ x64/x86 build tools**
3. In VS Code, re-run **CMake: Select a Kit** and pick the v143 kit

### OpenCL not found at configure time

```
Could not find OpenCL
```

OpenCL comes from the `opencl` port in `gladius/vcpkg.json`, so this usually
means the vcpkg dependencies were not built yet (see the first item in this
section). As a fallback, pass the include/lib paths manually:
   ```
   -DOpenCL_INCLUDE_DIR="C:\path\to\opencl\include"
   -DOpenCL_LIBRARY="C:\path\to\opencl\lib\OpenCL.lib"
   ```

### GPU hangs / TDR (Timeout Detection and Recovery)

Windows kills GPU operations that take longer than ~2 seconds. Gladius uses
OpenCL for heavy compute tasks that can exceed this limit, especially during
the initial mesh compilation or with complex models.

Fix by increasing the TDR delay in the registry:

1. Open **Registry Editor** (`regedit`)
2. Navigate to `HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\GraphicsDrivers`
3. Create or modify the `TdrDelay` DWORD value → set to `60` (seconds)
4. Reboot

See also: <https://www.pugetsystems.com/labs/hpc/Working-around-TDR-in-Windows-for-a-better-GPU-computing-experience-777/>

### Multiple GPUs (notebook with integrated + discrete)

Gladius uses the first available OpenCL device, which may be the slower
integrated GPU on a notebook with both Intel HD and NVIDIA/AMD discrete
graphics. To force the discrete GPU, look at `gladius/src/compute/ComputeContext.cpp`
and adjust the device selection logic, or configure your GPU driver's control
panel to assign Gladius to the high-performance GPU.

### Long first build / vcpkg timeout

The first-time vcpkg dependency build (30–60 min) can fail if your network
connection drops or a package download times out. Simply re-run the CMake
configure step — vcpkg resumes from where it left off and only rebuilds
failed packages.

### AntiVirus interference

Some antivirus products flag or slow down the many small compiler invocations
vcpkg makes. If the first build is unusually slow or reports access denied
errors, temporarily add the vcpkg directory and the build output directory to
your antivirus exclusions.
