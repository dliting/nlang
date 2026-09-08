add_executable(ndb
    main.cpp
    DebugSession.cpp
    MachineFrontEnd.cpp
    SourceCache.cpp
)
target_include_directories(ndb PRIVATE ${PROJECT_SOURCE_DIR}/src/vm)
target_link_libraries(ndb PRIVATE nlang_vm nlang_version)
