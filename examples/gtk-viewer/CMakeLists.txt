# A viewer with GTK 4, drawing through libbdf's cairo adapter.
#
#   cmake -B build && cmake --build build
#   ./build/gtk-viewer ../../samples/two-pages.bdf
cmake_minimum_required(VERSION 3.21)
project(gtk_viewer LANGUAGES C)

find_package(PkgConfig REQUIRED)
pkg_check_modules(GTK4 REQUIRED IMPORTED_TARGET gtk4)

# libbdf with its cairo adapter (bdf::cairo): one that is installed with
# it, or else this checkout of it, built with BDF_CAIRO
find_package(bdf CONFIG QUIET)
if(bdf_FOUND AND NOT TARGET bdf::cairo)
  message(FATAL_ERROR "the libbdf installed at ${bdf_DIR} has no cairo adapter: "
                      "install one built with -DBDF_CAIRO=ON")
elseif(NOT bdf_FOUND)
  set(BDF_CAIRO ON)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. libbdf EXCLUDE_FROM_ALL)
endif()

add_executable(gtk-viewer main.c ../common/open_doc.c)
target_include_directories(gtk-viewer PRIVATE ../common)
target_link_libraries(gtk-viewer PRIVATE bdf::cairo PkgConfig::GTK4)
set_target_properties(gtk-viewer PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(gtk-viewer PRIVATE -Wall -Wextra)
find_library(MATH_LIBRARY m)
if(MATH_LIBRARY)
  target_link_libraries(gtk-viewer PRIVATE ${MATH_LIBRARY})
endif()
