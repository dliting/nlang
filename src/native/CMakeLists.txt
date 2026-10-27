# Dynamically loadable native standard-library modules. Each namespace is a
# shared library that the VM loads through the same mechanism as a third-party
# native library (see include/nlang/vm/NativeHost.h). A module depends only on
# the stable C ABI header and the C++ standard library — it does NOT link
# nlang_vm and reaches VM functionality solely through the host callbacks.
#
# Default file names match the loader's convention:
#   Windows: nlang_<ns>.dll
#   Linux:   libnlang_<ns>.so
#   macOS:   libnlang_<ns>.dylib

set(NLANG_NATIVE_MODULES math io fs tensor)

foreach(mod IN LISTS NLANG_NATIVE_MODULES)
    add_library(nlang_${mod} SHARED ${mod}/${mod}_native.cpp)
    target_include_directories(nlang_${mod}
        PRIVATE ${PROJECT_SOURCE_DIR}/include)
    set_target_properties(nlang_${mod} PROPERTIES
        CXX_STANDARD 17
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON)
endforeach()

# Aggregate target for out-of-directory consumers that need "all modules
# built" as one build-order edge (the docs snippet audit) — depending on
# this name instead of re-listing the modules keeps the list single-sourced.
add_custom_target(nlang_native_modules)
foreach(mod IN LISTS NLANG_NATIVE_MODULES)
    add_dependencies(nlang_native_modules nlang_${mod})
endforeach()

# Multi-file module sources beyond the per-module naming pattern.
target_sources(nlang_tensor PRIVATE
    tensor/tensor_binary.cpp
    tensor/tensor_matrix.cpp
    tensor/tensor_reduce.cpp)

# Single authoritative output and NOTHING copies the modules around the
# build tree — hosts resolve them through NativeLibraryLoader's build-tree
# fallback (walk-up discovery, consulted after explicit search dirs; see
# NativeLibraryLoader.cpp). The install rules below still ship them beside
# the executables in bin/, where the loader's executable-dir search
# (always first) finds them.
#
# Directory suffix by generator kind, so the fallback's candidates always
# reach the modules: multi-config executables live in a <config>-named
# directory (leaf == config name), so native/$<CONFIG> is found by the
# walk-up's native/<leaf> candidate. Single-config generators put the
# executables in <tool-name> directories, so any per-config subdirectory
# would be unreachable (the walk-up probes native/<tool-name> and native
# only) — the modules must land in native/ with no suffix.
if(CMAKE_CONFIGURATION_TYPES)
    set(NLANG_NATIVE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/native/$<CONFIG>")
else()
    set(NLANG_NATIVE_OUTPUT_DIR "${CMAKE_BINARY_DIR}/native")
endif()
foreach(mod IN LISTS NLANG_NATIVE_MODULES)
    set_target_properties(nlang_${mod} PROPERTIES
        LIBRARY_OUTPUT_DIRECTORY "${NLANG_NATIVE_OUTPUT_DIR}"
        RUNTIME_OUTPUT_DIRECTORY "${NLANG_NATIVE_OUTPUT_DIR}")
endforeach()

# Install: the modules ship beside the executables in bin/, where the loader's
# executable-dir search finds them.
install(TARGETS nlang_math nlang_io nlang_fs nlang_tensor
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION bin)
