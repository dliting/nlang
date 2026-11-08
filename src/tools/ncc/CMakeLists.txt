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

#ASan tool chain: copy the asan runtime the loader resolves from the
#executable's own directory (guard and path in the top-level CMakeLists).
if(DEFINED NLANG_ASAN_RUNTIME)
    add_custom_command(TARGET ncc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NLANG_ASAN_RUNTIME}" $<TARGET_FILE_DIR:ncc>
        COMMENT "Copying ASan runtime beside ncc")
endif()
