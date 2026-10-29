# ---------------------------------------------------------------------------
# dataterm : data terminal application (F02-AK-4). Text messages as CoAP over
# UDP, built on libcoap. Links hhsdr_core_base and libcoap only.
# ---------------------------------------------------------------------------
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
  pkg_search_module(COAP QUIET libcoap-3 libcoap-3-notls)
endif()
# libcoap's own CMake build installs no pkg-config file, only a CMake package.
if(NOT COAP_FOUND)
  find_package(libcoap CONFIG QUIET)
  if(libcoap_FOUND)
    set(COAP_FOUND TRUE)
    set(COAP_VERSION ${libcoap_VERSION})
    set(COAP_STATIC_LDFLAGS libcoap::coap-3)
  endif()
endif()
if(NOT COAP_FOUND)
  message(WARNING "libcoap not found (pkg-config libcoap-3 or libcoap-3-notls, or CMake package libcoap): "
                  "dataterm and its tests are not built. Install libcoap3-dev.")
  return()
endif()
message(STATUS "dataterm: libcoap ${COAP_VERSION}")

include(CheckSymbolExists)
set(CMAKE_REQUIRED_INCLUDES ${COAP_STATIC_INCLUDE_DIRS})
set(CMAKE_REQUIRED_LIBRARIES ${COAP_STATIC_LDFLAGS})
check_symbol_exists(coap_session_set_app_data2 "coap3/coap.h" HH_COAP_HAS_APP_DATA2)
unset(CMAKE_REQUIRED_INCLUDES)
unset(CMAKE_REQUIRED_LIBRARIES)

add_library(hhsdr_dataterm STATIC
  src/peers.c
  src/dataterm.c
)
target_include_directories(hhsdr_dataterm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
# SYSTEM keeps libcoap's own headers out of our warning flags.
target_include_directories(hhsdr_dataterm SYSTEM PRIVATE ${COAP_STATIC_INCLUDE_DIRS})
target_link_libraries(hhsdr_dataterm PUBLIC hhsdr_core_base ${COAP_STATIC_LDFLAGS})
if(HH_COAP_HAS_APP_DATA2)
  target_compile_definitions(hhsdr_dataterm PRIVATE HH_COAP_HAS_APP_DATA2)
endif()

add_executable(dataterm src/main.c)
target_link_libraries(dataterm PRIVATE hhsdr_dataterm)

if(HH_BUILD_TESTS)
  foreach(t test_dataterm_peers test_dataterm_coap test_dataterm_wire)
    add_executable(${t} tests/${t}.c)
    target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/tests)
    target_include_directories(${t} SYSTEM PRIVATE ${COAP_STATIC_INCLUDE_DIRS})
    target_link_libraries(${t} PRIVATE hhsdr_dataterm)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
endif()
