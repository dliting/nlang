add_executable(nvm main.cpp)
target_include_directories(nvm PRIVATE
    ${PROJECT_SOURCE_DIR}/src/vm
    ${PROJECT_SOURCE_DIR}/include)
target_link_libraries(nvm PRIVATE nlang_vm nlang_langservice nlang_version)
