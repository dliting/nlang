# Fixture: vm linking core (runtime arrives transitively) stays clean.
target_link_libraries(nlang_vm
    PUBLIC nlang_core ${CMAKE_DL_LIBS})
