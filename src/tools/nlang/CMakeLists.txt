add_executable(nlang main.cpp)
#UTF-8 active code page (full-Unicode argv and paths; see the manifest).
if(MSVC)
    target_sources(nlang PRIVATE ${PROJECT_SOURCE_DIR}/cmake/nlang-utf8.manifest)
endif()
target_include_directories(nlang PRIVATE
    ${PROJECT_SOURCE_DIR}/src/vm
    ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(nlang PRIVATE
    nlang_compiler nlang_runtime nlang_vm nlang_langservice nlang_version)

#ASan tool chain: ship the asan runtime beside the exe (see the
#nlang_ship_asan_runtime function in the top-level CMakeLists).
nlang_ship_asan_runtime(nlang)
