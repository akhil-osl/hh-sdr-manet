# ---------------------------------------------------------------------------
# ATP/BIT evidence tooling (see README.md). Development and test tooling only:
# nothing here is linked into, or installed with, a production target.
# ---------------------------------------------------------------------------

# Golden-capture generator for the Wireshark dissector. Links the production
# codec on purpose — the point is to check the dissector against the real
# encoder and decoder, not against a copy of the layout.
add_executable(hh_wire_pcapgen wireshark/hh_wire_pcapgen.c)
target_link_libraries(hh_wire_pcapgen PRIVATE hhsdr_core)

if(HH_BUILD_TESTS)
  # Dissector vs C codec, plus atp-decode.sh evidence on the same capture.
  # Needs tshark (with Lua) and jq on the build host; exits 77 without them,
  # which ctest reports as skipped rather than passed.
  #
  # The netns traffic self-test (selftest/netns-selftest.sh) is deliberately
  # NOT registered: it moves real traffic for ~45 s, and ctest stays fast and
  # deterministic.
  add_test(NAME check_dissector
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/wireshark/check-dissector.sh
            $<TARGET_FILE:hh_wire_pcapgen> ${CMAKE_CURRENT_BINARY_DIR}/dissector)
  set_tests_properties(check_dissector PROPERTIES SKIP_RETURN_CODE 77)

  # atp-radioctl.sh against the real radiod and radioctl binaries: the public
  # CLI path the architecture requires test automation to use. Needs jq.
  add_test(NAME check_atp_radioctl
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/selftest/radioctl-selftest.sh
            $<TARGET_FILE:radiod> $<TARGET_FILE:radioctl>
            ${CMAKE_CURRENT_BINARY_DIR}/radioctl)
  set_tests_properties(check_atp_radioctl PROPERTIES SKIP_RETURN_CODE 77)
endif()
