# ---------------------------------------------------------------------------
# radiod : the radio-control daemon.
#
# In the target architecture this is the SINGLE PL/OpenCPI OWNER — the only
# process that opens the OpenCPI application. Every other process reaches the
# radio through librc (the control protocol), never by touching the PL itself.
#
# Two backends sit behind the same hh_radio_ops_t vtable, chosen by the
# `backend` config key: the mock, and (with -DHH_WITH_OPENCPI=ON) an OpenCPI
# backend that owns an application through the ACI. radiod's control loop,
# state machine and IPC do not depend on which one runs.
#
# DEPENDENCY RULE:
#   radiod -> hhsdr_radiod -> hhsdr_protocol -> hhsdr_core
# hhsdr_radiod must never depend on librc or radioctl; data flows the other
# way. Clients depend on the protocol, not on the daemon.
# ---------------------------------------------------------------------------
add_library(hhsdr_radiod STATIC
  src/radiod.c
  src/config.c
  src/events.c
  src/backends/mock_backend.c
  # Always built, even without OpenCPI, so a daemon lacking the backend can
  # still parse its keys and refuse `backend = ocpi` by name.
  src/backends/ocpi_config.c
)

target_include_directories(hhsdr_radiod PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hhsdr_radiod PUBLIC hhsdr_core_base hhsdr_protocol)

# ---------------------------------------------------------------------------
# OpenCPI backend (C++, optional).
#
# Linked statically against the CDK's own libraries, following the CDK's
# recipe for programs built outside an OpenCPI project (ocpisetup.mk,
# OCPI_API_LIBS). Two corrections to that recipe for OpenCPI 2.4.7, both
# found by linking: the advertised msg_driver_interface library does not
# exist, and base, msg, drc and drc_ad9361 (plus the ad9361 prerequisite) are
# needed but not listed. A link group resolves the circular references among
# them.
#
# The executable must export its symbols (ENABLE_EXPORTS, i.e. -rdynamic):
# OpenCPI loads its RCC container and workers as shared objects at run time,
# and they resolve OpenCPI symbols from the program itself.
# ---------------------------------------------------------------------------
if(HH_WITH_OPENCPI)
  set(HH_OPENCPI_CDK_DIR "$ENV{OCPI_CDK_DIR}" CACHE PATH
      "OpenCPI CDK directory (the one opencpi-setup.sh exports as OCPI_CDK_DIR)")
  set(HH_OPENCPI_PLATFORM "$ENV{OCPI_TOOL_PLATFORM}" CACHE STRING
      "OpenCPI RCC platform whose libraries to link, e.g. ubuntu22_04")
  if(NOT HH_OPENCPI_CDK_DIR OR NOT EXISTS "${HH_OPENCPI_CDK_DIR}/include/aci/OcpiApi.hh")
    message(FATAL_ERROR "HH_WITH_OPENCPI: no OpenCPI CDK at '${HH_OPENCPI_CDK_DIR}'. "
                        "Source opencpi-setup.sh or set -DHH_OPENCPI_CDK_DIR=...")
  endif()
  if(NOT HH_OPENCPI_PLATFORM)
    message(FATAL_ERROR "HH_WITH_OPENCPI: set -DHH_OPENCPI_PLATFORM (e.g. ubuntu22_04)")
  endif()
  set(_ocpi_lib "${HH_OPENCPI_CDK_DIR}/${HH_OPENCPI_PLATFORM}/lib")

  set(_ocpi_archives "")
  foreach(_l application remote_support container library metadata msg transport
             xfer base util foreign os drc drc_ad9361)
    list(APPEND _ocpi_archives "${_ocpi_lib}/libocpi_${_l}.a")
  endforeach()
  foreach(_l ad9361 lzma gmp gpsd)
    list(APPEND _ocpi_archives "${_ocpi_lib}/lib${_l}.a")
  endforeach()

  add_library(hhsdr_radiod_ocpi STATIC src/backends/ocpi_backend.cpp)
  # SYSTEM: OpenCPI's headers are not held to this project's warning flags.
  target_include_directories(hhsdr_radiod_ocpi SYSTEM PRIVATE
    "${HH_OPENCPI_CDK_DIR}/include/aci")
  target_link_libraries(hhsdr_radiod_ocpi PUBLIC hhsdr_radiod
    -Wl,--start-group ${_ocpi_archives} -Wl,--end-group rt dl pthread)
endif()

# The daemon binary. Kept thin: wiring and the control loop only.
add_executable(radiod radiod_main.c)
if(HH_WITH_OPENCPI)
  target_link_libraries(radiod PRIVATE hhsdr_radiod_ocpi)
  target_compile_definitions(radiod PRIVATE HH_HAVE_OPENCPI=1)
  set_target_properties(radiod PROPERTIES ENABLE_EXPORTS ON)
