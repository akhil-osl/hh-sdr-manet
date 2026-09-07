# Test infrastructure. Nothing under tests/ may be linked into production targets.
include_directories(${CMAKE_SOURCE_DIR}/tests)

function(hh_add_test name)
  add_executable(${name} ${ARGN})
  target_link_libraries(${name} PRIVATE hhsdr_core)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

hh_add_test(test_seq        unit/test_seq.c)
hh_add_test(test_config     unit/test_config.c)
hh_add_test(test_log        unit/test_log.c)
hh_add_test(test_dispatcher unit/test_dispatcher.c)
