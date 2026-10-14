# A viewer with SDL3 and libbdf's own rasterizer.
#
#   cmake -B build && cmake --build build
#   ./build/sdl-viewer ../../samples/two-pages.bdf
cmake_minimum_required(VERSION 3.21)
project(sdl_viewer LANGUAGES C)

find_package(SDL3 CONFIG REQUIRED)
# libbdf: one that is installed, or else this checkout of it
find_package(bdf CONFIG QUIET)
if(NOT bdf_FOUND)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. libbdf EXCLUDE_FROM_ALL)
endif()

add_executable(sdl-viewer main.c ../common/open_doc.c ../common/png_write.c)
target_include_directories(sdl-viewer PRIVATE ../common)
target_link_libraries(sdl-viewer PRIVATE bdf::bdf SDL3::SDL3)
set_target_properties(sdl-viewer PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
  target_compile_options(sdl-viewer PRIVATE /W4 /utf-8)
else()
  target_compile_options(sdl-viewer PRIVATE -Wall -Wextra)
  find_library(MATH_LIBRARY m)
  if(MATH_LIBRARY)
    target_link_libraries(sdl-viewer PRIVATE ${MATH_LIBRARY})
  endif()
endif()
