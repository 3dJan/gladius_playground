set(BINARY ${CMAKE_PROJECT_NAME})
file(GLOB SOURCES LIST_DIRECTORIES true *.h *.cpp)

# MCP sources are intentionally NOT part of the core targets (gladius, gladius_lib)
# They will be added to the dedicated gladiusmcp target below
file(GLOB MCP_SOURCES LIST_DIRECTORIES true mcp/*.h mcp/*.cpp)
# Add MCP tools sources
file(GLOB MCP_TOOLS_SOURCES LIST_DIRECTORIES true mcp/tools/*.h mcp/tools/*.cpp)



add_library(${BINARY}_lib STATIC ${SOURCES})
# MCP-enabled variant of the core library so Application.cpp and others are built with MCP code
add_library(${BINARY}_lib_mcp STATIC ${SOURCES})
target_compile_definitions(${BINARY}_lib_mcp PUBLIC GLADIUS_ENABLE_MCP)


if (MSVC)
    add_executable(${BINARY} WIN32 ${SOURCES} )
    set_property(TARGET ${BINARY} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
    set_property(TARGET ${BINARY}_lib PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
else()
    add_executable(${BINARY} ${SOURCES} )
endif()

# Add include paths
include_directories(
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/components/IconFontCppHeaders
    ${CMAKE_CURRENT_SOURCE_DIR}/components/psimpl/lib
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/io
    ${CMAKE_CURRENT_SOURCE_DIR}/contour
    ${CMAKE_CURRENT_SOURCE_DIR}/nodes
    ${CMAKE_CURRENT_SOURCE_DIR}/compute
    ${CMAKE_CURRENT_SOURCE_DIR}/slicer
)

set (INCLUDE_PATHS
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/io
    ${CMAKE_CURRENT_SOURCE_DIR}/contour
    ${CMAKE_CURRENT_SOURCE_DIR}/nodes
    ${CMAKE_CURRENT_SOURCE_DIR}/compute
    ${CMAKE_CURRENT_SOURCE_DIR}/slicer
    ${CMAKE_CURRENT_SOURCE_DIR}/components/IconFontCppHeaders
    ${CMAKE_CURRENT_SOURCE_DIR}/components
    ${OPENMESH_INCLUDE_DIRS}
    ${AdditionalIncludes}
)
target_include_directories(${BINARY}_lib PUBLIC ${INCLUDE_PATHS})
target_include_directories(${BINARY}_lib_mcp PUBLIC ${INCLUDE_PATHS})
target_include_directories(${BINARY} PUBLIC ${INCLUDE_PATHS})

find_package(NLopt CONFIG REQUIRED)

# Core dependencies (shared by gladius and gladiusmcp)
set(DEPENDENCIES_CORE
    OpenCL::OpenCL
    ${X11_LIBRARIES}
    ${OPENGL_LIBRARIES}
    glfw
    glad::glad
    fmt::fmt-header-only
    Eigen3::Eigen
    tinyfiledialogs::tinyfiledialogs
    Threads::Threads
    sago::platform_folders
    OpenVDB::openvdb
    ZLIB::ZLIB
    gladius::rc
    lodepng
    pugixml::pugixml
    ${OPENMESH_LIBRARIES}
    lib3mf::lib3mf
    unofficial::minizip::minizip
    imgui::imgui
    unofficial::imgui-node-editor::imgui-node-editor
    PkgConfig::Clipper2
    NLopt::nlopt
    muparser::muparser
    PkgConfig::libcoro
    OpenMP::OpenMP_CXX
)

# MCP-only dependencies
set(DEPENDENCIES_MCP
    httplib::httplib
    nlohmann_json::nlohmann_json
)

if (WIN32)
    set(DEPENDENCIES_CORE ${DEPENDENCIES_CORE} blosc_shared)
else()
    set(DEPENDENCIES_CORE ${DEPENDENCIES_CORE} blosc_static)
endif()

if (ENABLE_TRACY)
    set(DEPENDENCIES_CORE ${DEPENDENCIES_CORE} Tracy::TracyClient)
endif()


target_link_libraries(${BINARY}_lib PUBLIC ${DEPENDENCIES_CORE})
target_link_libraries(${BINARY}_lib_mcp PUBLIC ${DEPENDENCIES_CORE})
target_link_libraries(${BINARY} PUBLIC ${BINARY}_lib)

# Precompiled headers: heavy/frequently-used headers compiled once per target,
# dramatically reducing MSVC memory pressure and improving incremental build speed.
set(GLADIUS_PCH_HEADERS
    <algorithm>
    <array>
    <chrono>
    <cmath>
    <cstring>
    <filesystem>
    <fstream>
    <functional>
    <iostream>
    <limits>
    <map>
    <memory>
    <mutex>
    <optional>
    <set>
    <sstream>
    <stdexcept>
    <string>
    <unordered_map>
    <unordered_set>
    <utility>
    <variant>
    <vector>
    <fmt/format.h>
    <Eigen/Core>
    <imgui.h>
)
target_precompile_headers(${BINARY}_lib PRIVATE ${GLADIUS_PCH_HEADERS})
# _lib_mcp has GLADIUS_ENABLE_MCP defined so it gets its own PCH build
target_precompile_headers(${BINARY}_lib_mcp PRIVATE ${GLADIUS_PCH_HEADERS})
# executables share the PCH from their corresponding library (same flags)
target_precompile_headers(${BINARY} REUSE_FROM ${BINARY}_lib)

# Increase the MSVC compiler's internal heap to prevent C1060 heap-overflow
# errors on large translation units (e.g. ManifoldDualContouringGpu.cpp).
if (MSVC)
    target_compile_options(${BINARY}_lib PUBLIC /Zm200)
    target_compile_options(${BINARY}_lib_mcp PUBLIC /Zm200)
endif()

# Enable AVX2 SIMD instructions for Phase 3 optimization
# PUBLIC so consumers (tests, etc.) see the same Eigen alignment as the library,
# avoiding ODR violations from mismatched struct layouts.
if (NOT WIN32)
    target_compile_options(${BINARY}_lib PUBLIC -mavx2 -mfma)
    target_compile_options(${BINARY}_lib_mcp PUBLIC -mavx2 -mfma)
    target_compile_options(${BINARY} PRIVATE -mavx2 -mfma)
endif()

install(TARGETS ${BINARY} RUNTIME DESTINATION ${INSTALL_PREFIX}/)

#
# gladiusmcp: gladius with MCP server support
#
# Build gladiusmcp by linking the MCP-enabled core library and MCP sources
add_executable(gladiusmcp ${MCP_SOURCES} ${MCP_TOOLS_SOURCES} main.cpp)
target_include_directories(gladiusmcp PUBLIC ${INCLUDE_PATHS}
    ${CMAKE_CURRENT_SOURCE_DIR}/io
    ${CMAKE_CURRENT_SOURCE_DIR}/ui
    ${CMAKE_CURRENT_SOURCE_DIR}/contour
    ${CMAKE_CURRENT_SOURCE_DIR}/nodes
    ${CMAKE_CURRENT_SOURCE_DIR}/mcp
)
target_compile_definitions(gladiusmcp PRIVATE GLADIUS_ENABLE_MCP)
if(ENABLE_UI_TESTING)
    target_compile_definitions(gladiusmcp PRIVATE ENABLE_UI_TESTING=1 IMGUI_ENABLE_TEST_ENGINE)
endif()
target_link_libraries(gladiusmcp PUBLIC ${BINARY}_lib_mcp ${DEPENDENCIES_MCP})
# Ensure the MCP-enabled static library is built before linking the executable
add_dependencies(gladiusmcp ${BINARY}_lib_mcp)
# Share the PCH already built for the MCP library (same flags + GLADIUS_ENABLE_MCP)
target_precompile_headers(gladiusmcp REUSE_FROM ${BINARY}_lib_mcp)
if (NOT WIN32)
    target_compile_options(gladiusmcp PRIVATE -mavx2 -mfma)
endif()
# Install MCP variant if it was built in this build tree; do not fail packaging if absent
install(PROGRAMS "$<TARGET_FILE:gladiusmcp>" DESTINATION ${INSTALL_PREFIX}/ OPTIONAL)

if (ENABLE_UI_TESTING)
    target_compile_definitions(${BINARY}_lib PUBLIC ENABLE_UI_TESTING=1 IMGUI_ENABLE_TEST_ENGINE)
    target_compile_definitions(${BINARY}_lib_mcp PUBLIC ENABLE_UI_TESTING=1 IMGUI_ENABLE_TEST_ENGINE)
    target_compile_definitions(${BINARY} PUBLIC ENABLE_UI_TESTING=1 IMGUI_ENABLE_TEST_ENGINE)
endif()
