# bdf2txt with libbdf as one header: the build writes the header from the
# sources of this checkout (Python 3), as the release workflow does. With the
# bdf.h of a release, compile bdf2txt.c beside it and nothing else.
cmake_minimum_required(VERSION 3.21)
project(bdf2txt LANGUAGES C)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(LIBBDF ${CMAKE_CURRENT_SOURCE_DIR}/../..)
set(HEADER ${CMAKE_CURRENT_BINARY_DIR}/single/bdf/bdf.h)
file(GLOB LIBBDF_SOURCES ${LIBBDF}/src/*.c ${LIBBDF}/src/*.h ${LIBBDF}/include/bdf/bdf.h)
add_custom_command(
  OUTPUT ${HEADER}
  COMMAND Python3::Interpreter ${LIBBDF}/tools/amalgamate.py -o ${HEADER}
  DEPENDS ${LIBBDF}/tools/amalgamate.py ${LIBBDF_SOURCES}
  COMMENT "Joining libbdf into one header")

add_executable(bdf2txt bdf2txt.c ${HEADER})
target_include_directories(bdf2txt PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/single)
set_target_properties(bdf2txt PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
if(MSVC)
  target_compile_options(bdf2txt PRIVATE /utf-8)
else()
  # the rasterizer is not to fuse multiply-adds (it says so with a pragma
  # too), and needs the math library
  target_compile_options(bdf2txt PRIVATE -ffp-contract=off)
  find_library(MATH_LIBRARY m)
  if(MATH_LIBRARY)
    target_link_libraries(bdf2txt PRIVATE ${MATH_LIBRARY})
  endif()
endif()
