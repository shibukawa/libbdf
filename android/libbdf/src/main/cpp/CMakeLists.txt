# The native part of libbdf for Android: libbdf itself, built from this
# checkout for each ABI, and the JNI functions the Kotlin classes call
# (bdf_jni.c), in one shared library.
cmake_minimum_required(VERSION 3.21)
project(bdf_jni LANGUAGES C)

# the static library goes into a shared one
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../../../.. libbdf EXCLUDE_FROM_ALL)

add_library(bdf_jni SHARED bdf_jni.c)
target_link_libraries(bdf_jni PRIVATE bdf::bdf jnigraphics log)
set_target_properties(bdf_jni PROPERTIES C_STANDARD 11 C_STANDARD_REQUIRED ON)
target_compile_options(bdf_jni PRIVATE -Wall -Wextra -Werror)
# the functions of libbdf are reached through JNI only: keep them out of the
# library's exports
target_link_options(bdf_jni PRIVATE -Wl,--exclude-libs,ALL)
