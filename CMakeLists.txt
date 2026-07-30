cmake_minimum_required(VERSION 3.16)
project(NLang VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Options
option(NLANG_ENABLE_LLVM "Enable LLVM code generation backend" OFF)
option(NLANG_BUILD_IDE "Build the NLang IDE (requires Qt5)" OFF)
option(NLANG_BUILD_TESTS "Build tests" OFF)
option(NLANG_BUILD_EXAMPLES "Build examples" OFF)

# Python3 for build scripts (optional - used for code generation/post-processing)
# Override with -DPYTHON3_EXECUTABLE=...
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    set(PYTHON3_EXECUTABLE "${Python3_EXECUTABLE}" CACHE FILEPATH "Python3 interpreter")
    message(STATUS "Python3: ${PYTHON3_EXECUTABLE}")
endif()

# Flex/Bison paths (override with -DFLEX_EXE=... -DBISON_EXE=...)
# On Windows, find_package(FLEX/BISON) often fails because win_flex_bison
# is not on PATH. Users can set these cache variables explicitly.
set(FLEX_EXE "" CACHE FILEPATH "Path to flex executable")
set(BISON_EXE "" CACHE FILEPATH "Path to bison executable")
if(NOT FLEX_EXE)
    find_program(FLEX_EXE NAMES flex win_flex)
endif()
if(NOT BISON_EXE)
    find_program(BISON_EXE NAMES bison win_bison)
endif()
if(FLEX_EXE)
    message(STATUS "Flex: ${FLEX_EXE}")
endif()
if(BISON_EXE)
    message(STATUS "Bison: ${BISON_EXE}")
endif()

if(NLANG_ENABLE_LLVM)
    find_package(LLVM REQUIRED CONFIG)
    message(STATUS "Using LLVM ${LLVM_PACKAGE_VERSION}")
endif()

if(NLANG_BUILD_IDE)
    find_package(Qt5 COMPONENTS Core Xml Widgets REQUIRED)
endif()

# Subdirectories
add_subdirectory(src/runtime)
add_subdirectory(src/compiler)
add_subdirectory(src/vm)
add_subdirectory(src/tools/ncc)
add_subdirectory(src/tools/nvm)
add_subdirectory(src/tools/ndisasm)

if(NLANG_BUILD_IDE)
    add_subdirectory(src/tools/nide)
endif()

if(NLANG_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()