# The vcpkg port of libbdf, as an overlay port:
#
#   vcpkg install bdf --overlay-ports=<libbdf>/packaging/vcpkg
#
# It builds the release its version names. SHA512 is that of the release's
# source archive, which vcpkg prints when it is not the one written here.
# With BDF_VCPKG_SOURCE set to a checkout (and VCPKG_KEEP_ENV_VARS naming
# it), it builds that checkout instead: what the CI of libbdf does.
if(DEFINED ENV{BDF_VCPKG_SOURCE})
  set(SOURCE_PATH "$ENV{BDF_VCPKG_SOURCE}")
else()
  vcpkg_from_github(
    OUT_SOURCE_PATH SOURCE_PATH
    REPO shibukawa/libbdf
    REF "v${VERSION}"
    SHA512 0
    HEAD_REF main)
endif()

vcpkg_cmake_configure(
  SOURCE_PATH "${SOURCE_PATH}"
  OPTIONS
    -DBDF_BUILD_TESTS=OFF
    -DBDF_BUILD_FUZZ=OFF)
vcpkg_cmake_install()
vcpkg_cmake_config_fixup(CONFIG_PATH lib/cmake/bdf)
vcpkg_fixup_pkgconfig()

file(REMOVE_RECURSE "${CURRENT_PACKAGES_DIR}/debug/include")
vcpkg_install_copyright(FILE_LIST
  "${SOURCE_PATH}/LICENSE"
  "${SOURCE_PATH}/third_party/brotli/LICENSE"
  "${SOURCE_PATH}/third_party/go-math/LICENSE"
  "${SOURCE_PATH}/third_party/go-image/LICENSE"
  "${SOURCE_PATH}/third_party/go-text/LICENSE"
  "${SOURCE_PATH}/third_party/tinygodriver/LICENSE"
  "${SOURCE_PATH}/third_party/unicode/LICENSE")
file(INSTALL "${CMAKE_CURRENT_LIST_DIR}/usage" DESTINATION "${CURRENT_PACKAGES_DIR}/share/${PORT}")
