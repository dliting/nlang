add_executable(ncc
    main.cpp
    ProjectFile.cpp
)

#UTF-8 active code page (full-Unicode argv and paths; see the manifest).
if(MSVC)
    target_sources(ncc PRIVATE ${PROJECT_SOURCE_DIR}/cmake/nlang-utf8.manifest)
endif()

target_include_directories(ncc
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
)

target_link_libraries(ncc
    PRIVATE
        nlang_compiler
        nlang_runtime
        nlang_vm
        nlang_langservice
        tinyxml2
        nlang_version
)

#ASan tool chain: ship the asan runtime beside the exe (see the
#nlang_ship_asan_runtime function in the top-level CMakeLists).
nlang_ship_asan_runtime(ncc)
