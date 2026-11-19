add_library(bwt_compression
    src/compression/compression.cpp
    src/compression/bwt_mtf_rle/transforms.cpp
    src/compression/arithmetic/arithmetic.cpp
    src/compression/huffman/huffman.cpp
)
add_library(bwt::compression ALIAS bwt_compression)
find_package(Threads REQUIRED)
target_link_libraries(bwt_compression PRIVATE Threads::Threads)
target_include_directories(bwt_compression PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(bwt_compression PUBLIC cxx_std_23)
set_target_properties(bwt_compression PROPERTIES CXX_EXTENSIONS OFF)

add_library(compressor_concurrency src/concurrency/block_pool.cpp)
add_library(compressor::concurrency ALIAS compressor_concurrency)
target_include_directories(compressor_concurrency PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(compressor_concurrency PUBLIC cxx_std_23)
target_link_libraries(compressor_concurrency PRIVATE bwt::compression Threads::Threads)
set_target_properties(compressor_concurrency PROPERTIES CXX_EXTENSIONS OFF)

add_library(compressor_archive
    src/archive/archive.cpp
    src/archive/archive_v2.cpp
)
add_library(compressor::archive ALIAS compressor_archive)
target_include_directories(compressor_archive PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(compressor_archive PUBLIC cxx_std_23)
target_link_libraries(compressor_archive PRIVATE bwt::compression compressor::concurrency Threads::Threads)
set_target_properties(compressor_archive PROPERTIES CXX_EXTENSIONS OFF)
