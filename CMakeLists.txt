cmake_minimum_required(VERSION 3.25)
project(FluxGate VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FLUXGATE_BUILD_TESTS "Build unit tests and register integration tests" ON)
option(FLUXGATE_WERROR "Treat compiler warnings as errors" ON)
set(FLUXGATE_SANITIZE "" CACHE STRING "Sanitizers to enable, e.g. address,undefined or thread")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)

find_package(llhttp REQUIRED CONFIG)
find_package(spdlog REQUIRED CONFIG)
find_package(yaml-cpp REQUIRED CONFIG)

add_library(fluxgate_core STATIC
  src/config/config.cpp
  src/core/byte_buffer.cpp
  src/http/message.cpp
  src/http/parser.cpp
  src/http/policy.cpp
  src/log/log.cpp
  src/net/address.cpp
  src/net/file_descriptor.cpp
  src/net/socket.cpp
  src/net/tcp_stream.cpp
  src/proxy/outbound_channel.cpp
  src/proxy/server.cpp
  src/proxy/session.cpp
)
target_include_directories(fluxgate_core PUBLIC include)
target_compile_definitions(fluxgate_core PUBLIC FLUXGATE_VERSION="${PROJECT_VERSION}")
target_link_libraries(fluxgate_core
  PUBLIC
    llhttp::llhttp
    fluxgate_compile_options
  PRIVATE
    spdlog::spdlog
    yaml-cpp::yaml-cpp
)

add_executable(fluxgate src/main.cpp)
target_link_libraries(fluxgate PRIVATE fluxgate_core)

if(FLUXGATE_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
