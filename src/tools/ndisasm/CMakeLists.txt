add_executable(ndisasm main.cpp)
#UTF-8 active code page (full-Unicode argv and paths; see the manifest).
if(MSVC)
    target_sources(ndisasm PRIVATE ${PROJECT_SOURCE_DIR}/cmake/nlang-utf8.manifest)
endif()
target_include_directories(ndisasm PRIVATE ${PROJECT_SOURCE_DIR}/src/vm)
target_link_libraries(ndisasm PRIVATE nlang_vm nlang_version)

#ASan tool chain: ship the asan runtime beside the exe (see the
#nlang_ship_asan_runtime function in the top-level CMakeLists).
nlang_ship_asan_runtime(ndisasm)
