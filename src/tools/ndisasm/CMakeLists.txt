add_executable(ndisasm main.cpp)
#UTF-8 active code page (full-Unicode argv and paths; see the manifest).
if(MSVC)
    target_sources(ndisasm PRIVATE ${PROJECT_SOURCE_DIR}/cmake/nlang-utf8.manifest)
endif()
target_include_directories(ndisasm PRIVATE ${PROJECT_SOURCE_DIR}/src/vm)
target_link_libraries(ndisasm PRIVATE nlang_vm nlang_version)
