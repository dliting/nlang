add_executable(nvm main.cpp)
#UTF-8 active code page (full-Unicode argv and paths; see the manifest).
if(MSVC)
    target_sources(nvm PRIVATE ${PROJECT_SOURCE_DIR}/cmake/nlang-utf8.manifest)
endif()
target_include_directories(nvm PRIVATE
    ${PROJECT_SOURCE_DIR}/src/vm
    ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(nvm PRIVATE nlang_vm nlang_langservice nlang_version)

#ASan tool chain: copy the asan runtime the loader resolves from the
#executable's own directory (guard and path in the top-level CMakeLists).
if(DEFINED NLANG_ASAN_RUNTIME)
    add_custom_command(TARGET nvm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${NLANG_ASAN_RUNTIME}" $<TARGET_FILE_DIR:nvm>
        COMMENT "Copying ASan runtime beside nvm")
endif()
