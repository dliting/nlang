# Fixture: core linking runtime stays clean; the header comment
# may name nlang_compiler and nlang_vm -- a comment cannot add
# a dependency, so the CMake face matches stripped lines only.
target_link_libraries(nlang_core PUBLIC nlang_runtime)
