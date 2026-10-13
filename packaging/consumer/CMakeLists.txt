# A consumer of the installed library, through its CMake package.
cmake_minimum_required(VERSION 3.21)
project(bdf_consumer C)
find_package(bdf CONFIG REQUIRED)
add_executable(consumer main.c)
target_link_libraries(consumer PRIVATE bdf::bdf)
enable_testing()
add_test(NAME consumer COMMAND consumer)
