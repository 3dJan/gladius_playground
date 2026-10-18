# Cross-compiling Gladius for Steam Frame

Steam Frame uses a Snapdragon 8 Gen 3 (ARM64) and SteamOS 3. This guide's CMake preset provides the cross-compilation wiring, but **a matching Steam Frame development sysroot/toolchain is still required**. SteamOS is Arch-based; a Debian `arm64` sysroot or the Steam Deck image is not a substitute for Frame's userspace.

The initial Gladius profile is WebGPU-only with OpenVDB disabled. It still builds Gladius's native desktop UI and requires X11, GLFW, OpenMP, and Vulkan development/runtime support. Confirm these are available in Valve's Frame SDK and supported by the Frame runtime before investing in the sysroot: the current application is not a VR-shell/OpenXR app.

## Install host tools

On the Debian x86_64 build machine:

```sh
sudo apt update
sudo apt install -y \
    git curl zip unzip tar pkg-config \
    cmake ninja-build clang lld llvm \
    crossbuild-essential-arm64
```

The cross-essential package provides a generic AArch64 GCC toolchain that Clang can use for compiler support and binutils. It does **not** provide Steam Frame's target libc, C++ runtime, graphics libraries, or sysroot. Use the compiler/runtime supplied by the Frame SDK when Valve provides one; otherwise set `GLADIUS_ARM64_GCC_TOOLCHAIN_ROOT` to the appropriate SDK toolchain root.

Initialize and bootstrap the repository's vcpkg submodule if needed:

```sh
git submodule update --init --recursive vcpkg
./vcpkg/bootstrap-vcpkg.sh
```

## Obtain a Steam Frame sysroot first

Use a Valve-provided Steam Frame SDK/sysroot or developer image if available to your Steamworks/Frame developer account. It must match the Frame's ARM64 SteamOS userspace, including its libc and C++ runtime. Do not point the build at the x86_64 host's `/usr`, and do not populate the target with Debian `:arm64` libraries just because they have the right CPU architecture.

The public SteamOS source and package links are useful for checking licenses and package provenance, but do not currently provide a confirmed Frame ARM64 sysroot:

- Valve's [SteamOS package mirror](https://steamdeck-packages.steamos.cloud/archlinux-mirror/) lists only `x86_64` under the public `core-3.8.1x` and `holo-3.8.1x` channels checked for this guide.
- Valve's [SteamOS source repositories](https://gitlab.steamos.cloud/) are the source for open-source components, subject to their individual licenses; they are not a ready-made cross SDK.
- Valve provides a separate [Steam Frame recovery image](https://store.steampowered.com/steamos/download/?ver=steamframe). It is recovery media, not documented as a development sysroot. Its EULA restricts copying, modifying, and separating components of the backup image; do not extract or redistribute a sysroot from it without authorization from Valve.

If you have authorized shell access to a Frame, record the target details so the SDK can be matched:

```sh
uname -m
cat /etc/os-release
getconf GNU_LIBC_VERSION
```

The SDK/sysroot must supply target headers and libraries for the system dependencies Gladius actually probes: OpenMP, X11 (including Xrandr, Xinerama, Xcursor, and Xi), and Vulkan for Dawn's Vulkan backend. Use the package names and package manager for the Frame SDK; the Debian package names from the native Linux guide do not necessarily apply. The target also needs a Vulkan driver/runtime supported by its GPU.

## Configure and build

Once you have the Frame SDK, set the sysroot and toolchain paths it documents, then configure from the `gladius/` source directory:

```sh
export GLADIUS_ARM64_SYSROOT=/opt/steamframe-sdk/sysroot
# If the SDK's cross-toolchain is not rooted at /usr, also set:
# export GLADIUS_ARM64_GCC_TOOLCHAIN_ROOT=/opt/steamframe-sdk/toolchain
cd gladius
cmake --preset linux-arm64-webgpu-releaseWithDebug
cmake --build --preset linux-arm64-webgpu-releaseWithDebug \
    --target gladius gladiusmcp --parallel 8
```

The toolchain sets the Clang target triple to `aarch64-linux-gnu`, uses the `arm64-linux` vcpkg target triplet and `x64-linux` host triplet, and restricts package/header/library searches to target locations. Override `GLADIUS_ARM64_GCC_TOOLCHAIN_ROOT` with the SDK's toolchain root when appropriate. Confirm that `pkg-config` resolves `Clipper2` and `libcoro` from the ARM64 vcpkg prefix, not from host system paths.

Build application targets explicitly at first. The normal test targets use post-build GoogleTest discovery, which attempts to execute the built test binary on the build host. To build/run tests, configure a suitable `CMAKE_CROSSCOMPILING_EMULATOR` or run them on the ARM64 device.

Verify the output architecture before deploying:

```sh
readelf -h out/build/linux-arm64-webgpu-releaseWithDebug/src/gladius | grep Machine
```

## Notes and limitations

- The [Steam Frame product page](https://store.steampowered.com/sale/steamframe) identifies the device as Snapdragon 8 Gen 3 / ARM64. Valve's [SteamOS install/recovery instructions](https://help.steampowered.com/en/faqs/view/65B4-2AA3-5F37-4227) offer a Frame-specific recovery image, but this investigation did not find a public Frame ARM64 development sysroot or SDK. Ask Valve/Steamworks for the supported developer toolchain and package feed before configuring this preset.
- The current Gladius CMake configuration unconditionally requires X11 and GLFW for native Linux, including the WebGPU UI profile. A valid ARM64 sysroot alone will not make the application compatible with the Frame's VR shell if it does not provide the expected desktop windowing path.
- This preset is a starting cross-build configuration, not a promise that every vcpkg dependency is already validated on ARM64. In particular, Dawn's Vulkan configuration and the overlaid lib3mf port need confirmation with the selected vcpkg baseline and target sysroot.
- The lib3mf overlay now declares Linux ARM64 support so vcpkg can attempt the target build; this does not replace validating the resulting port build and runtime on the device.
- The AVX2/FMA flags are only applied to x86 targets. The beam-lattice Phase 3 SIMD implementation is x86-only; ARM builds use the scalar Phase 2 path when OpenVDB support is enabled.
- To use OpenCL, select an OpenCL-enabled configuration and provide ARM64 OpenCL headers/loader in the sysroot plus a working vendor ICD/runtime on the target. This initial preset disables OpenCL and OpenVDB.
