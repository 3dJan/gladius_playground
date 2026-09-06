
cmake_minimum_required(VERSION 3.21)

include(GoogleTest)
include(CTest)

set(CMAKE_CURRENT_BINDING_DIR ${CMAKE_SOURCE_DIR}/src/api/GladiusLib_component/Bindings/CppDynamic/)

set(BINARY ${CMAKE_PROJECT_NAME}_integrationtest)
file(GLOB_RECURSE TEST_SOURCES LIST_DIRECTORIES false *.h *.cpp)

# Remove OpenCL-requiring tests if disabled. This is also required for a
# WebGPU-only build because these sources include OpenCL C++ wrappers and use
# OpenCL-only ComputeCore APIs.
if(NOT ENABLE_OPENCL_TESTS)
    list(REMOVE_ITEM TEST_SOURCES 
        "${CMAKE_CURRENT_SOURCE_DIR}/GladiusLib_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Slice_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/DualContouringOctree_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/DualContouringStlExporter_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/ExportCancellation_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/HierarchicalDC_CompilationDebug_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/HierarchicalDC_ExtractionStep_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/HierarchicalDC_STLExport_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/HierarchicalDualContouring_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/ManifoldDualContouring_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/MeshBaseline_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/MeshSdfPerformance_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/ShellGenerator_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/ColorExport_Integration_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/ShellExportMeshQuality_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/GlobalMortonOctree_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/NodeLayoutEngine_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/PaletteExtractor_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/RayMarchPerf_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/SlicerProgramCompilation_tests.cpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/Writer3mf_SaveAs_tests.cpp"
    )
    message(STATUS "OpenCL tests disabled - excluding OpenCL-requiring integration tests")
endif()

set(SOURCES ${TEST_SOURCES})
add_executable(${BINARY} ${TEST_SOURCES})
if (MSVC)
    set_property(TARGET ${BINARY} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

set_property(TARGET ${BINARY} PROPERTY CXX_STANDARD 20)



message("CMAKE_CURRENT_BINDING_DIR=${CMAKE_CURRENT_BINDING_DIR}")
if (UNIX)
  target_link_libraries(${BINARY} PRIVATE ${CMAKE_DL_LIBS})
endif (UNIX)

find_package(imgui CONFIG REQUIRED)
find_package(unofficial-imgui-node-editor CONFIG REQUIRED)

# Note: imgui-node-editor depends on imgui, order matters for static linking
set(DEPENDENCIES ${CMAKE_PROJECT_NAME}_lib unofficial::imgui-node-editor::imgui-node-editor imgui::imgui GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main Eigen3::Eigen lodepng fmt::fmt)

if(GLADIUS_UI_BACKEND_NORMALIZED STREQUAL "OPENGL")
  find_package(OpenGL REQUIRED)
  list(APPEND DEPENDENCIES OpenGL::GL)
endif()

if (ENABLE_TRACY)
    set(DEPENDENCIES ${DEPENDENCIES} Tracy::TracyClient)
endif()


target_link_libraries(${BINARY} PRIVATE ${CMAKE_PROJECT_NAME}_lib  ${DEPENDENCIES})
target_include_directories(${BINARY} PUBLIC ../../src ${CMAKE_CURRENT_BINDING_DIR})

file(GLOB TEST_FILES CONFIGURE_DEPENDS
  "testdata/*.*"
  "${CMAKE_CURRENT_SOURCE_DIR}/../unittests/testdata/SphereInACage_small.3mf"
  "${CMAKE_CURRENT_SOURCE_DIR}/../unittests/testdata/SphereInACage_small.imagestack.3mf"
  "${CMAKE_CURRENT_SOURCE_DIR}/../unittests/testdata/SphereInACageSimplifiedMesh.3mf"
)

file(COPY ${TEST_FILES} DESTINATION "testdata")

file(GLOB FONT_FILES
  "../../components/imgui/misc/fonts/*.*"
)
file(COPY ${FONT_FILES}  DESTINATION "misc/fonts/")

# Use gtest_discover_tests for automatic test discovery with 'integration' label
gtest_discover_tests(${CMAKE_PROJECT_NAME}_integrationtest
    PROPERTIES LABELS "integration"
    DISCOVERY_MODE POST_BUILD
)
