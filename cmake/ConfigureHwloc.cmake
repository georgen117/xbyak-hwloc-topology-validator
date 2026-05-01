# cmake/ConfigureHwloc.cmake
#
# Helper script invoked by ExternalProject_Add on Linux/macOS to run hwloc's
# autotools build system.  It is called as a cmake -P script so that cmake
# itself (rather than a shell wrapper) drives the two-step configure.
#
# Expected variables (passed with -D):
#   HWLOC_SOURCE_DIR  – path to the hwloc source tree (the submodule root)
#   HWLOC_BINARY_DIR  – out-of-source build directory (created by ExternalProject)
#   HWLOC_INSTALL_DIR – installation prefix

message(STATUS "hwloc: running autogen.sh in ${HWLOC_SOURCE_DIR}")
execute_process(
    COMMAND sh "${HWLOC_SOURCE_DIR}/autogen.sh"
    WORKING_DIRECTORY "${HWLOC_SOURCE_DIR}"
    RESULT_VARIABLE _autogen_rc
    OUTPUT_VARIABLE _autogen_out
    ERROR_VARIABLE  _autogen_err
)
if(NOT _autogen_rc EQUAL 0)
    message(FATAL_ERROR "hwloc autogen.sh failed (exit ${_autogen_rc}):\n${_autogen_err}")
endif()

message(STATUS "hwloc: running ./configure --prefix=${HWLOC_INSTALL_DIR}")
execute_process(
    COMMAND sh "${HWLOC_SOURCE_DIR}/configure"
        "--prefix=${HWLOC_INSTALL_DIR}"
        "--disable-lstopo"
        "--disable-tools"
        "--enable-static"
        "--disable-shared"
    WORKING_DIRECTORY "${HWLOC_BINARY_DIR}"
    RESULT_VARIABLE _configure_rc
    OUTPUT_VARIABLE _configure_out
    ERROR_VARIABLE  _configure_err
)
if(NOT _configure_rc EQUAL 0)
    message(FATAL_ERROR "hwloc configure failed (exit ${_configure_rc}):\n${_configure_err}")
endif()
