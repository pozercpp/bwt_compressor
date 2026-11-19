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

add_test(NAME metadata_tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/metadata_test.py $<TARGET_FILE:compressor>)

add_library(archive_faults SHARED archive_faults.cpp)
target_link_libraries(archive_faults PRIVATE ${CMAKE_DL_LIBS})
add_test(NAME links_tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/links_test.py $<TARGET_FILE:compressor> $<TARGET_FILE:archive_faults>)

add_executable(progress_tests progress_test.cpp)
target_link_libraries(progress_tests PRIVATE compressor::archive bwt::compression)
add_test(NAME progress_tests COMMAND progress_tests)
add_test(NAME progress_cli_tests COMMAND ${Python3_EXECUTABLE}
    ${CMAKE_CURRENT_SOURCE_DIR}/progress_test.py $<TARGET_FILE:compressor>)
