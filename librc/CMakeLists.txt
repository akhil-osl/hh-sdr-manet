# ---------------------------------------------------------------------------
# librc : the radio-control client library.
#
# The "librc" box of the three-plane architecture. Every out-of-process client
# of radiod goes through here: radioctl, test automation, and in the target
# architecture the routing daemon and applications.
#
# DEPENDENCY RULE — this is the whole point of the component:
#   librc -> hhsdr_protocol -> hhsdr_core
# and NOTHING else. librc must never link hhsdr_radiod. Previously this code
# lived inside the daemon library, which meant any client had to link the
# entire daemon in order to talk to it.
# ---------------------------------------------------------------------------
add_library(librc STATIC
  src/rc_client.c
)

# Avoid the default "liblibrc.a"; the artifact is librc.a.
set_target_properties(librc PROPERTIES OUTPUT_NAME rc)

target_include_directories(librc PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(librc PUBLIC hhsdr_protocol)
