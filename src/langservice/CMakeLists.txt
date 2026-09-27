add_library(nlang_langservice
    SymbolIndex.cpp
)

target_include_directories(nlang_langservice
    PUBLIC
        $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)

# Self-contained: standard library only, no Qt, no compiler dependency.
target_compile_features(nlang_langservice PUBLIC cxx_std_17)
