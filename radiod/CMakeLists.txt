# ---------------------------------------------------------------------------
# radiod : the radio-control daemon.
#
# In the target architecture this is the SINGLE PL/OpenCPI OWNER — the only
# process that opens the OpenCPI application. Every other process reaches the
# radio through librc (the control protocol), never by touching the PL itself.
#
# That ownership is now implemented, by the `opencpi` backend: it reaches the
# PL through the rh_agent helper process and the RADHAWK API (api/radhawk.h in
# the OpenCPI project). radiod remains the only owner -- it spawns the agent,
# holds its pipes, and kills it on exit -- and the agent additionally holds
# RADHAWK's cross-process run lock while a session is open.
#
# BUILD DEPENDENCIES: none were added. No OpenCPI header is included and no
# OpenCPI symbol is referenced anywhere in this library, because the ACI lives
# in the separate agent binary; opencpi_agent_proto.h explains why that
# boundary is the only one this CDK allows. radiod therefore builds and links
# identically on a host with no OpenCPI installation, and the mock backend
# remains the default.
#
# The `opencpi` backend also implements the second, orthogonal property vtable
# (props.h), which is what the opencpi_* protocol verbs are serviced from; the
# mock implements only hh_radio_ops_t and those verbs then report
# HH_ERR_UNSUPPORTED.
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
  src/ring.c
  src/backends/mock_backend.c
  src/backends/opencpi_backend.c
)

target_include_directories(hhsdr_radiod PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(hhsdr_radiod PUBLIC hhsdr_core_base hhsdr_protocol)

# The daemon binary. Kept thin: wiring and the control loop only.
add_executable(radiod radiod_main.c)
target_link_libraries(radiod PRIVATE hhsdr_radiod)

# ---------------------------------------------------------------------------
# Tests. Registered here, with the component they exercise, rather than in the
# top-level tests/ tree -- radiod is self-contained.
# ---------------------------------------------------------------------------
if(HH_BUILD_TESTS)
  add_executable(test_mock_backend tests/test_mock_backend.c)
  target_link_libraries(test_mock_backend PRIVATE hhsdr_radiod)
  target_include_directories(test_mock_backend PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  add_test(NAME test_mock_backend COMMAND test_mock_backend)

  add_executable(test_ring tests/test_ring.c)
  target_link_libraries(test_ring PRIVATE hhsdr_radiod)
  target_include_directories(test_ring PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  add_test(NAME test_ring COMMAND test_ring)

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

  # Worker-property routing, against a props vtable defined in the test. What
  # is under test is radiod's own behaviour: the unsupported path, the running
  # flag, and verbatim pass-through of names, values and failure detail.
  add_executable(test_radiod_props tests/test_radiod_props.c)
  target_link_libraries(test_radiod_props PRIVATE hhsdr_radiod)
  target_include_directories(test_radiod_props PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  add_test(NAME test_radiod_props COMMAND test_radiod_props)

  # ---------------------------------------------------------------------------
  # The OpenCPI backend.
  #
  # stub_agent is TEST-ONLY infrastructure -- a deterministic implementation of
  # the rh_agent wire contract, in the same spirit as tests/sim/mock_radio.c. It
  # is what lets the whole chain be exercised on a machine with no OpenCPI
  # installation and no FPGA. It is never linked into a production target; the
  # real agent is applications/rh_agent/ in the OpenCPI project and is built
  # there, by OpenCPI's own application.mk.
  # ---------------------------------------------------------------------------
  add_executable(stub_agent tests/stub_agent.c)
  target_link_libraries(stub_agent PRIVATE hhsdr_radiod)

  add_executable(test_radiod_opencpi tests/test_radiod_opencpi.c)
  # librc as well as the daemon library: half the test drives the backend in
  # process, and half drives the real radiod binary over its socket.
  target_link_libraries(test_radiod_opencpi PRIVATE hhsdr_radiod librc)
  target_include_directories(test_radiod_opencpi PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_compile_definitions(test_radiod_opencpi PRIVATE
    RADIOD_BINARY_PATH="$<TARGET_FILE:radiod>"
    STUB_AGENT_PATH="$<TARGET_FILE:stub_agent>")
  add_dependencies(test_radiod_opencpi radiod stub_agent)
  add_test(NAME test_radiod_opencpi COMMAND test_radiod_opencpi)

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
