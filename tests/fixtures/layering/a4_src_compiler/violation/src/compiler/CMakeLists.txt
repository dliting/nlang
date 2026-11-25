# Fixture: the compiler borrowing the vm private dir.
target_include_directories(nlang_compiler PRIVATE
    ${PROJECT_SOURCE_DIR}/src/vm)
