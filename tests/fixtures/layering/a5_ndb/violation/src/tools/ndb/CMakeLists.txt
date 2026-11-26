# Fixture: ndb's link table naming the compiler target.
target_link_libraries(ndb PRIVATE nlang_vm nlang_compiler ${CMAKE_DL_LIBS})
