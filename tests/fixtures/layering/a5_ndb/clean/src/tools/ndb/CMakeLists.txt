# Fixture: ndb linking the vm side only stays clean.
target_link_libraries(ndb PRIVATE nlang_vm nlang_core ${CMAKE_DL_LIBS})
