cmake_minimum_required(VERSION 3.16)
project(NLang VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC decodes narrow string literals in the system codepage unless
# told otherwise, silently corrupting UTF-8 sources (e.g. Chinese
# document/UI text pinned by the nide tests). GCC/Clang already
# default to UTF-8, so the flag is only needed there.
if(MSVC)
    add_compile_options(/utf-8)
endif()

# Options
option(NLANG_ENABLE_LLVM "Enable LLVM code generation backend" OFF)
option(NLANG_BUILD_IDE "Build the NLang IDE (requires Qt5)" OFF)
option(NLANG_BUILD_TESTS "Build tests" OFF)
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
    #QuickWidgets and PrintSupport are not pulled in transitively by
    #WebEngineWidgets, yet the runtime closure needs their DLLs
    #(Qt5WebEngineWidgets statically imports PrintSupport and delay-
    #loads QuickWidgets; windeployqt's mapping lists both).
    find_package(Qt5 COMPONENTS Core Widgets WebEngineWidgets
        QuickWidgets PrintSupport REQUIRED)

    # QtWebEngine runtime closure for the embedded help browser: the
    # widget DLLs plus the helper process, Chromium resource paks and
    # locales the view launches from the application directory (the
    # layout from "Deploying Qt WebEngine Applications" in the Qt
    # docs; shared by the nide/test POST_BUILD steps and the install
    # rules below).
    set(NLANG_QTWEBENGINE_DLLS
        $<TARGET_FILE:Qt5::WebEngineWidgets>
        $<TARGET_FILE:Qt5::WebEngineCore>
        $<TARGET_FILE:Qt5::WebChannel>
        $<TARGET_FILE:Qt5::Qml>
        $<TARGET_FILE:Qt5::QmlModels>
        $<TARGET_FILE:Qt5::Quick>
        $<TARGET_FILE:Qt5::QuickWidgets>
        $<TARGET_FILE:Qt5::Network>
        $<TARGET_FILE:Qt5::Positioning>
        $<TARGET_FILE:Qt5::PrintSupport>)
    get_filename_component(NLANG_QTWEBENGINE_BIN
        "${Qt5_DIR}/../../../bin" ABSOLUTE)
    get_filename_component(NLANG_QTWEBENGINE_RESOURCES
        "${Qt5_DIR}/../../../resources" ABSOLUTE)
    get_filename_component(NLANG_QTWEBENGINE_LOCALES
        "${Qt5_DIR}/../../../translations/qtwebengine_locales" ABSOLUTE)
    set(NLANG_QTWEBENGINE_PROCESS
        "${NLANG_QTWEBENGINE_BIN}/QtWebEngineProcess.exe")
    # icudtl.dat and the 3 scale-variant paks; the devtools pak stays
    # out (the help viewer opens no DevTools).
    set(NLANG_QTWEBENGINE_RESOURCE_FILES
        "${NLANG_QTWEBENGINE_RESOURCES}/icudtl.dat"
        "${NLANG_QTWEBENGINE_RESOURCES}/qtwebengine_resources.pak"
        "${NLANG_QTWEBENGINE_RESOURCES}/qtwebengine_resources_100p.pak"
        "${NLANG_QTWEBENGINE_RESOURCES}/qtwebengine_resources_200p.pak")
    # Only the UI languages the IDE itself ships.
    set(NLANG_QTWEBENGINE_LOCALE_FILES
        "${NLANG_QTWEBENGINE_LOCALES}/en-US.pak"
        "${NLANG_QTWEBENGINE_LOCALES}/zh-CN.pak")

    # Copy the closure next to a target's executable (DLLs and the
    # helper process beside the exe, paks under resources/, locales
    # under translations/qtwebengine_locales/).
    function(nlang_copy_qtwebengine_runtime target)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${NLANG_QTWEBENGINE_DLLS}
                "${NLANG_QTWEBENGINE_PROCESS}"
                $<TARGET_FILE_DIR:${target}>
            COMMAND ${CMAKE_COMMAND} -E make_directory
                $<TARGET_FILE_DIR:${target}>/resources
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${NLANG_QTWEBENGINE_RESOURCE_FILES}
                $<TARGET_FILE_DIR:${target}>/resources
            COMMAND ${CMAKE_COMMAND} -E make_directory
                $<TARGET_FILE_DIR:${target}>/translations/qtwebengine_locales
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${NLANG_QTWEBENGINE_LOCALE_FILES}
                $<TARGET_FILE_DIR:${target}>/translations/qtwebengine_locales
            VERBATIM)
    endfunction()
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

