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

set(NLANG_NATIVE_MODULES math io fs tensor net)

# Tensor kernel sources build once as an OBJECT library and land both in
# the nlang_tensor DLL and in the native kernel unit tests
# (tests/test_native_tensor) — the tests exercise the exact objects that
# ship, no second source list to drift. The SIMD tier TUs join only on
# x64 (SSE2 is the architectural baseline there); the AVX2 TU carries a
# per-file arch flag (mixed-arch TU discipline: vector types never cross
# the TU boundary — the public interfaces pass raw pointers and counts).
set(NLANG_TENSOR_KERNEL_SOURCES
    tensor/SimdCpu.cpp
    tensor/KernelOpsCpu.cpp
    tensor/VecMathEntries.cpp
    tensor/tensor_binary.cpp
    tensor/tensor_matrix.cpp
    tensor/tensor_reduce.cpp
    tensor/tensor_views.cpp
    tensor/tensor_activation.cpp
    tensor/tensor_gather.cpp
    tensor/tensor_ops_cpu.cpp
    tensor/backend_registry.cpp
    tensor/tensor_random.cpp)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "(AMD64|x86_64|amd64)")
    list(APPEND NLANG_TENSOR_KERNEL_SOURCES
        tensor/VecMathSse2.cpp
        tensor/VecMathAvx2.cpp
        tensor/VecBinarySse2.cpp
        tensor/VecBinaryAvx2.cpp
        tensor/VecReduceSse2.cpp
        tensor/VecReduceAvx2.cpp
        tensor/MatMulSse2.cpp
        tensor/MatMulAvx2.cpp)
    set(NLANG_TENSOR_X64 ON)
endif()

foreach(mod IN LISTS NLANG_NATIVE_MODULES)
    if(mod STREQUAL "tensor")
        add_library(nlang_tensor_kernels OBJECT ${NLANG_TENSOR_KERNEL_SOURCES})
        add_library(nlang_tensor SHARED
            tensor/tensor_native.cpp
            $<TARGET_OBJECTS:nlang_tensor_kernels>)
        set(NLANG_TENSOR_LIBS nlang_tensor nlang_tensor_kernels)
        foreach(tgt IN LISTS NLANG_TENSOR_LIBS)
            target_include_directories(${tgt}
                PRIVATE ${PROJECT_SOURCE_DIR}/include)
            set_target_properties(${tgt} PROPERTIES
                CXX_STANDARD 17
                CXX_STANDARD_REQUIRED ON
                CXX_EXTENSIONS OFF
                POSITION_INDEPENDENT_CODE ON)
        endforeach()
        if(NLANG_TENSOR_X64)
            target_compile_definitions(nlang_tensor_kernels
                PRIVATE NLANG_SIMD_X64=1)
            #Every AVX2 tier TU needs the arch flag — this list must
            #track the SIMD tier sources above (a missed file compiles
            #its intrinsics without the arch contract and misbehaves at
            #runtime).
            if(MSVC)
                set_source_files_properties(
                    tensor/VecMathAvx2.cpp
                    tensor/VecBinaryAvx2.cpp
                    tensor/VecReduceAvx2.cpp
                    tensor/MatMulAvx2.cpp
                    PROPERTIES COMPILE_OPTIONS "/arch:AVX2")
            else()
                set_source_files_properties(
                    tensor/VecMathAvx2.cpp
                    tensor/VecBinaryAvx2.cpp
                    tensor/VecReduceAvx2.cpp
                    tensor/MatMulAvx2.cpp
                    PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma")
            endif()
        endif()
    else()
        add_library(nlang_${mod} SHARED ${mod}/${mod}_native.cpp)
        target_include_directories(nlang_${mod}
            PRIVATE ${PROJECT_SOURCE_DIR}/include)
        set_target_properties(nlang_${mod} PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
            POSITION_INDEPENDENT_CODE ON)
    endif()
endforeach()

# Aggregate target for out-of-directory consumers that need "all modules
# built" as one build-order edge (the docs snippet audit) — depending on
# this name instead of re-listing the modules keeps the list single-sourced.
add_custom_target(nlang_native_modules)
foreach(mod IN LISTS NLANG_NATIVE_MODULES)
    add_dependencies(nlang_native_modules nlang_${mod})
endforeach()

# net: per-executor state + one NetPlatform implementation per platform
# (design §6). StackDump.cpp is compiled in (not linked) so the module's
# NLANG_ASSERT sites resolve in Debug builds — the module links nothing
# but the platform socket libs (the file is self-contained; dbghelp
# auto-links through its own pragma, and it is an empty TU on POSIX).
target_sources(nlang_net PRIVATE
    net/net_jobs.cpp
    net/net_options.cpp
    net/net_slots.cpp
    net/net_state.cpp
    net/net_tcp.cpp
    net/net_tcp_connect.cpp
    net/net_udp.cpp
    net/net_icmp.cpp
    ${PROJECT_SOURCE_DIR}/src/runtime/StackDump.cpp)
if(WIN32)
    target_sources(nlang_net PRIVATE
        net/net_platform_windows.cpp
        net/net_platform_icmp_windows.cpp)
    target_link_libraries(nlang_net PRIVATE ws2_32 iphlpapi)
else()
    target_sources(nlang_net PRIVATE
        net/net_platform_posix.cpp)
endif()

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
install(TARGETS nlang_math nlang_io nlang_fs nlang_tensor nlang_net
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION bin)
