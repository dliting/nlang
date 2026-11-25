# Fixture: the vm link line naming the compiler target.
target_link_libraries(nlang_vm
    PRIVATE nlang_compiler nlang_langservice ${CMAKE_DL_LIBS})
