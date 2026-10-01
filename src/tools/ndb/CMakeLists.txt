add_executable(ndb
    main.cpp
    DebugSession.cpp
    MachineFrontEnd.cpp
    SourceCache.cpp
)
#UTF-8 active code page (full-Unicode argv and paths; see the manifest).
if(MSVC)
    target_sources(ndb PRIVATE ${PROJECT_SOURCE_DIR}/cmake/nlang-utf8.manifest)
endif()
target_include_directories(ndb PRIVATE ${PROJECT_SOURCE_DIR}/src/vm)
target_link_libraries(ndb PRIVATE nlang_vm nlang_version)
