add_executable(bwt_tests compression_test.cpp)
target_link_libraries(bwt_tests PRIVATE bwt::compression)
add_test(NAME bwt_tests COMMAND bwt_tests)

add_executable(huffman_tests huffman_test.cpp)
target_link_libraries(huffman_tests PRIVATE bwt::compression)
add_test(NAME huffman_tests COMMAND huffman_tests)

add_executable(archive_test_driver archive_test_driver.cpp)
target_link_libraries(archive_test_driver PRIVATE compressor::archive)

find_package(Python3 3.9 COMPONENTS Interpreter REQUIRED)
add_test(NAME cli_tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/cli_test.py $<TARGET_FILE:compressor> $<TARGET_FILE:archive_test_driver>)

add_test(NAME archive_v2_tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/archive_v2_test.py $<TARGET_FILE:compressor>)
