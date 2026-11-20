cmake_minimum_required(VERSION 3.16)
project(compressor VERSION 1.0.0 LANGUAGES CXX)

option(BWT_BUILD_TESTS "Build BWT compression tests" OFF)

option(BWT_BUILD_FUZZERS "Build libFuzzer targets with ASan and UBSan (Clang)" OFF)
if(BWT_BUILD_FUZZERS)
    if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "BWT_BUILD_FUZZERS requires Clang with libFuzzer")
    endif()
    # Instrument the libraries, not only the harness; leave normal builds alone.
    add_compile_options(-fsanitize=fuzzer-no-link,address,undefined
        -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

add_subdirectory(src)
add_subdirectory(app)

if(BWT_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

if(BWT_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
