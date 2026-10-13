# ---------------------------------------------------------------------------
# radioctl : the radio-control CLI.
#
# The public command-line entry point to the radio. The architecture requires
# test automation to drive the system through this interface rather than
# through private/internal access.
#
# DEPENDENCY RULE: radioctl -> librc -> hhsdr_protocol, and nothing else. It
# must never link hhsdr_radiod, and per the single-owner rule it never touches
# OpenCPI or the PL — every radio operation is a request serviced by radiod.
# ---------------------------------------------------------------------------
add_executable(radioctl radioctl_main.c)
target_link_libraries(radioctl PRIVATE librc)

# ---------------------------------------------------------------------------
# End-to-end through the public CLI: runs the real radioctl binary against the
# real radiod binary. This is the path the architecture requires test
# automation to use, so it is exercised exactly as an ATB/BIT script would.
# ---------------------------------------------------------------------------
if(HH_BUILD_TESTS)
  add_executable(test_radioctl tests/test_radioctl.c)
  target_link_libraries(test_radioctl PRIVATE librc)
  target_include_directories(test_radioctl PRIVATE ${CMAKE_SOURCE_DIR}/tests)
  target_compile_definitions(test_radioctl PRIVATE
    RADIOD_BINARY_PATH="$<TARGET_FILE:radiod>"
    RADIOCTL_BINARY_PATH="$<TARGET_FILE:radioctl>")
  add_dependencies(test_radioctl radiod radioctl)
  add_test(NAME test_radioctl COMMAND test_radioctl)
endif()
