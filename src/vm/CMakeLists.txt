add_library(nlang_vm
    OpCodeTable.cpp
    VmExecutor.cpp
    IntrinsicsMath.cpp
    IntrinsicsIo.cpp
    IntrinsicsString.cpp
    IntrinsicsFs.cpp
    VmBackend.cpp
    ModuleLoader.cpp
    ModuleSaver.cpp
)

target_include_directories(nlang_vm
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(nlang_vm
    PUBLIC nlang_runtime
    PRIVATE nlang_compiler
)
