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

target_link_libraries(ncc PRIVATE $<LINK_GROUP:START> nlang_compiler nlang_runtime nlang_vm nlang_langservice nlang_version $<LINK_GROUP:END> tinyxml2)

# ASan runtime provisioning (see the root CMakeLists): one sweep covers
# this directory's executable targets.
_nlang_sweep_asan_runtime("${CMAKE_CURRENT_SOURCE_DIR}")
