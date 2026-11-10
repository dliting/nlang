#--- nide deployment self-containment check (Phase 10 Step 9) ---
#Copies the assembled nide output (nide.exe + Qt DLLs + platforms/ +
#ncc/nvm/ndb, placed there by the POST_BUILD steps) plus the stdlib
#native modules (NLANG_NATIVE_DLLS -- they live only under
#<build>/native/<config>, so the install-form beside-exe layout is
#assembled here) to a scratch directory and runs a curated subset of
#the main-window test suite from there with the QPA plugin path
#cleared. The test exe must resolve the Qt DLLs (app-dir first), the
#platform plugin, the stdlib package, the native modules and
#ncc/nvm/ndb purely from the copied layout.
#
#Division of concerns: business behavior is the in-tree
#mainwindow_tests run's job (the full 115-test suite runs there).
#This check owns ONE property -- self-containment of the copied
#layout -- so it runs one test per resolution class instead of
#re-running the whole suite. Re-running everything couples this
#layout gate to the business suite's runtime (which varies 2x+ with
#machine load) and adds no coverage: a test that passes in-tree
#only fails here for layout reasons, and those reasons fall into
#the classes below.
#
#A console test exe is used because the GUI nide.exe never exits on
#its own, and both kinds hang in the CRT-abort/WER path instead of
#exiting when platform init fails -- so the only trustworthy signal
#is "exit code 0 within the bound".
#
#Resolution classes and their one probe each:
#  testOpenStandaloneFileJoinsTree   Qt DLLs, platform plugin, widgets
#  testF12OpensStdLibDefinition      stdlib package + symbol index files
#  testBuildAndRun                   ncc + nvm spawn, ConPTY terminal run
#  testStdinRowFeedsRunningProgram   live PTY interaction (deep terminal)
#  testDebugPagePopulatesOnStop      ndb spawn + machine debug protocol
#When a new resolution class joins the deployment (a new shipped DLL
#family, a new tool), add its probe here -- do not fall back to the
#full-suite rerun.

if(NOT EXISTS "${DEPLOY_DIR}/nide.exe")
    message(FATAL_ERROR
        "nide.exe not found in ${DEPLOY_DIR} -- build target nide first")
endif()
#The debug probe drives ndb from the app directory, so a missing copy
#must fail here as a layout defect, not inside a test.
if(NOT EXISTS "${DEPLOY_DIR}/ndb.exe")
    message(FATAL_ERROR
        "ndb.exe not found in ${DEPLOY_DIR} -- the nide POST_BUILD copy "
        "list is missing it")
endif()
if(NOT EXISTS "${TEST_EXE}")
    message(FATAL_ERROR
        "test_mainwindow.exe not found at ${TEST_EXE} -- build it first")
endif()
#The probe run goes through the progress wrapper (see below), so both
#halves of it must be there; failing here says "the caller did not pass
#them" instead of "the probe suite did not pass".
foreach(helper IN ITEMS PYTHON3_EXE QTEST_PROGRESS)
    if(NOT EXISTS "${${helper}}")
        message(FATAL_ERROR
            "${helper} is missing or not set (${${helper}}) -- "
            "tests/CMakeLists.txt must pass it to this script")
    endif()
endforeach()

file(REMOVE_RECURSE "${CHECK_DIR}")
file(MAKE_DIRECTORY "${CHECK_DIR}")
file(COPY "${DEPLOY_DIR}/" DESTINATION "${CHECK_DIR}")
file(COPY "${TEST_EXE}" "${QT_TEST_DLL}" DESTINATION "${CHECK_DIR}")

#Stdlib native modules no longer land in the tool dirs (single
#authoritative <build>/native/<config> output since 0.11.4), so the
#install-form layout -- DLLs beside the executable, where the loader's
#executable-dir search finds them -- is materialized here explicitly.
file(COPY ${NLANG_NATIVE_DLLS} DESTINATION "${CHECK_DIR}")

