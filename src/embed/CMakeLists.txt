#Embedding adapter: the public host API (include/nlang/embed) over the
#VM. Internal VM headers (NcuLoader/VmExecutor/...) come in PRIVATE —
#nlang_vm only exports include/ (nvm target precedent). PRIVATE link is
#enough for a static library: CMake still links its dependencies into
#final binaries, without propagating VM usage requirements to hosts.
add_library(nlang_embed
    Value.cpp
    Marshalling.cpp
)
target_include_directories(nlang_embed
    PUBLIC
        ${PROJECT_SOURCE_DIR}/include
    PRIVATE
        ${PROJECT_SOURCE_DIR}/src/vm
)
target_link_libraries(nlang_embed
    PRIVATE
        nlang_vm
        nlang_runtime
)