# --- Documentation site (nlang-docs subproject) -----------------------------
# Defined BEFORE add_subdirectory(tests): test targets hook a dependency on
# nlang_docs via if(TARGET nlang_docs), which only sees targets defined at
# that point.
option(NLANG_BUILD_DOCS
    "Build the mkdocs documentation site (needs the pinned Python env)"
    ON)
set(NLANG_DOCS_PYTHON "python" CACHE STRING
    "Python interpreter for the docs target (py313's python.exe)")
set(NLANG_DOCS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tools/nlang-docs)
#The subproject is invoked without installing it (offline builds must
#not need pip): its src/ goes through PYTHONPATH.
set(NLANG_DOCS_RUN
    ${CMAKE_COMMAND} -E env
    PYTHONPATH=${NLANG_DOCS_DIR}/src
    ${NLANG_DOCS_PYTHON} -m nlang_docs)
if(NLANG_BUILD_DOCS)
    #Fail fast with a pointed hint instead of a Python stack trace at
    #build time when the pinned env is missing.
    execute_process(
        COMMAND ${NLANG_DOCS_PYTHON} -m mkdocs --version
        RESULT_VARIABLE NLANG_DOCS_PROBE
        OUTPUT_QUIET ERROR_QUIET)
    if(NOT NLANG_DOCS_PROBE EQUAL 0)
        message(FATAL_ERROR
            "NLANG_BUILD_DOCS needs the nlang-docs pipeline runnable via "
            "'${NLANG_DOCS_PYTHON}': pip install -r "
            "tools/docs-requirements.txt "
            "(or configure with -DNLANG_BUILD_DOCS=OFF)")
    endif()
    file(GLOB_RECURSE NLANG_DOC_PAGES CONFIGURE_DEPENDS
        ${CMAKE_CURRENT_SOURCE_DIR}/docs/*.md)
    #ALL + no OUTPUT: every build reruns the pipeline once (seconds of
    #Python startup, accepted; add a stamp file if it ever becomes a
    #pain). build = mkdocs --strict + linkcheck chained, so a broken
    #link fails the build.
    add_custom_target(nlang_docs ALL
        COMMAND ${NLANG_DOCS_RUN} build
                --config ${CMAKE_CURRENT_SOURCE_DIR}/mkdocs.yml
                --site-dir ${CMAKE_CURRENT_BINARY_DIR}/docs/site
                #The 4th build stage audits the guide's snippets by
                #compiling+running them with the tree's own tools, so a
                #language change that breaks a documented snippet fails
                #the docs build.
                --ncc $<TARGET_FILE:ncc>
                --nvm $<TARGET_FILE:nvm>
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/mkdocs.yml
                ${NLANG_DOC_PAGES}
                ${NLANG_DOCS_DIR}/src/nlang_docs/cli.py
                ${NLANG_DOCS_DIR}/src/nlang_docs/linkcheck.py
                ${NLANG_DOCS_DIR}/src/nlang_docs/offline_search.py
                ${NLANG_DOCS_DIR}/src/nlang_docs/snippets.py
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "nlang-docs: building + auditing docs site"
        VERBATIM)
    # The package ships the generated site, not the markdown sources.
    install(DIRECTORY ${CMAKE_CURRENT_BINARY_DIR}/docs/site/
            DESTINATION docs/site)
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
    # QtWebEngine runtime for the embedded help browser (closure
    # defined next to find_package(Qt5) above).
    install(FILES ${NLANG_QTWEBENGINE_DLLS} "${NLANG_QTWEBENGINE_PROCESS}"
        DESTINATION bin)
    install(FILES ${NLANG_QTWEBENGINE_RESOURCE_FILES}
        DESTINATION bin/resources)
    install(FILES ${NLANG_QTWEBENGINE_LOCALE_FILES}
        DESTINATION bin/translations/qtwebengine_locales)
endif()

install(DIRECTORY examples/ DESTINATION examples)
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
