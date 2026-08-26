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
# Full path to makensis.exe for the NSIS installer (leave empty to look it
# up on PATH). Only needed on the machine producing the release packages.
set(NLANG_NSIS_MAKENSIS "" CACHE FILEPATH "Path to makensis.exe for CPack NSIS")

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
    find_package(Qt5 COMPONENTS Core Widgets REQUIRED)
endif()

# Subdirectories
add_subdirectory(src/runtime)
add_subdirectory(src/compiler)
add_subdirectory(src/vm)
add_subdirectory(src/3rdparty/tinyxml2)
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

# ---- Install rules & release packaging -------------------------------------
# The release package is a self-contained layout mirroring the proven nide
# deploy dir (nide_deploy_check): every executable and the Qt runtime live
# together in bin/, because nide invokes ncc/nvm from its own directory.
# Build packages from the IDE tree (NLANG_BUILD_IDE=ON, which contains all
# tools) with:  cpack -C Release -B <release_dir>
install(TARGETS ncc nvm ndisasm RUNTIME DESTINATION bin)

if(NLANG_BUILD_IDE)
    install(TARGETS nide RUNTIME DESTINATION bin)
    # install(TARGETS) cannot carry imported Qt DLLs; $<TARGET_FILE:...> is
    # the documented idiom for shipping prebuilt runtime dependencies.
    install(FILES
        $<TARGET_FILE:Qt5::Core>
        $<TARGET_FILE:Qt5::Gui>
        $<TARGET_FILE:Qt5::Widgets>
        DESTINATION bin)
    # Ship only the default platform plugin — the Qt plugins dir also
    # carries debug variants (qwindowsd.dll etc.) that don't belong in a
    # release package. "*d.dll"-style excludes are unsafe (qdirect2d.dll
    # is a release name), so whitelist the one file we need.
    install(DIRECTORY "${Qt5_DIR}/../../../plugins/platforms/"
        DESTINATION bin/platforms
        FILES_MATCHING PATTERN "qwindows.dll")
endif()

install(DIRECTORY examples/ DESTINATION examples)
# Internal dev-process docs (specs/plans, phase-history roadmap) stay out
# of the public package — they reference files the package doesn't ship.
install(DIRECTORY docs/ DESTINATION docs
    PATTERN "superpowers" EXCLUDE
    PATTERN "roadmap.md" EXCLUDE)
install(FILES LICENSE README.md DESTINATION .)

set(CPACK_PACKAGE_NAME "NLang")
set(CPACK_PACKAGE_VENDOR "NLang Project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "NLang scripting language compiler, virtual machine and IDE")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "NLang")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

if(WIN32)
    set(CPACK_GENERATOR "ZIP;NSIS")
    # One deterministic file name for both generators (zip / installer).
    set(CPACK_PACKAGE_FILE_NAME "NLang-${PROJECT_VERSION}-win64")
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
    set(CPACK_NSIS_DISPLAY_NAME "NLang ${PROJECT_VERSION}")
    set(CPACK_NSIS_PACKAGE_NAME "NLang ${PROJECT_VERSION}")
    if(NLANG_NSIS_MAKENSIS)
        # CPACK_NSIS_EXECUTABLE exists since CMake 3.21; on older CMake the
        # option would silently fall back to a PATH lookup of makensis.
        if(CMAKE_VERSION VERSION_LESS 3.21)
            message(WARNING "NLANG_NSIS_MAKENSIS requires CMake 3.21+ "
                "(CPACK_NSIS_EXECUTABLE); it will be ignored by this CMake")
        endif()
        set(CPACK_NSIS_EXECUTABLE "${NLANG_NSIS_MAKENSIS}")
    endif()
    if(NLANG_BUILD_IDE)
        # Start Menu shortcut; executables are in the default bin/ dir.
        set(CPACK_PACKAGE_EXECUTABLES "nide;NLang IDE")
    endif()
endif()

include(CPack)
