#FAQ fact pin (ctest entry ncc_no_o_output_cwd): `ncc build src.n` without
#-o writes <stem>.nmod into the PROCESS CWD, not beside the source. Both
#halves are asserted: the .nmod must appear in the scratch CWD and must
#not appear next to the source. Arguments:
#  NCC - ncc binary; SOURCE - the .n file; CWD_DIR - scratch working dir.
cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED NCC OR NOT DEFINED SOURCE OR NOT DEFINED CWD_DIR)
    message(FATAL_ERROR "usage: cmake -DNCC=... -DSOURCE=... -DCWD_DIR=... -P ncc_no_o_output_cwd.cmake")
endif()

get_filename_component(stem "${SOURCE}" NAME_WE)
set(cwd_nmod "${CWD_DIR}/${stem}.nmod")
set(beside_nmod "${SOURCE}/../${stem}.nmod")
get_filename_component(beside_nmod "${beside_nmod}" ABSOLUTE)

#Pre-clean both candidate locations so a stray artifact from an earlier
#run cannot fake or mask the fact under test.
file(REMOVE "${cwd_nmod}" "${beside_nmod}")
file(MAKE_DIRECTORY "${CWD_DIR}")

execute_process(
    COMMAND "${NCC}" build "${SOURCE}"
    WORKING_DIRECTORY "${CWD_DIR}"
    RESULT_VARIABLE rc
    OUTPUT_QUIET ERROR_QUIET)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "ncc build failed with exit ${rc}")
endif()

if(NOT EXISTS "${cwd_nmod}")
    message(FATAL_ERROR
        "ncc without -o did not write ${stem}.nmod into the working "
        "directory (${CWD_DIR})")
endif()

if(EXISTS "${beside_nmod}")
    message(FATAL_ERROR
        "ncc without -o wrote the .nmod beside the source "
        "(${beside_nmod}); expected the working directory only")
endif()

message(STATUS "ncc without -o wrote ${stem}.nmod into the CWD: OK")
