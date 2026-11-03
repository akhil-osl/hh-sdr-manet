# ---------------------------------------------------------------------------
# Host test application (see README.md). Python, standard library only.
# Development and test tooling: nothing here is linked into, or installed
# with, a production target.
# ---------------------------------------------------------------------------
if(HH_BUILD_TESTS)
  find_program(HH_PYTHON3 python3)
  if(HH_PYTHON3)
    add_test(NAME hosttest_unit
      COMMAND ${HH_PYTHON3} -m unittest discover -s tests
      WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR})
    set_tests_properties(hosttest_unit PROPERTIES
      ENVIRONMENT "PYTHONDONTWRITEBYTECODE=1;PYTHONPATH=${CMAKE_CURRENT_SOURCE_DIR}:${CMAKE_CURRENT_SOURCE_DIR}/tests")
  else()
    message(STATUS "python3 not found: hosttest_unit not registered")
  endif()
endif()
