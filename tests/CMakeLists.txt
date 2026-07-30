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

# End-to-end test: compile-only (exits 0)
add_test(NAME e2e_compile
    COMMAND ncc build ${PROJECT_SOURCE_DIR}/examples/hello.n)
