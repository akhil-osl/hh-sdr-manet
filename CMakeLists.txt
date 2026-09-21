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

# ---------------------------------------------------------------------------
# librc : the radio-control client library. Depends on the protocol codec
# only — never on the daemon.
# ---------------------------------------------------------------------------
add_subdirectory(librc)

# Node daemon. Links only the production library — never the test simulator.
add_executable(hh-manet src/main.c)
target_link_libraries(hh-manet PRIVATE hhsdr_core)

# Tests are registered by each component's own CMakeLists, guarded on this.
# (Deliberately not CTest's BUILD_TESTING: this project calls enable_testing()
# directly rather than include(CTest), so BUILD_TESTING is never defined.)
enable_testing()
set(HH_BUILD_TESTS ON)

# ---------------------------------------------------------------------------
# radiod : the radio-control daemon, and in the target architecture the single
# PL/OpenCPI owner. Self-contained under radiod/, which also registers its own
# tests. See radiod/CMakeLists.txt.
# ---------------------------------------------------------------------------
add_subdirectory(radiod)

# ---------------------------------------------------------------------------
# radioctl : the radio-control CLI. Reaches radiod through librc only.
# ---------------------------------------------------------------------------
add_subdirectory(radioctl)

add_subdirectory(tests)
