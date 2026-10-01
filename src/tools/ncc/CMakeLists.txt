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
        tinyxml2
        nlang_version
)
