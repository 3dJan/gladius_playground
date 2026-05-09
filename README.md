# Gladius


[![Latest Release][def]](https://github.com/3MFConsortium/gladius/releases/latest) is available for download.

<div align="center">
  <a href="https://github.com/3MFConsortium/gladius/releases/latest">
    <img src="https://img.shields.io/badge/Get%20it%20now%21-blue?style=for-the-badge" alt="Get it now!">
  </a>
</div>

# 
![Screencast of gladius](gladius/documentation/img/gladius_animation.gif "3mf with volumetric extension")

Gladius is a developement tool used as a playground for the Volumetric Extension of the 3MF file format. It is designed to be a base for processing implicit geometries, especially 3mf files with the volumetric extension, including the implicit namespace. See <https://github.com/3MFConsortium/spec_volumetric> for more information about the volumetric extension.

Gladius can be uses as a library or as a standalone application. It offers a graphical programming interface for designing parts and a rendering engine for visualizing the results. The software is written in C++ and uses OpenCL for the computations. The rendering is done with OpenGL. The software is designed to be easily extensible and can be used as a base for other applications that need to process implicit geometries.

Note: The software is still in an early stage and might contain bugs. The software is provided as is and without any warranty. Use at your own risk.




# Features

- Import and export of 3mf files with volumetric extension
- Edit function graphs
- Create custom functions
- Visualize 3mf files with volumetric extension
- Generate contours

### Import

- 3mf with volumetric extension (as graph using the implicit namespace or using Image3D)

### Export

- 3mf with volumetric extension
- openvdb
- stl
- svg (contours)
- cli (contours)

# API

It offers API bindings for C#, C++ and Python, but additionally bindings could be generated for other languages supported by the Automatic Component Toolkit (<https://github.com/Autodesk/AutomaticComponentToolkit>). The API offers methods for extracting contours and generating meshs and is suited for integation in slicers or other 3D printing software.

# User Interface

The UI is based on the ImGUI library and offers a graphical programming interface for designing parts. The UI is still in an early stage and might be extended in the future.

![Screenshot of Gladius](gladius/documentation/img/gladius_screenshot.jpg "Screenshot of Gladius")
*Design of a filament spool holder in Gladius.*

# System Requirements

Gladius is designed to run on Windows and Linux, but might run on other platforms as well. The software is tested on Windows 10, 11 and Debian 12 Bookworm. The software requires a OpenCL 1.2 capable GPU and OpenGL for the UI. The software is designed to run on modern hardware and might not work on older systems or systems with outdated drivers or virtual machines with limited GPU support.

# Getting Started

1. Installation process
    See Build and Test
2. Software dependencies
    Gladius needs OpenCL. To be able to build you will propably need to install an OpenCL SDK that is usally provided by GPU vendors. To run the software installing the OpenCL runtime for your GPU should be sufficent. Some vendors (e.g AMD) already include it in the display driver packages. If the target system does not have an OpenCL 1.2 capable GPU you can also install a OpenCL CPU runtime like the one from Intel
    Intel: <https://software.intel.com/en-us/articles/opencl-drivers>
    AMD: <https://rocm.github.io/index.html>
    NVidia: <https://developer.NVidia.com/cuda-toolkit>

# Build

> Binary packages are also available: [![Latest Release][def]](https://github.com/3MFConsortium/gladius/releases/latest)

Step-by-step build instructions are in the platform-specific guides:

- **[Building on Linux](docs/building-linux.md)** — Debian/Ubuntu, Fedora; Clang + Ninja; vcpkg; OpenCL runtime options including Rusticl and POCL
- **[Building on Windows](docs/building-windows.md)** — Visual Studio 2022 (MSVC v143); Ninja; vcpkg; OpenCL SDK

> macOS is not supported — OpenCL is deprecated and non-functional on current macOS releases.

Both guides cover:
- OS-level package installation
- OpenCL runtime selection (GPU vendor drivers, Mesa Rusticl, POCL)
- vcpkg setup
- CMake configure and build
- IDE setup (VS Code and, on Windows, Visual Studio)
- Troubleshooting common problems

# Troubleshooting and Known Issues

## Windows — GPU hangs (TDR)

Windows restarts the display driver if it does not respond within ~2 seconds. Gladius's OpenCL compute passes can exceed this limit with complex models. Fix by increasing the TDR delay in the registry — see the [Windows build guide](docs/building-windows.md#gpu-hangs--tdr-timeout-detection-and-recovery) or:
<https://www.pugetsystems.com/labs/hpc/Working-around-TDR-in-Windows-for-a-better-GPU-computing-experience-777/>

## Multiple GPUs

Gladius selects the first available OpenCL device, which may be the slower integrated GPU on notebooks with both integrated and discrete graphics. See `gladius/src/compute/ComputeContext.cpp` to change the device selection logic, or use your GPU driver's control panel to assign Gladius to the high-performance GPU.

# Design your own models

Gladius allows the design of parts using a method that is called Constructive Solid Geometry (CSG). The main idea is to use a few primitives like spheres, cylinder, cubes etc. that you combine to complex parts. For that you use boolean operations like union, intersection and difference.

[def]: https://img.shields.io/github/release/3MFConsortium/gladius.svg
