vcpkg_check_linkage(ONLY_DYNAMIC_LIBRARY)

vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO 3MFConsortium/lib3mf
    REF "82c524d6701a535ad11f8dae4a46a27fa199829c" # develop branch, v2.6.0 with Boolean spec support
    SHA512 bfa93a2e087c5d0497dab7c55aee4f7e7e35e8718e250e9ad89f63b32c0b9e4dc0920b6d1c92d32b6fb9d606470c36846b39028f60992e1ada6d3b6103a8c206
    PATCHES
        fix-lib3mf-config-root.patch
        fix-missing-algorithm.patch
)

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DUSE_INCLUDED_ZLIB=OFF
        -DUSE_INCLUDED_LIBZIP=OFF
        -DUSE_INCLUDED_SSL=OFF
        -DUSE_INCLUDED_CPPBASE64=OFF
        -DUSE_INCLUDED_FASTFLOAT=OFF
        -DBUILD_FOR_CODECOVERAGE=OFF
        -DLIB3MF_TESTS=OFF
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/lib3mf)
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")

file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
