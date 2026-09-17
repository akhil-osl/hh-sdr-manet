# MANET_Node SCA adapter -- standalone build fragment.
#
# NOT wired into the top-level CMakeLists.txt add_subdirectory() graph, and
# does not build automatically as part of the existing `cmake --build`
# workflow. This is intentional for this phase: the adapter is additive,
# optional, and must not become a build dependency of hh-manet / hh-manet-sim
# / the CTest suite. To build ONLY this target explicitly:
#
#   cmake -S sca/MANET_Node -B sca/MANET_Node/build \
#         -DHHSDR_CORE_LIB=<path-to>/build/libhhsdr_core.a \
#         -DHHSDR_INCLUDE_DIR=<path-to>/include
#   cmake --build sca/MANET_Node/build
#
# This project intentionally does NOT run this build -- see this
# directory's README.md for why, and what remains for a later phase.
cmake_minimum_required(VERSION 3.10)
project(manet_sca_adapter C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

if(NOT HHSDR_INCLUDE_DIR)
  set(HHSDR_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../include")
endif()
if(NOT HHSDR_CORE_LIB)
  set(HHSDR_CORE_LIB "${CMAKE_CURRENT_SOURCE_DIR}/../../build/libhhsdr_core.a")
endif()

add_library(manet_sca_adapter SHARED
  adapter/manet_sca_adapter.c
)
target_include_directories(manet_sca_adapter PUBLIC
  ${HHSDR_INCLUDE_DIR}
  ${CMAKE_CURRENT_SOURCE_DIR}/adapter
)
target_link_libraries(manet_sca_adapter PUBLIC ${HHSDR_CORE_LIB} m)
