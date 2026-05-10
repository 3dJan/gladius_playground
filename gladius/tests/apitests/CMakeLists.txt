cmake_minimum_required(VERSION 3.21)

include(GoogleTest)
include(CTest)

set(CMAKE_CURRENT_BINDING_DIR ${CMAKE_SOURCE_DIR}/src/api/GladiusLib_component/Bindings/CppDynamic/)

set(BINARY ${CMAKE_PROJECT_NAME}_apitest)
file(GLOB_RECURSE TEST_SOURCES LIST_DIRECTORIES false *.h *.cpp)

set(SOURCES ${TEST_SOURCES})
add_executable(${BINARY} ${TEST_SOURCES})

# Include MCP sources for tests that validate MCP behavior
file(GLOB API_MCP_SOURCES CONFIGURE_DEPENDS
  "${CMAKE_SOURCE_DIR}/src/mcp/*.h" "${CMAKE_SOURCE_DIR}/src/mcp/*.cpp"
  "${CMAKE_SOURCE_DIR}/src/mcp/tools/*.h" "${CMAKE_SOURCE_DIR}/src/mcp/tools/*.cpp"
)
target_sources(${BINARY} PRIVATE ${API_MCP_SOURCES})
target_compile_definitions(${BINARY} PRIVATE GLADIUS_ENABLE_MCP)

if (MSVC)
    set_property(TARGET ${BINARY} PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")
endif()

set_property(TARGET ${BINARY} PROPERTY CXX_STANDARD 20)

message("CMAKE_CURRENT_BINDING_DIR=${CMAKE_CURRENT_BINDING_DIR}")

if (UNIX)
    target_link_libraries(${BINARY} PRIVATE ${CMAKE_DL_LIBS})
endif (UNIX)

# OpenGL is needed because gladius_lib links imgui which uses OpenGL
find_package(OpenGL REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(unofficial-imgui-node-editor CONFIG REQUIRED)

# Note: imgui-node-editor depends on imgui, order matters for static linking
set(DEPENDENCIES unofficial::imgui-node-editor::imgui-node-editor imgui::imgui GTest::gtest GTest::gtest_main GTest::gmock GTest::gmock_main Eigen3::Eigen lodepng fmt::fmt OpenGL::GL)

# Add MCP-only dependencies for tests that use MCP
list(APPEND DEPENDENCIES httplib::httplib nlohmann_json::nlohmann_json)

if (ENABLE_TRACY)
    set(DEPENDENCIES ${DEPENDENCIES} Tracy::TracyClient)
endif()

target_link_libraries(${BINARY} PRIVATE ${CMAKE_PROJECT_NAME}_lib ${DEPENDENCIES})

# Include directories for test helpers and MCP sources
target_include_directories(${BINARY} PUBLIC 
    ../../src 
    ../../src/io 
    ${CMAKE_CURRENT_BINDING_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}  # For testhelper.h, testdata.h (local copies)
)

file(GLOB TEST_FILES CONFIGURE_DEPENDS
    "testdata/*.*"
    "${CMAKE_CURRENT_SOURCE_DIR}/../integrationtests/testdata/*.*"
)

file(COPY ${TEST_FILES} DESTINATION "testdata")

file(GLOB FONT_FILES
    "../../components/imgui/misc/fonts/*.*"
)
file(COPY ${FONT_FILES} DESTINATION "misc/fonts/")

# Ensure licenses are available for dialogs or setup code that scans getAppDir()/licenses
file(COPY "../../components/licenses" DESTINATION ".")

# Provide example template used by Document::newFromTemplate()
file(MAKE_DIRECTORY "examples")
file(COPY "../../examples/template.3mf" DESTINATION "examples/")

# Use gtest_discover_tests for automatic test discovery with 'api' label
gtest_discover_tests(${BINARY}
    PROPERTIES LABELS "api"
    DISCOVERY_MODE POST_BUILD
)
