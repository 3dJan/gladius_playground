vcpkg_check_linkage(ONLY_STATIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO thedmd/imgui-node-editor
    REF v0.9.3
    SHA512 83573b6ed776095837373bc95be1c1f5b85e9c5fae2145647f9cb6fdc17d3889edce716ac9e27c1bbde56f00803a66db98ca856910e6e0ce8714d3c5ce3f7c3f
    HEAD_REF master
    PATCHES
        fix-vec2-math-operators.patch
)

file(COPY "${CMAKE_CURRENT_LIST_DIR}/CMakeLists.txt" DESTINATION "${SOURCE_PATH}")

# Safety guard: clamp grid VIEW_SIZE to prevent infinite loop from float precision loss.
# When VIEW_SIZE exceeds ~5.4e8, float addition (x += 32) stops changing x, causing an
# infinite loop in the grid drawing code. Clamping to 1e7 allows 312500 grid lines per
# axis which is more than sufficient for any visible area.
vcpkg_replace_string("${SOURCE_PATH}/imgui_node_editor.cpp"
    "ImVec2 VIEW_SIZE = m_Canvas.ViewRect().GetSize();"
    "ImVec2 VIEW_SIZE = m_Canvas.ViewRect().GetSize();\n        VIEW_SIZE.x = ImMin(VIEW_SIZE.x, 1e7f);\n        VIEW_SIZE.y = ImMin(VIEW_SIZE.y, 1e7f);"
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS_DEBUG
        -DIMGUI_NODE_EDITOR_SKIP_HEADERS=ON
)

vcpkg_cmake_install()

vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(PACKAGE_NAME unofficial-${PORT} CONFIG_PATH share/unofficial-${PORT})

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
