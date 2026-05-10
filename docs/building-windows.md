# Building Gladius on Windows

This guide walks you through building Gladius from source on Windows.
It is written for developers who may be unfamiliar with C++ build tooling —
prior knowledge of Python or another language is enough to follow along.

> **Note:** Gladius requires an OpenCL-capable GPU and OpenGL.
> It is tested on **Windows 10** and **Windows 11** (64-bit only).

---

## Table of Contents

1. [Install prerequisites](#1-install-prerequisites)
2. [Install vcpkg](#2-install-vcpkg)
3. [Set environment variables](#3-set-environment-variables)
4. [Clone the repository](#4-clone-the-repository)
5. [Configure and build](#5-configure-and-build)
6. [Verify the build](#6-verify-the-build)
7. [Getting started in VS Code](#7-getting-started-in-vs-code)
8. [Getting started in Visual Studio](#8-getting-started-in-visual-studio)
9. [Troubleshooting](#9-troubleshooting)

---

## 1. Install prerequisites

### Visual Studio 2022 (required)

Gladius requires the **MSVC v143** compiler toolset (shipped with Visual Studio 2022).
You can use either the full IDE or just the standalone Build Tools.

**Option A — Full Visual Studio 2022** (includes the IDE):

Download from <https://visualstudio.microsoft.com/vs/>

During installation, select the workload:

- **Desktop development with C++**

This automatically includes MSVC v143, CMake, Ninja, and the Windows SDK.

**Option B — Build Tools only** (smaller, no IDE, VS Code is your editor):

Download "Build Tools for Visual Studio 2022" from
<https://visualstudio.microsoft.com/visual-cpp-build-tools/>

Select the same workload: **Desktop development with C++**

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

### OpenCL SDK

The Khronos OpenCL SDK provides the headers and ICD loader needed to compile
OpenCL code. At runtime, your GPU driver provides the actual implementation.

Download and install the **Khronos OpenCL SDK**:
<https://github.com/KhronosGroup/OpenCL-SDK/releases>

> Alternatively, installing the **CUDA Toolkit** (NVIDIA) or **ROCm for Windows**
> (AMD) also installs OpenCL support for that GPU. The Khronos SDK is vendor-neutral
> and works regardless of which GPU you have.

After installing, the environment variable `OPENCL_ROOT` (or `OCL_ROOT`)
should point to the SDK. CMake will pick it up automatically via `find_package(OpenCL)`.

---

## 2. Install vcpkg

vcpkg is the package manager that downloads and builds all C++ dependencies.
**The first build will take 30–60 minutes** because vcpkg compiles everything
from source — this is a one-time cost.

Open **PowerShell** (or the new Windows Terminal) and run:

```powershell
# Clone vcpkg to a permanent location (e.g. C:\vcpkg):
git clone https://github.com/microsoft/vcpkg.git C:\vcpkg

# Bootstrap (compiles the vcpkg executable):
C:\vcpkg\bootstrap-vcpkg.bat
```

---

## 3. Set environment variables

CMake and vcpkg need to know where vcpkg lives. Set this as a **system-wide**
environment variable so it persists across terminal sessions and IDE restarts.

**Via Settings UI:**

1. Open **Start** → search for **"Edit the system environment variables"**
2. Click **Environment Variables…**
3. Under **System variables**, click **New**:
   - Variable name: `VCPKG_ROOT`
   - Variable value: `C:\vcpkg` (or wherever you cloned vcpkg)
4. Click OK and close all dialogs

**Via PowerShell (permanent, system-wide):**

```powershell
[System.Environment]::SetEnvironmentVariable("VCPKG_ROOT", "C:\vcpkg", "Machine")
```

> You must open a **new** terminal after setting this for it to take effect.

**Verify:**

```powershell
echo $env:VCPKG_ROOT
# Expected: C:\vcpkg
```

> **Why this matters:** The CMake presets reference `$env{VCPKG_ROOT}` to
> locate the vcpkg toolchain file. If it is unset, CMake will not find any
> third-party libraries and the configure step fails with errors like
> `Could not find a package configuration file provided by "fmt"`.

---

## 4. Clone the repository

Open a terminal (**Git Bash**, **PowerShell**, or **Developer Command Prompt**):

```bash
git clone https://github.com/3MFConsortium/gladius.git
cd gladius

# Initialize the two vendored header-only libraries:
git submodule update --init --recursive
```

> **What the submodules provide:**
> - `gladius/components/IconFontCppHeaders` — Font Awesome icon constants for the UI
> - `gladius/components/psimpl` — polyline simplification
>
> If you forget this step you will see build errors like
> `fatal error: IconsFontAwesome5.h: No such file or directory`.

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

Open the **"x64 Native Tools Command Prompt for VS 2022"** from the Start menu
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

Run the fast unit tests (no GPU required for most):

```cmd
cd gladius\gladius
ctest --preset ReleaseWithDebug --output-on-failure
```

> Note: Windows does not have the `ReleaseWithDebug-noOpenCL` CTest preset
> listed in `CMakePresets.json`. To skip OpenCL tests, configure with
> `-DENABLE_OPENCL_TESTS=OFF` added to the CMake configure step.

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
2. Run **CMake: Select a Kit** → choose **Visual Studio Community 2022 Release - amd64**
   (or the Build Tools equivalent)
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

1. Open Visual Studio 2022
2. Choose **Open a local folder** and select the `gladius/gladius` folder
3. Visual Studio detects `CMakePresets.json` automatically
4. In the configuration dropdown at the top, select **x64-release-debug**
5. **Build → Build All** (or press `Ctrl+Shift+B`)

Visual Studio will run the CMake configure step (including vcpkg dependency
download) and then build the project.

---

## 9. Troubleshooting

### `VCPKG_ROOT` is not set

```
CMake Error: Could not find a package configuration file provided by "fmt"
```

Check:

```powershell
echo $env:VCPKG_ROOT
```

If empty:
1. Make sure you set it as a **system** variable (not just user-level) and
   opened a **new** terminal after setting it.
2. If using VS Code, restart VS Code entirely after setting the environment
   variable — it inherits env vars at launch time.

### MSVC toolset version mismatch

```
error MSB8020: The build tools for v142 cannot be found.
```

This means Visual Studio 2019 (v142) Build Tools are being used instead of
2022 (v143). Fix:

1. Open the Visual Studio Installer
2. Ensure **Visual Studio 2022** (or Build Tools 2022) is installed with the
   **Desktop development with C++** workload
3. In VS Code, re-run **CMake: Select a Kit** and pick the 2022 option

### OpenCL not found at configure time

```
Could not find OpenCL
```

1. Ensure the Khronos OpenCL SDK (or CUDA toolkit) is installed
2. Reopen your terminal/VS Code after installing so the new environment
   variables (`OPENCL_ROOT`, `CUDA_PATH`, etc.) are visible
3. As a fallback, pass the include/lib paths manually:
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
