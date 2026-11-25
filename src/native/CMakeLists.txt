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

set(NLANG_NATIVE_MODULES math io fs tensor net system gzip)

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
    tensor/tensor_where.cpp
    tensor/tensor_scatter.cpp
    tensor/tensor_scatter_add.cpp
    tensor/tensor_sort.cpp
    tensor/tensor_take_along.cpp
    tensor/tensor_unique.cpp
    tensor/tensor_assign_into.cpp
    
    tensor/tensor_matrix.cpp
    tensor/tensor_reduce.cpp
    tensor/tensor_views.cpp
    tensor/tensor_activation.cpp
    tensor/tensor_gather.cpp
    tensor/tensor_ops_cpu.cpp
    tensor/backend_registry.cpp
    tensor/plugin_loader.cpp
    tensor/backend_stream.cpp
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
        net/net_platform_posix.cpp
        net/net_platform_icmp_posix.cpp)
endif()

# gzip module statically inlines vendored zlib 1.3.1 (spec D23); the
# zlib sources are compiled directly into the module target — no
# separate static lib, no external dependency.
target_sources(nlang_gzip PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/zlib/adler32.c
    ${PROJECT_SOURCE_DIR}/third_party/zlib/compress.c
    ${PROJECT_SOURCE_DIR}/third_party/zlib/crc32.c
    ${PROJECT_SOURCE_DIR}/third_party/zlib/deflate.c
    ${PROJECT_SOURCE_DIR}/third_party/zlib/infback.c
    ${PROJECT_SOURCE_DIR}/third_party/zlib/inffast.c
    ${PROJECT_SOURCE_DIR}/third_party/zlib/inflate.c
    ${PROJECT_SOURCE_DIR}/third_party/zlib/inftrees.c
    ${PROJECT_SOURCE_DIR}/third_party/zlib/trees.c
    ${PROJECT_SOURCE_DIR}/third_party/zlib/uncompr.c
    ${PROJECT_SOURCE_DIR}/third_party/zlib/zutil.c)
target_include_directories(nlang_gzip PRIVATE ${PROJECT_SOURCE_DIR}/third_party/zlib)
target_compile_definitions(nlang_gzip PRIVATE _CRT_SECURE_NO_WARNINGS)

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

#--- v0.14.x device line: the CUDA plugin (spec D5 — the vendor
#runtime ships inside the plugin DLL; the build compiles the kernels
#via nvcc directly — the CUDA VS integration is not a dependency).
#Optional: the NLANG_CUDA_ROOT cache var gates the target (absent =
#the plugin target skips; the cpu/mirror backends are unaffected).
set(NLANG_CUDA_ROOT "" CACHE PATH "CUDA toolkit root (enables the cuda device plugin)")
if(NLANG_CUDA_ROOT)
    if(WIN32)
        set(NLANG_CUDA_NVCC "${NLANG_CUDA_ROOT}/bin/nvcc.exe")
    else()
        set(NLANG_CUDA_NVCC "${NLANG_CUDA_ROOT}/bin/nvcc")
    endif()
    if(NOT EXISTS "${NLANG_CUDA_NVCC}")
        message(WARNING "NLANG_CUDA_ROOT set but nvcc not found — the cuda plugin is skipped")
    else()
        set(NLANG_CUDA_ARCH "sm_86" CACHE STRING
            "CUDA arch (sm_86 = RTX 30-class, sm_89 = RTX 40-class)")
        set(NLANG_TENSOR_CUDA_OBJ
            "${CMAKE_CURRENT_BINARY_DIR}/tensor_cuda/cuda_kernels${CMAKE_CXX_COMPILER_RECOGNIZED_OBJECT_SUFFIX}")
        file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/tensor_cuda")
        if(WIN32)
            set(NLANG_CUDA_NVCC_HOST_FLAGS
                -ccbin "D:/dev/VS2022/VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe"
                -Xcompiler "/EHsc /GR /std:c++17 /wd4819 /MD")
            set(NLANG_CUDA_LINK_LIBS
                "${NLANG_CUDA_ROOT}/lib/x64/cudart_static.lib")
            set(NLANG_CUDA_LINK_FLAGS "/NODEFAULTLIB:LIBCMT")
        else()
            #g++ is nvcc's default host compiler: no -ccbin. -fPIC because
            #the kernel object lands in a shared library; -fmad=false
            #matches the Windows host flags so kernel numerics are
            #identical across platforms.
            set(NLANG_CUDA_NVCC_HOST_FLAGS
                -Xcompiler "-fPIC,-O3,-std=c++17")
            #Dynamic cudart: the CUDA runtime loads with the plugin .so
            #(RTLD_LOCAL, no interposing into the host).
            set(NLANG_CUDA_LINK_LIBS
                "${NLANG_CUDA_ROOT}/lib64/libcudart.so" cuda)
            set(NLANG_CUDA_LINK_FLAGS "")
        endif()
        add_custom_command(
            OUTPUT "${NLANG_TENSOR_CUDA_OBJ}"
            COMMAND "${NLANG_CUDA_NVCC}" -arch=${NLANG_CUDA_ARCH}
                ${NLANG_CUDA_NVCC_HOST_FLAGS}
                -I"${NLANG_CUDA_ROOT}/include"
                -I"${PROJECT_SOURCE_DIR}/include"
                -I"${PROJECT_SOURCE_DIR}/src/vm"
                -I"${CMAKE_CURRENT_SOURCE_DIR}/tensor"
                "-std=c++17" -fmad=false
                -c "${CMAKE_CURRENT_SOURCE_DIR}/tensor_cuda/cuda_kernels.cu"
                -o "${NLANG_TENSOR_CUDA_OBJ}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tensor_cuda/cuda_kernels.cu"
                "${CMAKE_CURRENT_SOURCE_DIR}/tensor_cuda/cuda_kernels.h"
                "${CMAKE_CURRENT_SOURCE_DIR}/tensor_cuda/cuda_mt19937.cuh"
            COMMENT "nvcc: cuda_kernels.cu (${NLANG_CUDA_ARCH} + PTX)")
        add_library(nlang_tensor_cuda SHARED
            tensor_cuda/cuda_backend.cpp
            tensor_cuda/backend_descriptor.h
            "${NLANG_TENSOR_CUDA_OBJ}")
        target_include_directories(nlang_tensor_cuda
            PRIVATE ${PROJECT_SOURCE_DIR}/include
                ${PROJECT_SOURCE_DIR}/src/vm
                ${CMAKE_CURRENT_SOURCE_DIR}/tensor
                ${NLANG_CUDA_ROOT}/include)
        set_target_properties(nlang_tensor_cuda PROPERTIES
            CXX_STANDARD 17
            CXX_STANDARD_REQUIRED ON
            LINK_FLAGS "${NLANG_CUDA_LINK_FLAGS}")
        target_link_libraries(nlang_tensor_cuda PRIVATE
            ${NLANG_CUDA_LINK_LIBS})
        set_target_properties(nlang_tensor_cuda PROPERTIES
            LIBRARY_OUTPUT_DIRECTORY "${NLANG_NATIVE_OUTPUT_DIR}"
            RUNTIME_OUTPUT_DIRECTORY "${NLANG_NATIVE_OUTPUT_DIR}")
    endif()
endif()

# Install: the modules ship beside the executables in bin/, where the loader's
# executable-dir search finds them.
install(TARGETS nlang_math nlang_io nlang_fs nlang_tensor nlang_net nlang_system nlang_gzip
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION bin)
