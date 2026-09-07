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
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
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
)
target_link_libraries(hhsdr_core PUBLIC m)

enable_testing()
add_subdirectory(tests)
