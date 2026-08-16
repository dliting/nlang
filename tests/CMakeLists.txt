add_executable(test_vm
    test_vm/test_vm.cpp
)

target_include_directories(test_vm
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
        ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(test_vm
    PRIVATE
        nlang_vm
        nlang_compiler
        nlang_runtime
)

add_test(NAME vm_tests COMMAND test_vm)

# IDE ProjectModel unit tests (Phase 10 Step 1).
# Requires Qt5 (Core + Xml + Test) — only built when IDE is enabled.
if(NLANG_BUILD_IDE)
    find_package(Qt5Test REQUIRED)
    set(CMAKE_AUTOMOC ON)
    add_executable(test_projectmodel
        test_nide/test_projectmodel.cpp
    )
    target_link_libraries(test_projectmodel
        PRIVATE
            nide_projectmodel
            Qt5::Test
    )
    add_test(NAME projectmodel_tests COMMAND test_projectmodel)
endif()

# End-to-end test: compile-only (exits 0)
add_test(NAME e2e_compile
    COMMAND ncc build ${PROJECT_SOURCE_DIR}/examples/hello.n)