else()
  target_link_libraries(radiod PRIVATE hhsdr_radiod)
endif()

# ---------------------------------------------------------------------------
# Tests. Registered here, with the component they exercise, rather than in the
# top-level tests/ tree -- radiod is self-contained.
# ---------------------------------------------------------------------------
if(HH_BUILD_TESTS)
  add_executable(test_mock_backend tests/test_mock_backend.c)
  target_link_libraries(test_mock_backend PRIVATE hhsdr_radiod)
  target_include_directories(test_mock_backend PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  add_test(NAME test_mock_backend COMMAND test_mock_backend)

  add_executable(test_radiod_config tests/test_radiod_config.c)
  target_link_libraries(test_radiod_config PRIVATE hhsdr_radiod)
  target_include_directories(test_radiod_config PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  add_test(NAME test_radiod_config COMMAND test_radiod_config)

  add_executable(test_radiod_faults tests/test_radiod_faults.c)
  target_link_libraries(test_radiod_faults PRIVATE hhsdr_radiod)
  target_include_directories(test_radiod_faults PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  add_test(NAME test_radiod_faults COMMAND test_radiod_faults)

  add_executable(test_radiod_state_machine tests/test_radiod_state_machine.c)
  target_link_libraries(test_radiod_state_machine PRIVATE hhsdr_radiod)
  target_include_directories(test_radiod_state_machine PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  add_test(NAME test_radiod_state_machine COMMAND test_radiod_state_machine)

  # End-to-end: spawns the real radiod binary and drives it over its socket.
  # Links librc ONLY -- not hhsdr_radiod. That is deliberate: the test drives
  # the daemon exactly as an external client would, which proves librc is
  # genuinely independent of the daemon rather than merely filed separately.
  add_executable(test_radiod_daemon tests/test_radiod_daemon.c)
  target_link_libraries(test_radiod_daemon PRIVATE librc)
  target_include_directories(test_radiod_daemon PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_compile_definitions(test_radiod_daemon PRIVATE
    RADIOD_BINARY_PATH="$<TARGET_FILE:radiod>")
  add_dependencies(test_radiod_daemon radiod)
  add_test(NAME test_radiod_daemon COMMAND test_radiod_daemon)
endif()

# OpenCPI backend tests: real OpenCPI applications built from the stock
# file_read/file_write components, so they need the CDK at run time too.
if(HH_BUILD_TESTS AND HH_WITH_OPENCPI)
  set(HH_OPENCPI_LIBRARY_PATH "${HH_OPENCPI_CDK_DIR}/../projects/core/exports/artifacts"
      CACHE PATH "Artifact directory holding ocpi.core file_read/file_write for the tests")
  set(_ocpi_test_env
    "OCPI_CDK_DIR=${HH_OPENCPI_CDK_DIR}"
    "OCPI_LIBRARY_PATH=${HH_OPENCPI_LIBRARY_PATH}")
  set(_ocpi_fixtures "${CMAKE_CURRENT_SOURCE_DIR}/tests/ocpi")

  add_executable(test_ocpi_backend tests/test_ocpi_backend.c)
  target_link_libraries(test_ocpi_backend PRIVATE hhsdr_radiod_ocpi)
  target_include_directories(test_ocpi_backend PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_compile_definitions(test_ocpi_backend PRIVATE
    HH_OCPI_FIXTURE_DIR="${_ocpi_fixtures}")
  set_target_properties(test_ocpi_backend PROPERTIES ENABLE_EXPORTS ON)
  add_test(NAME test_ocpi_backend COMMAND test_ocpi_backend
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(test_ocpi_backend PROPERTIES ENVIRONMENT "${_ocpi_test_env}")

  # Like test_radiod_daemon, links librc only and drives the real binary.
  add_executable(test_radiod_ocpi_daemon tests/test_radiod_ocpi_daemon.c)
  target_link_libraries(test_radiod_ocpi_daemon PRIVATE librc)
  target_include_directories(test_radiod_ocpi_daemon PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_compile_definitions(test_radiod_ocpi_daemon PRIVATE
    RADIOD_BINARY_PATH="$<TARGET_FILE:radiod>"
    HH_OCPI_FIXTURE_DIR="${_ocpi_fixtures}")
  add_dependencies(test_radiod_ocpi_daemon radiod)
  add_test(NAME test_radiod_ocpi_daemon COMMAND test_radiod_ocpi_daemon
           WORKING_DIRECTORY ${CMAKE_CURRENT_BINARY_DIR})
  set_tests_properties(test_radiod_ocpi_daemon PROPERTIES ENVIRONMENT "${_ocpi_test_env}")
endif()
