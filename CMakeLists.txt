cmake_minimum_required(VERSION 3.16)
project(arches-server VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json so clang-tidy / cppcheck can run from the
# Makefile `lint` target without a CMake-side custom target.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CompilerWarnings.cmake)
include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# HAL library (sibling directory) — must come before add_subdirectory(src)
# so the HAL targets are visible to subsystem CMakeLists that link them.
#
# Linking strategy (mirrors Arches-Client, validated by its build):
#   - hal_protos_shared : SHARED generated protobuf + gRPC contracts (common,
#                         hal_service, client_service, every module .proto).
#                         Carries protobuf + gRPC transitively (PUBLIC), so the
#                         server gets the proto message types, grpc++ runtime,
#                         and protobuf includes WITHOUT generating any proto
#                         sources locally. Linking it also registers every
#                         hal.modules.<id>.Result descriptor in the generated
#                         DescriptorPool — which the §5 reflection-based
#                         module-Result unpack relies on. Single source of
#                         generated proto symbols → no ODR/duplicate-symbol risk.
#
# Unlike Acadia-Server (which self-generates 3 protos via protoc), Arches-Server
# consumes the HAL subproject's protos, exactly like Arches-Client does. No
# find_package(gRPC/Protobuf) here, no local protoc invocation.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED)  # HAL requires Threads::Threads

add_subdirectory(
    ${CMAKE_SOURCE_DIR}/../Arches-HAL
    ${CMAKE_BINARY_DIR}/arches_hal_build
    EXCLUDE_FROM_ALL
)

# ---------------------------------------------------------------------------
# Host dependencies. The HAL subproject finds protobuf + gRPC itself; we do not
# regenerate protos here, so protoc / grpc_cpp_plugin are NOT required on the host.
# ---------------------------------------------------------------------------
find_package(spdlog REQUIRED)

# yaml-cpp (modules.yaml registry)
find_package(yaml-cpp REQUIRED)
# Older yaml-cpp doesn't export yaml-cpp::yaml-cpp target; create wrapper.
if(NOT TARGET yaml-cpp::yaml-cpp)
  add_library(yaml-cpp-wrapper INTERFACE)
  target_link_libraries(yaml-cpp-wrapper INTERFACE yaml-cpp)
  target_include_directories(yaml-cpp-wrapper INTERFACE ${YAML_CPP_INCLUDE_DIR})
  add_library(yaml-cpp::yaml-cpp ALIAS yaml-cpp-wrapper)
endif()

# SQLite3 (master §4: *-Server is gRPC + SQLite; deviation from Acadia-Server's
# PostgreSQL, flagged in the porting note).
find_package(SQLite3 REQUIRED)

# Position independent code for shared linking.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

set(MAINFOLDER ${PROJECT_BINARY_DIR})
set(EXECUTABLE_OUTPUT_PATH "${MAINFOLDER}/bin")
set(LIBRARY_OUTPUT_PATH "${MAINFOLDER}/lib")

# ---------------------------------------------------------------------------
# Sources
# ---------------------------------------------------------------------------
add_subdirectory(src)

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
option(BUILD_TESTS "Build unit and integration tests" ON)
if(BUILD_TESTS)
  find_package(GTest REQUIRED)
  include(GoogleTest)  # gtest_discover_tests + LABELS for `ctest -L arches_server`
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Optional sanitizers
# ---------------------------------------------------------------------------
option(ENABLE_SANITIZERS "Enable -fsanitize=address,undefined" OFF)
if(ENABLE_SANITIZERS)
  target_compile_options(arches-server PRIVATE -fsanitize=address,undefined)
  target_link_options(arches-server PRIVATE -fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS arches-server
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)