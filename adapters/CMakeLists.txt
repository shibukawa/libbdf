# The adapters: the pages of a document drawn by another 2D library through
# bdf_play_page (include/bdf/bdf_<name>.h says how each is used). Each is a
# static library of its own, bdf::<name>, built when its option is on, and
# tested by a program that compares what it draws with what the rasterizer
# draws (tools/compare.h).

set(BDF_ADAPTER_TARGETS "")

# bdf_adapter(<name> <source>): the library bdf_<name> (bdf::<name>).
function(bdf_adapter name source)
  add_library(bdf_${name} STATIC ${source})
  add_library(bdf::${name} ALIAS bdf_${name})
  target_link_libraries(bdf_${name} PUBLIC bdf)
  target_compile_options(bdf_${name} PRIVATE ${BDF_WARNINGS})
  set_target_properties(bdf_${name} PROPERTIES
    EXPORT_NAME ${name}
    POSITION_INDEPENDENT_CODE ON
    PUBLIC_HEADER ${PROJECT_SOURCE_DIR}/include/bdf/bdf_${name}.h)
  set(BDF_ADAPTER_TARGETS ${BDF_ADAPTER_TARGETS} bdf_${name} PARENT_SCOPE)
endfunction()

# bdf_adapter_test(<name> <source>): the comparison program of an adapter,
# which compares the pages built into it when it is given no document.
function(bdf_adapter_test name source)
  if(NOT BDF_BUILD_TESTS)
    return()
  endif()
  add_executable(compare_${name} ${source} tools/compare.c)
  target_include_directories(compare_${name} PRIVATE ${PROJECT_SOURCE_DIR}/tests)
  target_link_libraries(compare_${name} PRIVATE bdf_${name})
  if(BDF_LIBM)
    target_link_libraries(compare_${name} PRIVATE ${BDF_LIBM})
  endif()
  add_test(NAME adapter-${name} COMMAND compare_${name})
endfunction()

if(BDF_CAIRO)
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(BDF_CAIRO_DEP REQUIRED IMPORTED_TARGET cairo)
  bdf_adapter(cairo cairo/bdf_cairo.c)
  target_link_libraries(bdf_cairo PUBLIC PkgConfig::BDF_CAIRO_DEP)
  if(BDF_LIBM)
    target_link_libraries(bdf_cairo PRIVATE ${BDF_LIBM})
  endif()
  bdf_adapter_test(cairo cairo/compare_cairo.c)
endif()

if(BDF_COREGRAPHICS)
  if(NOT APPLE)
    message(FATAL_ERROR "BDF_COREGRAPHICS needs an Apple platform")
  endif()
  bdf_adapter(coregraphics coregraphics/bdf_coregraphics.c)
  target_link_libraries(bdf_coregraphics PUBLIC "-framework CoreGraphics" "-framework CoreFoundation")
  bdf_adapter_test(coregraphics coregraphics/compare_coregraphics.c)
endif()

set(BDF_ADAPTER_TARGETS ${BDF_ADAPTER_TARGETS} PARENT_SCOPE)
