# Fixture: core borrowing the compiler private dir -- the
# green-path blind spot the review named: without this CMake
# face the include face alone cannot see a bare-name include
# of a compiler header.
target_include_directories(nlang_core PRIVATE
    ${PROJECT_SOURCE_DIR}/src/compiler)
