# Fixture: ndisasm linking vm plus core stays clean.
target_link_libraries(ndisasm PRIVATE nlang_vm nlang_core ${CMAKE_DL_LIBS})
