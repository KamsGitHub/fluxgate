find_package(GTest REQUIRED CONFIG)
include(GoogleTest)

add_executable(fluxgate_unit_tests
  unit/byte_buffer_test.cpp
  unit/config_test.cpp
  unit/file_descriptor_test.cpp
  unit/log_test.cpp
  unit/parser_test.cpp
  unit/policy_test.cpp
  unit/session_test.cpp
  unit/tcp_stream_test.cpp
)
target_link_libraries(fluxgate_unit_tests PRIVATE
  fluxgate_core
  GTest::gtest
  GTest::gtest_main
)
gtest_discover_tests(fluxgate_unit_tests)

# Black-box tests drive the real binary over real sockets.
find_program(FLUXGATE_PYTEST pytest HINTS "${CMAKE_SOURCE_DIR}/.venv/bin")
if(FLUXGATE_PYTEST)
  add_test(
    NAME integration
    COMMAND "${FLUXGATE_PYTEST}" -q "${CMAKE_CURRENT_SOURCE_DIR}/integration"
  )
  set_tests_properties(integration PROPERTIES
    ENVIRONMENT "FLUXGATE_BIN=$<TARGET_FILE:fluxgate>"
    TIMEOUT 300
  )
else()
  message(WARNING "pytest not found: integration tests will not be registered")
endif()
