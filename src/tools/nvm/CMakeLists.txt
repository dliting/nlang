add_executable(nvm main.cpp)
#UTF-8 active code page (full-Unicode argv and paths; see the manifest).
if(MSVC)
    target_sources(nvm PRIVATE ${PROJECT_SOURCE_DIR}/cmake/nlang-utf8.manifest)
endif()
target_include_directories(nvm PRIVATE
    ${PROJECT_SOURCE_DIR}/src/vm
    ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(nvm PRIVATE nlang_vm nlang_langservice nlang_version)
