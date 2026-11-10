add_executable(embed_host main.cpp)
target_link_libraries(embed_host PRIVATE nlang_embed)

# ASan runtime provisioning (see the root CMakeLists): one sweep covers
# this directory's executable targets.
_nlang_sweep_asan_runtime("${CMAKE_CURRENT_SOURCE_DIR}")
