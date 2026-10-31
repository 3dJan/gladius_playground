vcpkg_download_distfile(ARCHIVE
    URLS "https://gitlab.vci.rwth-aachen.de:9000/OpenMesh/OpenMesh/-/archive/OpenMesh-${VERSION}/OpenMesh-OpenMesh-${VERSION}.tar.gz"
    FILENAME "OpenMesh-OpenMesh-${VERSION}.tar.gz"
    SHA512 90550eeb458f252c6251a0e31c708d5469704093d9297893830808daa6dec13c3241ad9338a2820f9591ed6f238bd6cf6b8f411eff0fafe10aa4f1c25b5ed793
)

vcpkg_extract_source_archive(
    SOURCE_PATH
    ARCHIVE "${ARCHIVE}"
)

# GitLab source archives omit submodule contents. Use the gitlink recorded by
# OpenMesh-11.0, rather than a moving cmake-library branch.
set(CMAKE_LIBRARY_REF 9e180cdb4df1d8370ccd9aeee1499709441dbdd1)
vcpkg_download_distfile(CMAKE_LIBRARY_ARCHIVE
  URLS "https://gitlab.vci.rwth-aachen.de:9000/cmake/cmake-library/-/archive/${CMAKE_LIBRARY_REF}/cmake-library-${CMAKE_LIBRARY_REF}.tar.gz"
  FILENAME "cmake-library-${CMAKE_LIBRARY_REF}.tar.gz"
  SHA512 796ce9ebf2462c26561964f7f308c7e37dfab9def61027270985a6ebbbd8e6e480681a714e32781bb041a4734711a7efa99393dd5439724f3ad71a0a80967493
)
vcpkg_extract_source_archive(CMAKE_LIBRARY_SOURCE_PATH
  ARCHIVE "${CMAKE_LIBRARY_ARCHIVE}"
)

# Non-editable extraction starts clean; preserve already-patched editable trees.
if(NOT EXISTS "${SOURCE_PATH}/.gladius-patched")
  file(COPY "${CMAKE_LIBRARY_SOURCE_PATH}/" DESTINATION "${SOURCE_PATH}/cmake-library")
  z_vcpkg_apply_patches(
    SOURCE_PATH "${SOURCE_PATH}"
    PATCHES
        fix-library-install-path.patch
        fix-pkgconfig.patch
        support-arm64-win.patch
  )
  file(TOUCH "${SOURCE_PATH}/.gladius-patched")
endif()

if(VCPKG_LIBRARY_LINKAGE STREQUAL "dynamic")
  set(OPENMESH_BUILD_SHARED ON)
else()
  set(OPENMESH_BUILD_SHARED OFF)
endif()

vcpkg_cmake_configure(
    SOURCE_PATH "${SOURCE_PATH}"
    OPTIONS
        -DBUILD_APPS=OFF
        -DVCI_COMMON_DO_NOT_COPY_POST_BUILD=ON
        -DVCI_NO_LIBRARY_INSTALL=ON
        -DOPENMESH_BUILD_SHARED=${OPENMESH_BUILD_SHARED}
	MAYBE_UNUSED_VARIABLES
		OPENMESH_BUILD_SHARED
        # [TODO]: add apps as feature, requires qt5 and freeglut
)

vcpkg_cmake_install()
vcpkg_copy_pdbs()
vcpkg_cmake_config_fixup(PACKAGE_NAME OpenMesh CONFIG_PATH "share/OpenMesh/cmake")
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/include/OpenMesh/Tools/VDPM/xpm")

vcpkg_install_copyright(FILE_LIST "${SOURCE_PATH}/LICENSE")
