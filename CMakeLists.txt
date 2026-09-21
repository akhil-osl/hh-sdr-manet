cmake_minimum_required(VERSION 3.10)
project(hh_sdr_manet C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Debug)
endif()

add_compile_options(-Wall -Wextra -Wshadow -Wpointer-arith -Wcast-qual -Wstrict-prototypes)

option(HH_SANITIZE "Build with address/UB sanitizers" OFF)
if(HH_SANITIZE)
  # add_link_options() requires CMake 3.13; this project targets 3.10.
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -fsanitize=address,undefined")
endif()

include_directories(${CMAKE_SOURCE_DIR}/include)

# ---------------------------------------------------------------------------
# hhsdr_core : production library. Must never depend on tests/ or the simulator.
# ---------------------------------------------------------------------------
add_library(hhsdr_core STATIC
  src/core/types.c
  src/core/clock.c
  src/core/log.c
  src/core/config.c
  src/core/events.c
  src/core/dispatcher.c
  src/radio/wire.c
  src/adapters/hw/hw_adapter.c
  src/manet/discovery.c
  src/manet/neighbor.c
  src/manet/link_health.c
  src/manet/route_table.c
  src/manet/routing.c
  src/dataplane/forwarder.c
  src/manet/failure_detector.c
  src/manet/topology.c
  src/manet/self_healing.c
  src/manet/node.c
  src/manet/telemetry.c
  src/sca/resource.c
)
target_link_libraries(hhsdr_core PUBLIC m)

# ---------------------------------------------------------------------------
# protocol : the control-protocol codec, shared by radiod and its clients.
# Kept separate from both so that a client library never has to link the
# daemon. See protocol/CMakeLists.txt.
# ---------------------------------------------------------------------------
add_subdirectory(protocol)

# Node daemon. Links only the production library — never the test simulator.
add_executable(hh-manet src/main.c)
target_link_libraries(hh-manet PRIVATE hhsdr_core)

# ---------------------------------------------------------------------------
# radiod : standalone radio-control daemon (control plane only) plus its
# mock backend and client library. Depends on hhsdr_core for rc.h/radio.h
# and core utilities, never the other way around.
# ---------------------------------------------------------------------------
add_library(hhsdr_radiod STATIC
  src/radiod/mock_backend.c
  src/radiod/radiod.c
  src/radiod/rc_client.c
)
target_link_libraries(hhsdr_radiod PUBLIC hhsdr_core hhsdr_protocol)

add_executable(radiod tools/radiod/radiod_main.c)
target_link_libraries(radiod PRIVATE hhsdr_radiod)

enable_testing()
add_subdirectory(tests)