#The scratch is nested 9 levels under <build>/tests, beyond the reach of
#the walk-up discovery used for stdlib (and native modules), so nothing
#can rescue the check from the build tree. The install form instead
#ships stdlib within walk-up reach of the executables; materializing it
#beside the exe exercises FindStdLibDir's exe-relative probe — every
#resolution class must come from the copied layout.
file(COPY "${STDLIB_DIR}/" DESTINATION "${CHECK_DIR}/stdlib")

#Pin Qt's paths to the scratch copy: without this, Qt falls back to
#the compiled-in plugin dir of whatever Qt build produced the DLLs
#(QLibraryInfo::PluginsPath), and a dev machine with Qt installed
#would silently rescue a broken deployment. Prefix=. makes the app
#dir the only search root -- the deployment property under test.
file(WRITE "${CHECK_DIR}/qt.conf" "[Paths]\nPrefix=.\n")

#Healthy run: well under a minute (five tests, a handful of real
#ncc/nvm/ndb child sessions). A broken layout hangs in abort (see
#header), and hanging has to be both stopped and explained, which a
#bare execute_process TIMEOUT does neither of:
#  * the test exe holds its own -o log under an exclusive lock for the
#    whole run, so a run cmake itself kills leaves the log unreadable --
#    the failure arrives with no output at all;
#  * the abort/WER path also leaves the spawned ncc/nvm/ndb children
#    running behind it.
#tools/qtest_progress.py owns both jobs: at the bound it kills the whole
#child tree, and because QTest unlocks the log when the process dies, it
#then reads that log and records the last completed probe. Its 124 means
#"hit the bound"; any other code is the test exe's own.
set(RESULT_FILE "${CHECK_DIR}/deploy_check_result.txt")
set(PROGRESS_FILE "${CHECK_DIR}/deploy_check_progress.txt")
#The inner bound is settable from the command line so the timeout branch
#(kill the tree, name the last completed probe) can be measured in
#seconds instead of 150; the outer backstop keeps its margin behind
#whatever bound is in use.
if(NOT PROBE_TIMEOUT)
    set(PROBE_TIMEOUT 150)
endif()
math(EXPR OUTER_TIMEOUT "${PROBE_TIMEOUT} + 25")
execute_process(
    COMMAND "${PYTHON3_EXE}" "${QTEST_PROGRESS}"
        --qtest-log "${RESULT_FILE}"
        --progress-log "${PROGRESS_FILE}"
        --timeout ${PROBE_TIMEOUT}
        -- "${CHECK_DIR}/test_mainwindow.exe"
            testOpenStandaloneFileJoinsTree
            testF12OpensStdLibDefinition
            testBuildAndRun
            testStdinRowFeedsRunningProgram
            testDebugPagePopulatesOnStop
    WORKING_DIRECTORY "${CHECK_DIR}"
    #Backstop for a wedged wrapper: the inner bound kills the test exe
    #first, so reaching this one means the wrapper never got to report.
    TIMEOUT ${OUTER_TIMEOUT}
    RESULT_VARIABLE result)

if(NOT result EQUAL 0)
    #Both logs are readable whenever the wrapper did the killing (the
    #test exe is dead, so its locks are gone); after the outer bound the
    #progress file is the wrapper's own and still shows how far it got.
    foreach(log IN ITEMS "${RESULT_FILE}" "${PROGRESS_FILE}")
        if(EXISTS "${log}")
            file(READ "${log}" log_text)
            message("${log}:\n${log_text}")
        endif()
    endforeach()
    if(result EQUAL 124)
        message(FATAL_ERROR
            "the deployment probe suite hit the ${PROBE_TIMEOUT} s bound; "
            "the last completed probe named above says where it stopped")
    endif()
    message(FATAL_ERROR
        "the deployment probe suite did not pass from the scratch "
        "deployment (result: ${result})")
endif()
