#3.24 for $<LINK_GROUP:RESCAN> (the nlang_vm<->nlang_compiler static-library
#cycle, see NLANG_CORE_LINK below).
cmake_minimum_required(VERSION 3.24)

#Single version source: the VERSION file feeds project()/CPack, the
#generated nlang_version.h and the docs site. file(READ) does not watch
#its input, so the file must be registered as a re-configure dependency
#explicitly -- otherwise a version bump would not retrigger configure.
set_property(DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS VERSION)
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
    message(FATAL_ERROR "VERSION file missing at the repository root "
        "(it holds the single-source NLang version, e.g. 0.1.0)")
endif()
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" NLANG_VERSION)
string(STRIP "${NLANG_VERSION}" NLANG_VERSION)
if(NOT NLANG_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+[a-z]?$")
    message(FATAL_ERROR "VERSION must be a semantic version (x.y.z with "
        "an optional letter tag), found: '${NLANG_VERSION}'")
endif()
#project() takes a numeric x.y.z only; the letter tag (a work-version
#marker like 0.13.1a) rides along in the full string for CPack naming
#and the --version output.
string(REGEX MATCH "^[0-9]+\\.[0-9]+\\.[0-9]+" NLANG_VERSION_NUMERIC
    "${NLANG_VERSION}")

# C is enabled for the vendored libvterm (pure C99 static library).
project(NLang VERSION ${NLANG_VERSION_NUMERIC} LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# MSVC decodes narrow string literals in the system codepage unless
# told otherwise, silently corrupting UTF-8 sources (e.g. Chinese
# document/UI text pinned by the nide tests). GCC/Clang already
# default to UTF-8, so the flag is only needed there.
if(MSVC)
    add_compile_options(/utf-8)
    #Exception unwinding semantics must hold in every build, including
    #ones that override CMAKE_CXX_FLAGS (e.g. -fsanitize=address): an
    #explicit override replaces CMake's default flags and silently drops
    #this switch, after which destructors are not guaranteed to run
    #during unwinding (C4530). Pinning it here makes that impossible.
    add_compile_options(/EHsc)
endif()

#Generated version header (build tree only, single source: the VERSION
#file; the numeric components come from the numeric prefix so a letter
#tag never reaches MAJOR/MINOR/PATCH). The INTERFACE target carries the
#include dir to the tools.
string(REPLACE "." ";" NLANG_VERSION_LIST "${NLANG_VERSION_NUMERIC}")
list(GET NLANG_VERSION_LIST 0 NLANG_VERSION_MAJOR)
list(GET NLANG_VERSION_LIST 1 NLANG_VERSION_MINOR)
list(GET NLANG_VERSION_LIST 2 NLANG_VERSION_PATCH)
configure_file(cmake/nlang_version.h.in
    ${CMAKE_CURRENT_BINARY_DIR}/generated/nlang_version.h @ONLY)
add_library(nlang_version INTERFACE)
target_include_directories(nlang_version INTERFACE
    ${CMAKE_CURRENT_BINARY_DIR}/generated)

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
        $<TARGET_FILE:Qt5::Qml>
        $<TARGET_FILE:Qt5::QmlModels>
        $<TARGET_FILE:Qt5::Quick>
        $<TARGET_FILE:Qt5::QuickWidgets>
        $<TARGET_FILE:Qt5::Network>
        $<TARGET_FILE:Qt5::PrintSupport>)
    #WebChannel and Positioning ride the windeployqt WebEngine closure,
    #but a Qt build may omit the modules entirely (the acceptance box's
    #self-built Qt does); their targets then do not exist and even a
    #variable mention of $<TARGET_FILE:...> for them breaks the generate
    #step wherever the list is expanded. Guard by target existence so
    #any conforming Qt configures — a Qt without them ships a
    #WebEngineCore that does not link them either.
    foreach(NLANG_QTWEBENGINE_OPTIONAL WebChannel Positioning)
        if(TARGET Qt5::${NLANG_QTWEBENGINE_OPTIONAL})
            list(APPEND NLANG_QTWEBENGINE_DLLS
                "$<TARGET_FILE:Qt5::${NLANG_QTWEBENGINE_OPTIONAL}>")
        endif()
    endforeach()
    get_filename_component(NLANG_QTWEBENGINE_BIN
        "${Qt5_DIR}/../../../bin" ABSOLUTE)
    get_filename_component(NLANG_QTWEBENGINE_RESOURCES
        "${Qt5_DIR}/../../../resources" ABSOLUTE)
    get_filename_component(NLANG_QTWEBENGINE_LOCALES
        "${Qt5_DIR}/../../../translations/qtwebengine_locales" ABSOLUTE)
    #The helper process follows Qt's debug naming (QtWebEngineProcessd
    #under Debug) while the DLL list above rides $<TARGET_FILE:...> and
    #is config-aware already — splice the suffix per configuration so
    #a Debug build launches the Debug process (the release one beside
    #Debug DLLs fails to load and the help browser dies at startup).
    set(NLANG_QTWEBENGINE_PROCESS
        "${NLANG_QTWEBENGINE_BIN}/QtWebEngineProcess$<$<CONFIG:Debug>:d>.exe")
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


# ---- AddressSanitizer runtime provisioning ----------------------------------
# MSVC links the sanitizer DYNAMIC runtime: the flags carry
# -fsanitize=address as a plain compiler flag, not the EnableASAN project
# property, so MSBuild never copies clang_rt.asan_dynamic-x86_64.dll beside
# linked executables — an instrumented exe in an unprovisioned directory
# dies at load time (0xc0000135). Each executable-creating CMakeLists ends
# with one sweep call (add_custom_command(TARGET ... POST_BUILD) is
# directory-bound, so the sweep cannot run centrally); the recursion below
# covers the directory's whole subtree. cl.exe shares its directory with
# the clang_rt runtime DLLs, so the compiler path is the lookup.
function(nlang_copy_asan_runtime target)
    get_filename_component(_asan_bin_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(_asan_dll "${_asan_bin_dir}/clang_rt.asan_dynamic-x86_64.dll")
    if(NOT EXISTS "${_asan_dll}")
        message(FATAL_ERROR "AddressSanitizer runtime DLL not found beside "
            "the compiler: ${_asan_dll}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${_asan_dll}" $<TARGET_FILE_DIR:${target}>
        VERBATIM)
endfunction()

#Attach the provisioning to every executable target created in dir.
#(add_custom_command(TARGET ...) is directory-bound, so the call lives in
#the CMakeLists that creates the targets, not a central sweep.) No-op on
#non-MSVC trees and builds without the sanitizer flag.
function(_nlang_sweep_asan_runtime dir)
    if(NOT MSVC OR NOT CMAKE_CXX_FLAGS MATCHES "fsanitize=address")
        return()
    endif()
    get_property(_targets DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
    foreach(_t IN LISTS _targets)
        get_property(_type TARGET ${_t} PROPERTY TYPE)
        if(_type STREQUAL "EXECUTABLE")
            nlang_copy_asan_runtime(${_t})
        endif()
    endforeach()

endfunction()

# The nlang_vm<->nlang_compiler static-library cycle: ModuleBuilder
# (compiler) drives VmBackend (the vm's bytecode emitter), which in turn
# consumes the compiler's BuildEnvironment/ModuleRegistry/IsTypeMember.
# MSVC's multi-pass linker resolves such a cycle natively and rejects the
# RESCAN group at generate time; GNU ld is single-pass and needs the group
# rescan. Link the plain archives on MSVC, the rescan group elsewhere.
# (CMake 3.24+ for $<LINK_GROUP:RESCAN>.)
if(MSVC)
    set(NLANG_CORE_LINK nlang_vm nlang_compiler nlang_runtime)
else()
    set(NLANG_CORE_LINK
        $<LINK_GROUP:RESCAN,nlang_vm;nlang_compiler;nlang_runtime>)
endif()

# Subdirectories
add_subdirectory(src/runtime)
add_subdirectory(src/langservice)
add_subdirectory(src/compiler)
add_subdirectory(src/vm)
add_subdirectory(src/embed)
#C++ example hosts link nlang_embed; the .n examples ship via the
#install(DIRECTORY examples/) rule further down.
add_subdirectory(examples)
add_subdirectory(src/3rdparty/tinyxml2)
add_subdirectory(src/tools/ncc)
add_subdirectory(src/tools/nvm)
add_subdirectory(src/tools/ndisasm)
add_subdirectory(src/tools/ndb)
add_subdirectory(src/tools/nlang)

if(NLANG_BUILD_IDE)
    #libvterm is only consumed by nide (terminal emulation), so it sits
    #inside the IDE gate and costs nothing in tool-only builds.
    add_subdirectory(src/3rdparty/libvterm)
    add_subdirectory(src/tools/nide)
endif()
# Native stdlib modules are added after the tools so their POST_BUILD step can
# deploy each module into every host tool's output directory.
add_subdirectory(src/native)

# --- Standard library package (phase 6) --------------------------------------
# Bootstrap the stdlib package with the just-built ncc: the library sources
# are copied into the build tree's stdlib dir — which doubles as the
# compile-time stdlib source dir every tool resolves via FindStdLibDir
# (walk-up from the executable directory finds this copy first in every
# build-tree layout) — and the generated .nproj builds them project-mode,
# so stdlib.npkg carries one member per unit. Executors resolve stdlib
# symbol imports from that package at load time (nloader -> nlink; design
# section 5). The target is ALWAYS-run on purpose: no stamp means no
# staleness hole when only ncc changed (the pack is cheap and
# byte-deterministic). Targeted builds of a single tool skip it — run a
# full build (or this target) before running anything that imports the
# standard library.
set(NLANG_STDLIB_BUILD_DIR ${CMAKE_BINARY_DIR}/stdlib)
file(MAKE_DIRECTORY ${NLANG_STDLIB_BUILD_DIR})
# ONE source list drives every consumer of the file set (the generated
# .nproj and the copy below; the install rule ships the whole stdlib/
# directory and is list-free by construction). A new stdlib/<name>.n
# must not depend on remembering to edit each site — a missed copy also
# misses the package member, because the build-tree copy shadows the
# source tree for compile-time resolution. CONFIGURE_DEPENDS: cmake
# re-runs (and the list refreshes) whenever the glob result changes, so
# adding or removing a stdlib source needs no manual reconfigure.
# copy_if_different never DELETES: removing a stdlib source leaves its
# stale copy in the build-tree dir, still shadowing the source tree
# (consumers compile against a signature the package no longer ships)
# — delete the stale copy from the build-tree stdlib dir by hand after
# a source removal.
file(GLOB NLANG_STDLIB_SOURCES CONFIGURE_DEPENDS
    ${PROJECT_SOURCE_DIR}/stdlib/*.n)
#The prelude packages are compile-time-inline only: the compiler embeds
#stdlib/lang.n and stdlib/containers.n at build time, so the .nproj
#pack list excludes them — a prelude member in the package would be a
#member no import can reach. The build-tree COPY keeps both files: the
#stdlib dir is the FindStdLibDir hit and the disk copy is the canonical
#prelude source for compile-time resolution (edit-and-rebuild
#iteration).
set(NLANG_STDLIB_PACKAGE_SOURCES ${NLANG_STDLIB_SOURCES})
list(REMOVE_ITEM NLANG_STDLIB_PACKAGE_SOURCES
    ${PROJECT_SOURCE_DIR}/stdlib/lang.n
    ${PROJECT_SOURCE_DIR}/stdlib/containers.n)
#One string argument, not two: a multi-value set() joins with ";" and
#the separator would land between the XML declaration and the root
#element (tinyxml2 happens to tolerate the stray text node).
set(NLANG_STDLIB_NPROJ "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<Project name=\"stdlib\">\n  <Sources>\n")
foreach(src IN LISTS NLANG_STDLIB_PACKAGE_SOURCES)
    get_filename_component(name "${src}" NAME)
    string(APPEND NLANG_STDLIB_NPROJ "    <File path=\"${name}\"/>\n")
endforeach()
string(APPEND NLANG_STDLIB_NPROJ "  </Sources>\n</Project>\n")
file(WRITE ${NLANG_STDLIB_BUILD_DIR}/stdlib.nproj "${NLANG_STDLIB_NPROJ}")
add_custom_target(nlang_stdlib_package ALL
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${NLANG_STDLIB_SOURCES}
        ${NLANG_STDLIB_BUILD_DIR}
    COMMAND $<TARGET_FILE:ncc> build -p
        ${NLANG_STDLIB_BUILD_DIR}/stdlib.nproj
        -o ${NLANG_STDLIB_BUILD_DIR}/stdlib.npkg
    DEPENDS ncc
    COMMENT "Packing stdlib.npkg (per-unit members)"
    VERBATIM)

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
        ${CMAKE_CURRENT_SOURCE_DIR}/docs/user_manual/zh/*.md
        ${CMAKE_CURRENT_SOURCE_DIR}/docs/user_manual/en/*.md)
    #ALL + no OUTPUT: every build reruns the pipeline once (seconds of
    #Python startup, accepted; add a stamp file if it ever becomes a
    #pain). Each language tree is its own mkdocs build (strict + offline
    #search + snippet audit), merged under site/{zh,en}; the static
    #landing page becomes site/index.html (two links + a
    #navigator.language jump, all file:// friendly).
    add_custom_target(nlang_docs ALL
        #Start every run from a clean site root, so stale artifacts of
        #an earlier layout cannot survive into the tree that the
        #install(DIRECTORY docs/site/) rule ships. Build-time cleanup
        #(inside the target's command list): configure-time file()
        #would only run on re-configure, leaving incremental builds
        #with the stale tree.
        COMMAND ${CMAKE_COMMAND} -E remove_directory
                ${CMAKE_CURRENT_BINARY_DIR}/docs/site
        COMMAND ${NLANG_DOCS_RUN} build
                --config ${CMAKE_CURRENT_SOURCE_DIR}/docs/mkdocs.zh.yml
                --site-dir ${CMAKE_CURRENT_BINARY_DIR}/docs/site/zh
                #The 4th build stage audits the manual tree's snippets
                #by compiling+running them with the tree's own tools,
                #so a language change that breaks a documented snippet
                #fails the docs build.
                --ncc $<TARGET_FILE:ncc>
                --nvm $<TARGET_FILE:nvm>
                #The trees cross-link each other, so their pages only
                #resolve once BOTH are built: defer the site audits
                #until after the second build (see the check commands).
                --defer-site-audit
        COMMAND ${NLANG_DOCS_RUN} build
                --config ${CMAKE_CURRENT_SOURCE_DIR}/docs/mkdocs.en.yml
                --site-dir ${CMAKE_CURRENT_BINARY_DIR}/docs/site/en
                --ncc $<TARGET_FILE:ncc>
                --nvm $<TARGET_FILE:nvm>
                --defer-site-audit
        COMMAND ${NLANG_DOCS_RUN} check
                --site-dir ${CMAKE_CURRENT_BINARY_DIR}/docs/site/zh
                --config ${CMAKE_CURRENT_SOURCE_DIR}/docs/mkdocs.zh.yml
        COMMAND ${NLANG_DOCS_RUN} check
                --site-dir ${CMAKE_CURRENT_BINARY_DIR}/docs/site/en
                --config ${CMAKE_CURRENT_SOURCE_DIR}/docs/mkdocs.en.yml
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${NLANG_DOCS_DIR}/src/nlang_docs/data/landing.html
                ${CMAKE_CURRENT_BINARY_DIR}/docs/site/index.html
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/docs/mkdocs.base.yml
                ${CMAKE_CURRENT_SOURCE_DIR}/docs/mkdocs.zh.yml
                ${CMAKE_CURRENT_SOURCE_DIR}/docs/mkdocs.en.yml
                ${NLANG_DOC_PAGES}
                ${NLANG_DOCS_DIR}/src/nlang_docs/cli.py
                ${NLANG_DOCS_DIR}/src/nlang_docs/linkcheck.py
                ${NLANG_DOCS_DIR}/src/nlang_docs/offline_search.py
                ${NLANG_DOCS_DIR}/src/nlang_docs/snippets.py
                ${NLANG_DOCS_DIR}/src/nlang_docs/data/landing.html
        WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
        COMMENT "nlang-docs: building + auditing bilingual docs site"
        VERBATIM)
    # The snippet audit compiles manual programs with $<TARGET_FILE:ncc>,
    # which resolves `import io` through the build-tree stdlib package.
    # Without this edge a parallel build can schedule the audit while the
    # (always-run) stdlib pack is still refreshing after an ncc rebuild,
    # and every importing snippet fails with "Module 'io' not found".
    # The audit also RUNS the built nvm against the native modules, so it
    # must wait for the tools and the module aggregate (see
    # src/native/CMakeLists.txt) — without these edges a contract change
    # (e.g. a new required export) lets the audit execute a half-rebuilt
    # tree and fail, which MSBuild then reports as an MSB8066 docs
    # "flake" while silently skipping still-pending targets.
    add_dependencies(nlang_docs ncc nvm nlang_stdlib_package
        nlang_native_modules)
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
install(TARGETS ncc nvm ndisasm ndb nlang embed_host RUNTIME DESTINATION bin)

# Standard library declarations, installed at the top level next to bin/ so
# FindStdLibDir (which walks up from the executable's directory) resolves them
# in the installed layout: prefix/bin/ncc -> prefix/stdlib.
install(DIRECTORY stdlib/ DESTINATION stdlib
    FILES_MATCHING PATTERN "*.n")
# The packed standard library (built into the build tree's stdlib dir by
# the nlang_stdlib_package bootstrap) ships beside the .n declarations —
# executors resolve stdlib unit imports from it at load time (phase 6).
install(FILES ${CMAKE_BINARY_DIR}/stdlib/stdlib.npkg DESTINATION stdlib)

if(NLANG_BUILD_IDE)
    install(TARGETS nide RUNTIME DESTINATION bin)
    if(WIN32)
        # install(TARGETS) cannot carry imported Qt DLLs; $<TARGET_FILE:...> is
        # the documented idiom for shipping prebuilt runtime dependencies.
        # The whole Qt runtime closure below is the Windows deploy shape.
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
    elseif(UNIX)
        #The Linux deploy shape: same beside-exe layout as Windows (the
        #IDE resolves ncc/nvm/ndb from its own directory), with the
        #shared-library names and Qt paths of a prefix-installed Qt —
        #the helper process lives in libexec/ and Chromium's paks in
        #resources/ (self-built Qt prefix layout; apt puts them under
        #share/qt5 instead). The tools themselves install through the
        #unconditional install(TARGETS) above.
        install(FILES
            $<TARGET_FILE:Qt5::Core>
            $<TARGET_FILE:Qt5::Gui>
            $<TARGET_FILE:Qt5::Widgets>
            DESTINATION bin)
        install(FILES ${NLANG_QTWEBENGINE_DLLS}
            DESTINATION bin)
        get_filename_component(NLANG_QT_PREFIX
            "${Qt5_DIR}/../../.." ABSOLUTE)
        install(FILES "${NLANG_QT_PREFIX}/libexec/QtWebEngineProcess"
            DESTINATION libexec)
        install(FILES ${NLANG_QTWEBENGINE_RESOURCE_FILES}
            DESTINATION resources)
        install(FILES ${NLANG_QTWEBENGINE_LOCALE_FILES}
            DESTINATION translations/qtwebengine_locales)
        #The whole platforms directory (offscreen + xcb): the offscreen
        #plugin is what the deployment check's probes run under.
        install(DIRECTORY "${NLANG_QT_PREFIX}/plugins/platforms"
            DESTINATION plugins)
        #The dlopen'd native modules ride beside the executables in the
        #deployed layout (same reason the Windows check hand-assembles
        #them from NATIVE_DLL_DIR).
        install(DIRECTORY "${CMAKE_BINARY_DIR}/native/"
            DESTINATION bin
            FILES_MATCHING PATTERN "libnlang_*.so*")
        #Qt resolves a prefix-relative qt.conf beside the executable;
        #pin plugins/resources to the layout instead of the dev Qt.
        install(CODE
            "file(WRITE \"\${CMAKE_INSTALL_PREFIX}/bin/qt.conf\"
                \"[Paths]\\nPrefix=..\\n\")")
    endif()
endif()

install(DIRECTORY examples/ DESTINATION examples)
install(FILES LICENSE README.md README.zh-CN.md CHANGELOG.md
        CHANGELOG.zh-CN.md DESTINATION .)

set(CPACK_PACKAGE_NAME "NLang")
set(CPACK_PACKAGE_VENDOR "NLang Project")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "NLang scripting language compiler, virtual machine and IDE")
set(CPACK_PACKAGE_VERSION "${NLANG_VERSION}")
set(CPACK_PACKAGE_INSTALL_DIRECTORY "NLang")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE")

if(WIN32)
    set(CPACK_GENERATOR "ZIP;NSIS")
    # One deterministic file name for both generators (zip / installer).
    set(CPACK_PACKAGE_FILE_NAME "NLang-${NLANG_VERSION}-win64")
    set(CPACK_NSIS_INSTALL_ROOT "$PROGRAMFILES64")
    set(CPACK_NSIS_DISPLAY_NAME "NLang ${NLANG_VERSION}")
    set(CPACK_NSIS_PACKAGE_NAME "NLang ${NLANG_VERSION}")
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
elseif(UNIX)
    #The portable-archive counterpart of the Windows zip: the same
    #install layout, unpack-and-run. TGZ over XZ for the widest tar
    #compatibility; the premise is a prefix-installed Qt (see the
    #install rules' Linux arm).
    set(CPACK_GENERATOR "TGZ")
    set(CPACK_PACKAGE_FILE_NAME "NLang-${NLANG_VERSION}-linux64")
endif()

include(CPack)
