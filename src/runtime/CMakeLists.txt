add_library(nlang_runtime
    Runtime.cpp
    Module.cpp
    Node.cpp
    NodeContainers.cpp
    IdString.cpp
    Variant.cpp
    RnData.cpp
    RnMisc.cpp
    RnTypes.cpp
    RuntimeNode.cpp
    Log.cpp
    Utils.cpp
)

target_include_directories(nlang_runtime
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${PROJECT_SOURCE_DIR}/include/nlang/runtime
)

target_compile_definitions(nlang_runtime
    PRIVATE NLANG_RUNTIME_EXPORT
)
