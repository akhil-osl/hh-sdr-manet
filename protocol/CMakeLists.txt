# ---------------------------------------------------------------------------
# hhsdr_protocol : the control-protocol codec shared by radiod and its clients.
#
# Deliberately independent of both the daemon and the client library: it knows
# how to encode and decode control messages and nothing else. That is what lets
# librc depend on the protocol without dragging in the daemon.
#
# The current codec is the line-oriented ASCII key=value protocol. The target
# architecture calls for versioned TLV (ICD-2), which has no specification yet
# (see unknown.md, U-01). When it exists, its codec is added here alongside the
# existing one -- no radiod or librc change required.
# ---------------------------------------------------------------------------
add_library(hhsdr_protocol STATIC
  src/rc.c
)

target_include_directories(hhsdr_protocol PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# hh_status_t / hh_node_id_t come from core types.
target_link_libraries(hhsdr_protocol PUBLIC hhsdr_core)
