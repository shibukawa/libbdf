# bdf-tool: a command-line program over libbdf.
#
#   cmake -B build && cmake --build build
#   ./build/bdf-tool info ../../samples/two-pages.bdf
cmake_minimum_required(VERSION 3.21)
project(bdf_tool LANGUAGES C)

# libbdf: one that is installed (find_package), or else this checkout of it.
# A project of your own would take it with FetchContent from its repository
# the same way.
find_package(bdf CONFIG QUIET)
if(NOT bdf_FOUND)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. libbdf EXCLUDE_FROM_ALL)
endif()

add_executable(bdf-tool bdf_tool.c ../common/open_doc.c ../common/png_write.c)
target_include_directories(bdf-tool PRIVATE ../common)
target_link_libraries(bdf-tool PRIVATE bdf::bdf)
set_target_properties(bdf-tool PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
  target_compile_options(bdf-tool PRIVATE /W4 /utf-8)
else()
  target_compile_options(bdf-tool PRIVATE -Wall -Wextra)
endif()
