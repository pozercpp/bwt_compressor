add_executable(bwt_tests compression_test.cpp)
target_link_libraries(bwt_tests PRIVATE bwt::compression)
add_test(NAME bwt_tests COMMAND bwt_tests)

add_executable(huffman_tests huffman_test.cpp)
target_link_libraries(huffman_tests PRIVATE bwt::compression)
add_test(NAME huffman_tests COMMAND huffman_tests)

find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
add_test(NAME cli_tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/cli_test.py $<TARGET_FILE:compressor>)
