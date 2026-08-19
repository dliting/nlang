#--- nide deployment self-containment check (Phase 10 Step 9) ---
#Copies the assembled nide output (nide.exe + Qt DLLs + platforms/ +
#ncc/nvm, placed there by the POST_BUILD steps) to a scratch directory
#and runs the full main-window test suite from there with the QPA
#plugin path cleared. The test exe must resolve the Qt DLLs (app-dir
#first), the platform plugin and ncc/nvm purely from the copied
#layout, and pass all 26 tests. A console test exe is used because the
#GUI nide.exe never exits on its own, and both kinds hang in the
#CRT-abort/WER path instead of exiting when platform init fails -- so
#the only trustworthy signal is "exit code 0 within the bound".

if(NOT EXISTS "${DEPLOY_DIR}/nide.exe")
    message(FATAL_ERROR
        "nide.exe not found in ${DEPLOY_DIR} -- build target nide first")
endif()
if(NOT EXISTS "${TEST_EXE}")
    message(FATAL_ERROR
        "test_mainwindow.exe not found at ${TEST_EXE} -- build it first")
endif()

file(REMOVE_RECURSE "${CHECK_DIR}")
file(MAKE_DIRECTORY "${CHECK_DIR}")
file(COPY "${DEPLOY_DIR}/" DESTINATION "${CHECK_DIR}")
file(COPY "${TEST_EXE}" "${QT_TEST_DLL}" DESTINATION "${CHECK_DIR}")

#Pin Qt's paths to the scratch copy: without this, Qt falls back to
#the compiled-in plugin dir of whatever Qt build produced the DLLs
#(QLibraryInfo::PluginsPath), and a dev machine with Qt installed
#would silently rescue a broken deployment. Prefix=. makes the app
#dir the only search root -- the deployment property under test.
file(WRITE "${CHECK_DIR}/qt.conf" "[Paths]\nPrefix=.\n")

#Healthy run: ~5s. A broken layout hangs in abort (see header) and
#gets killed at the bound, which fails the exit-code check below.
set(RESULT_FILE "${CHECK_DIR}/deploy_check_result.txt")
execute_process(
    COMMAND "${CHECK_DIR}/test_mainwindow.exe" -o "${RESULT_FILE},txt"
    WORKING_DIRECTORY "${CHECK_DIR}"
    TIMEOUT 30
    RESULT_VARIABLE result)

if(NOT result EQUAL 0)
    if(EXISTS "${RESULT_FILE}")
        file(READ "${RESULT_FILE}" tail)
        message("test output:\n${tail}")
    endif()
    message(FATAL_ERROR
        "the main-window suite did not pass from the scratch deployment "
        "(result: ${result})")
endif()
