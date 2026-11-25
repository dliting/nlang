add_executable(nlang main.cpp)
#UTF-8 active code page (full-Unicode argv and paths; see the manifest).
if(MSVC)
    target_sources(nlang PRIVATE ${PROJECT_SOURCE_DIR}/cmake/nlang-utf8.manifest)
endif()
target_include_directories(nlang PRIVATE
    ${PROJECT_SOURCE_DIR}/src/vm
    ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(nlang PRIVATE $<LINK_GROUP:START> nlang_compiler nlang_runtime nlang_vm nlang_langservice nlang_version $<LINK_GROUP:END>)

# ASan runtime provisioning (see the root CMakeLists): one sweep covers
# this directory's executable targets.
_nlang_sweep_asan_runtime("${CMAKE_CURRENT_SOURCE_DIR}")
