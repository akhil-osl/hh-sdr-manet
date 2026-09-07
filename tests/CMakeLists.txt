# Test infrastructure. Nothing under tests/ may be linked into production targets.
include_directories(${CMAKE_SOURCE_DIR}/tests ${CMAKE_SOURCE_DIR}/tests/sim)

# Test-only simulation infrastructure. Never linked into production targets.
add_library(hhsim STATIC sim/mock_radio.c)
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
