# ---------------------------------------------------------------------------
# radiod : the radio-control daemon.
#
# In the target architecture this is the SINGLE PL/OpenCPI OWNER — the only
# process that opens the OpenCPI application. Every other process reaches the
# radio through librc (the control protocol), never by touching the PL itself.
#
# That ownership is NOT yet implemented: the OpenCPI application XML, worker
# names, properties and ACI lifecycle are all unspecified (see unknown.md,
# U-03 and U-04). radiod today owns a mock backend behind the same
# hh_radio_ops_t vtable a real backend will implement, so its control loop,
# lifecycle state machine and IPC are already backend-agnostic and will not
# need to change when a real backend arrives.
#
# DEPENDENCY RULE:
#   radiod -> hhsdr_radiod -> hhsdr_protocol -> hhsdr_core
# hhsdr_radiod must never depend on librc or radioctl; data flows the other
# way. Clients depend on the protocol, not on the daemon.
# ---------------------------------------------------------------------------
add_library(hhsdr_radiod STATIC
  src/radiod.c
  src/config.c
  src/backends/mock_backend.c
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

  add_executable(test_radiod_config tests/test_radiod_config.c)
  target_link_libraries(test_radiod_config PRIVATE hhsdr_radiod)
  target_include_directories(test_radiod_config PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  add_test(NAME test_radiod_config COMMAND test_radiod_config)

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
