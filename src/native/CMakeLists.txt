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

set(NLANG_NATIVE_MODULES math io fs)

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

# Deploy every module next to each host tool. The loader's default search path
# is the host executable's directory. ncc/nvm/ndb always exist; nide only when
# the IDE is built. This subdirectory is added AFTER the tools, so the tool
# targets (and their output dirs) are known here.
set(NLANG_NATIVE_HOST_TOOLS ncc nvm ndb)
if(TARGET nide)
    list(APPEND NLANG_NATIVE_HOST_TOOLS nide)
endif()
foreach(mod IN LISTS NLANG_NATIVE_MODULES)
    foreach(tool IN LISTS NLANG_NATIVE_HOST_TOOLS)
        add_custom_command(TARGET nlang_${mod} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:nlang_${mod}>
                $<TARGET_FILE_DIR:${tool}>
            VERBATIM)
    endforeach()
endforeach()

# Install: the modules ship beside the executables in bin/, where the loader's
# executable-dir search finds them.
install(TARGETS nlang_math nlang_io nlang_fs
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION bin)
