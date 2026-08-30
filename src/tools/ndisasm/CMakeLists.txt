add_executable(ndisasm main.cpp)
target_include_directories(ndisasm PRIVATE ${PROJECT_SOURCE_DIR}/src/vm)
target_link_libraries(ndisasm PRIVATE nlang_vm nlang_version)
