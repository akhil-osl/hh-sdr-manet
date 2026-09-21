# Test infrastructure. Nothing under tests/ may be linked into production targets.
include_directories(${CMAKE_SOURCE_DIR}/tests ${CMAKE_SOURCE_DIR}/tests/sim)

# Test-only simulation infrastructure. Never linked into production targets.
add_library(hhsim STATIC sim/mock_radio.c sim/netsim.c)
target_link_libraries(hhsim PUBLIC hhsdr_core)

function(hh_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE hhsdr_core hhsim)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

hh_add_test(test_seq        unit/test_seq.c)
hh_add_test(test_config     unit/test_config.c)
hh_add_test(test_log        unit/test_log.c)
hh_add_test(test_dispatcher unit/test_dispatcher.c)
hh_add_test(test_wire       unit/test_wire.c)
hh_add_test(test_radio      unit/test_radio.c)
hh_add_test(test_discovery  unit/test_discovery.c)
hh_add_test(test_neighbor   unit/test_neighbor.c)
hh_add_test(test_link_health unit/test_link_health.c)
hh_add_test(test_routing    unit/test_routing.c)
hh_add_test(test_forwarder  unit/test_forwarder.c)
hh_add_test(test_recovery   unit/test_recovery.c)
hh_add_test(test_integration integration/test_integration.c)
hh_add_test(test_scenarios  scenario/test_scenarios.c)
hh_add_test(test_topology_scenarios scenario/test_topology_scenarios.c)
hh_add_test(test_route_verification scenario/test_route_verification.c)
hh_add_test(test_status_scenarios scenario/test_status_scenarios.c)
hh_add_test(test_failure_recovery_chain scenario/test_failure_recovery_chain.c)
hh_add_test(test_telemetry_sca unit/test_telemetry_sca.c)
hh_add_test(test_telemetry_verification unit/test_telemetry_verification.c)
hh_add_test(test_node        unit/test_node.c)
hh_add_test(test_status_events unit/test_status_events.c)
hh_add_test(test_interface_compliance unit/test_interface_compliance.c)
hh_add_test(test_rc_protocol unit/test_rc_protocol.c)

# radiod-specific tests link hhsdr_radiod in addition to hhsdr_core.
add_executable(test_mock_backend unit/test_mock_backend.c)
target_link_libraries(test_mock_backend PRIVATE hhsdr_radiod)
add_test(NAME test_mock_backend COMMAND test_mock_backend)

add_executable(test_radiod_state_machine unit/test_radiod_state_machine.c)
target_link_libraries(test_radiod_state_machine PRIVATE hhsdr_radiod)
add_test(NAME test_radiod_state_machine COMMAND test_radiod_state_machine)

# End-to-end: spawns the real radiod binary and drives it over its socket.
add_executable(test_radiod_daemon radiod/test_radiod_daemon.c)
target_link_libraries(test_radiod_daemon PRIVATE hhsdr_radiod)
target_compile_definitions(test_radiod_daemon PRIVATE
  RADIOD_BINARY_PATH="$<TARGET_FILE:radiod>")
add_dependencies(test_radiod_daemon radiod)
add_test(NAME test_radiod_daemon COMMAND test_radiod_daemon)

# Tests for the simulation tooling itself. Needs the tools/sim sources, which
# are development tooling and never linked into the production library.
add_executable(test_simui unit/test_simui.c ${CMAKE_SOURCE_DIR}/tools/sim/simui.c)
target_include_directories(test_simui PRIVATE ${CMAKE_SOURCE_DIR}/tools/sim)
target_link_libraries(test_simui PRIVATE hhsdr_core hhsim)
add_test(NAME test_simui COMMAND test_simui)

# Demonstration binary: prints network behavior for a human reader.
# Not registered as a test; it is a tool for inspecting the stack by eye.
add_executable(demo_selfheal ${CMAKE_SOURCE_DIR}/tools/demo_selfheal.c)
target_link_libraries(demo_selfheal PRIVATE hhsdr_core hhsim)

# hh-manet-sim: the multi-node simulation driver. Development tooling only --
# it links the production library plus the test simulator, and is never linked
# into the hh-manet daemon.
add_executable(hh-manet-sim
  ${CMAKE_SOURCE_DIR}/tools/sim/hh_manet_sim.c
  ${CMAKE_SOURCE_DIR}/tools/sim/simui.c)
target_include_directories(hh-manet-sim PRIVATE ${CMAKE_SOURCE_DIR}/tools/sim)
target_link_libraries(hh-manet-sim PRIVATE hhsdr_core hhsim)
set_target_properties(hh-manet-sim PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})

# hh-manet-telemetry-serve: long-running netsim driver that serves telemetry
# over TCP for the MA-OI GUI integration (docs/GUI-INTEGRATION-PLAN.md).
# Development tooling only -- same status as hh-manet-sim above, never
# linked into the hh-manet daemon. Reuses simui.c only for node-id naming.
add_executable(hh-manet-telemetry-serve
  ${CMAKE_SOURCE_DIR}/tools/sim/telemetry_serve.c
  ${CMAKE_SOURCE_DIR}/tools/sim/telemetry_server.c
  ${CMAKE_SOURCE_DIR}/tools/sim/simui.c)
target_include_directories(hh-manet-telemetry-serve PRIVATE ${CMAKE_SOURCE_DIR}/tools/sim)
target_link_libraries(hh-manet-telemetry-serve PRIVATE hhsdr_core hhsim)
set_target_properties(hh-manet-telemetry-serve PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR})
