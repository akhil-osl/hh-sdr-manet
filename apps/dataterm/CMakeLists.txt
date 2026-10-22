# ---------------------------------------------------------------------------
# dataterm : data terminal application (F02-AK-4). Text messages over UDP with
# ACK and retry. Links hhsdr_core_base only.
# ---------------------------------------------------------------------------
add_library(hhsdr_dataterm STATIC
  src/msg.c
  src/dataterm.c
  src/udp.c
)
target_include_directories(hhsdr_dataterm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(hhsdr_dataterm PUBLIC hhsdr_core_base)

add_executable(dataterm src/main.c)
target_link_libraries(dataterm PRIVATE hhsdr_dataterm)

if(HH_BUILD_TESTS)
  foreach(t test_dataterm_msg test_dataterm test_dataterm_udp)
    add_executable(${t} tests/${t}.c)
    target_include_directories(${t} PRIVATE ${CMAKE_SOURCE_DIR}/tests ${CMAKE_SOURCE_DIR}/tests/sim)
    target_link_libraries(${t} PRIVATE hhsdr_dataterm)
    add_test(NAME ${t} COMMAND ${t})
  endforeach()
endif()
