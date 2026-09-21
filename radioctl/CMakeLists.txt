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
