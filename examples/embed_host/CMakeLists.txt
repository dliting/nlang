add_executable(embed_host main.cpp)
target_link_libraries(embed_host PRIVATE nlang_embed)
#embed_host_smoke launches this host directly, so an ASan tree needs the
#ASan runtime beside it (nlang_ship_asan_runtime function in the top-level
#CMakeLists).
nlang_ship_asan_runtime(embed_host)
