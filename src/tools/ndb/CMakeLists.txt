add_executable(ndb
    main.cpp
    DebugSession.cpp
    DebugSessionInspect.cpp
    MachineFrontEnd.cpp
    SourceCache.cpp
)
#UTF-8 active code page (full-Unicode argv and paths; see the manifest).
if(MSVC)
    target_sources(ndb PRIVATE ${PROJECT_SOURCE_DIR}/cmake/nlang-utf8.manifest)
endif()
target_include_directories(ndb PRIVATE ${PROJECT_SOURCE_DIR}/src/vm)
target_link_libraries(ndb PRIVATE $<LINK_GROUP:START> nlang_vm nlang_langservice nlang_version nlang_compiler $<LINK_GROUP:END>)

# ASan runtime provisioning (see the root CMakeLists): one sweep covers
# this directory's executable targets.
_nlang_sweep_asan_runtime("${CMAKE_CURRENT_SOURCE_DIR}")
