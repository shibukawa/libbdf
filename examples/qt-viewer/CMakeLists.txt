# A viewer with Qt Widgets, drawing through libbdf's QPainter adapter.
#
#   cmake -B build && cmake --build build
#   ./build/qt-viewer ../../samples/two-pages.bdf
cmake_minimum_required(VERSION 3.21)
project(qt_viewer LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
find_package(Qt6 REQUIRED COMPONENTS Widgets)

# libbdf with its Qt adapter (bdf::qt): one that is installed with it, or
# else this checkout of it, built with BDF_QT
find_package(bdf CONFIG QUIET)
if(bdf_FOUND AND NOT TARGET bdf::qt)
  message(FATAL_ERROR "the libbdf installed at ${bdf_DIR} has no Qt adapter: "
                      "install one built with -DBDF_QT=ON")
elseif(NOT bdf_FOUND)
  set(BDF_QT ON)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../.. libbdf EXCLUDE_FROM_ALL)
endif()

add_executable(qt-viewer main.cpp ../common/open_doc.c)
target_include_directories(qt-viewer PRIVATE ../common)
target_link_libraries(qt-viewer PRIVATE bdf::qt Qt6::Widgets)
set_target_properties(qt-viewer PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
  target_compile_options(qt-viewer PRIVATE /W4 /utf-8)
else()
  target_compile_options(qt-viewer PRIVATE -Wall -Wextra)
endif()
