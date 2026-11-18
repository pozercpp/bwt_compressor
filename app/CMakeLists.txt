add_executable(compressor main.cpp)
target_link_libraries(compressor PRIVATE bwt::compression)
set_target_properties(compressor PROPERTIES CXX_EXTENSIONS OFF)
