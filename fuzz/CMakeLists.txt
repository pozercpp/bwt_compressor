foreach(format bwc1 bwa1 bwa2)
    add_executable(fuzz_${format} ${format}.cpp)
    target_link_libraries(fuzz_${format} PRIVATE compressor::archive bwt::compression)
    target_link_options(fuzz_${format} PRIVATE -fsanitize=fuzzer)
endforeach()

if(BWT_BUILD_TESTS)
    find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
    add_test(NAME fuzz_smoke_tests COMMAND ${Python3_EXECUTABLE}
        ${PROJECT_SOURCE_DIR}/tests/fuzz_smoke_test.py ${PROJECT_BINARY_DIR})
    set_tests_properties(fuzz_smoke_tests PROPERTIES TIMEOUT 120)
endif()
