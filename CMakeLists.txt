cmake_minimum_required(VERSION 3.16)
project(compressor VERSION 1.0.0 LANGUAGES CXX)

option(BWT_BUILD_TESTS "Build BWT compression tests" OFF)

add_subdirectory(src)
add_subdirectory(app)

if(BWT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
