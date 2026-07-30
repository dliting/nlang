add_executable(ncc
    main.cpp
)

target_include_directories(ncc
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
)

target_link_libraries(ncc
    PRIVATE
        nlang_compiler
        nlang_runtime
        nlang_vm
)
