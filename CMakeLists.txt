cmake_minimum_required(VERSION 3.16)
project(
  arches-server
  VERSION 0.1.0
  LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Export compile_commands.json so clang-tidy / cppcheck can run from the
# Makefile `lint` target without a CMake-side custom target.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CompilerWarnings.cmake)
include(GNUInstallDirs)

# ---------------------------------------------------------------------------
# HAL library (sibling directory) — must come before add_subdirectory(src) so
# the HAL targets are visible to subsystem CMakeLists that link them.
#
# Linking strategy (mirrors Arches-Client, validated by its build): -
# hal_protos_shared : SHARED generated protobuf + gRPC contracts (common,
# hal_service, client_service, every module .proto). Carries protobuf + gRPC
# transitively (PUBLIC), so the server gets the proto message types, grpc++
# runtime, and protobuf includes WITHOUT generating any proto sources locally.
# Linking it also registers every hal.modules.<id>.Result descriptor in the
# generated DescriptorPool — which the §5 reflection-based module-Result unpack
# relies on. Single source of generated proto symbols → no ODR/duplicate-symbol
# risk.
#
# Unlike the prior reference server (which self-generates 3 protos via protoc),
# Arches-Server consumes the HAL subproject's protos, exactly like Arches-Client
# does. No find_package(gRPC/Protobuf) here, no local protoc invocation.
# ---------------------------------------------------------------------------
find_package(Threads REQUIRED) # HAL requires Threads::Threads

add_subdirectory(${CMAKE_SOURCE_DIR}/../Arches-HAL
                 ${CMAKE_BINARY_DIR}/arches_hal_build EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Optional sanitizers (ASan + UBSan). Applied as DIRECTORY-level flags here —
# AFTER the HAL add_subdirectory (so the HAL subproject is NOT instrumented; it
# ships pre-built and instrumenting it conficts with its -Werror + external
# deps) but BEFORE src/ and tests/ are added, so every in-repo target (the
# arches_* static libs, the arches-server exe, and the test exes that actually
# reproduce the bug) is instrumented. The earlier target-specific form only
# flagged `arches-server` — the test binary was uninstrumented, so ASan could
# not see the intermittent heap corruption in the bidi-stream interop test.
# ---------------------------------------------------------------------------
option(ENABLE_SANITIZERS "Enable -fsanitize=address,undefined" OFF)
if(ENABLE_SANITIZERS)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=address,undefined)
endif()

# ---------------------------------------------------------------------------
# Host dependencies. The HAL subproject finds protobuf + gRPC itself; we do not
# regenerate protos here, so protoc / grpc_cpp_plugin are NOT required on the
# host.
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

# SQLite3 (master §4: *-Server is gRPC + SQLite; deviation from the prior
# reference server's PostgreSQL, flagged in the porting note).
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
  include(GoogleTest) # gtest_discover_tests + LABELS for `ctest -L
                      # arches_server`
  enable_testing()
  add_subdirectory(tests)
endif()

# ---------------------------------------------------------------------------
# Generated version header (parity with the prior reference server; the
# arches-server target also gets ARCHES_SERVER_VERSION as a compile definition —
# this header is for TUs that prefer to #include it).
# ---------------------------------------------------------------------------
set(GENERATED_DIR "${PROJECT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GENERATED_DIR}")
configure_file("${CMAKE_SOURCE_DIR}/cmake/version.hpp.in"
               "${GENERATED_DIR}/version.hpp" @ONLY)

# ---------------------------------------------------------------------------
# Doxygen docs target (§12 mandatory for *-Server; the prior reference server
# shipped a Doxyfile but never wired the target — Arches-Server wires it).
# Builds the `doc_doxygen` target that the Makefile `doc` target drives via
# `ninja doc_doxygen`. Best-effort: if Doxygen is not installed, configure
# succeeds but the target is absent (the Makefile reports this).
# ---------------------------------------------------------------------------
find_package(Doxygen QUIET)
if(DOXYGEN_FOUND)
  set(DOXYFILE_IN "${CMAKE_SOURCE_DIR}/Doxyfile.in")
  set(DOXYFILE_OUT "${PROJECT_BINARY_DIR}/Doxyfile")
  configure_file("${DOXYFILE_IN}" "${DOXYFILE_OUT}" @ONLY)
  add_custom_target(
    arches_server_doc_doxygen
    COMMAND ${DOXYGEN_EXECUTABLE} "${DOXYFILE_OUT}"
    WORKING_DIRECTORY "${PROJECT_BINARY_DIR}"
    VERBATIM
    COMMENT "Generating Doxygen HTML docs for Arches-Server")
else()
  message(
    STATUS
      "Doxygen not found — `ninja doc_doxygen` / `make doc` will skip (install doxygen to enable)."
  )
endif()

# ---------------------------------------------------------------------------
# clang-format target (drives the Makefile `format` target via `ninja format`).
# Best-effort: if clang-format is not installed, the target prints a notice.
# ---------------------------------------------------------------------------
find_program(CLANG_FORMAT_EXE NAMES clang-format clang-format-15
                                    clang-format-14)
if(CLANG_FORMAT_EXE)
  file(GLOB_RECURSE ARCHES_FORMAT_SOURCES CONFIGURE_DEPENDS
       "${CMAKE_SOURCE_DIR}/src/*.cc" "${CMAKE_SOURCE_DIR}/src/*.h"
       "${CMAKE_SOURCE_DIR}/tests/*.cc")
  add_custom_target(
    arches_server_format
    COMMAND "${CLANG_FORMAT_EXE}" -i --style=file ${ARCHES_FORMAT_SOURCES}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}"
    VERBATIM
    COMMENT "Running clang-format on Arches-Server sources")
else()
  add_custom_target(
    arches_server_format
    COMMAND ${CMAKE_COMMAND} -E echo
            "clang-format not installed; skipping format."
    VERBATIM)
endif()

# ---------------------------------------------------------------------------
# CPack packaging (§12: .deb, .rpm, .ipk for Yocto/opkg). The prior reference
# server had no CPack wiring; Arches-Server adds it. .ipk is produced by the DEB
# generator with an opkg-compatible rename (Yocto consumes .ipk).
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME "arches-server")
set(CPACK_PACKAGE_VERSION "${PROJECT_VERSION}")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "Arches-Server — C++17 gRPC IV&V server (NVIDIA Jetson / Arches)")
set(CPACK_PACKAGE_CONTACT "SoCcentric IV&V")
if(EXISTS "${CMAKE_SOURCE_DIR}/LICENSE")
  set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
endif()
set(CPACK_STRIP_FILES ON)
set(CPACK_PACKAGE_FILE_NAME
    "${CPACK_PACKAGE_NAME}-${CPACK_PACKAGE_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")

# Debian / .deb (also the basis for the .ipk Yocto/opkg variant).
set(CPACK_DEBIAN_PACKAGE_MAINTAINER "SoCcentric IV&V")
set(CPACK_DEBIAN_PACKAGE_SECTION "devel")
set(CPACK_DEBIAN_PACKAGE_DEPENDS
    "libsqlite3-0, libspdlog1, libyaml-cpp0.7, libgrpc++1, libprotobuf23")

# RPM.
set(CPACK_RPM_PACKAGE_LICENSE "Proprietary")
set(CPACK_RPM_PACKAGE_GROUP "Development/Tools")
set(CPACK_RPM_PACKAGE_REQUIRES "sqlite, spdlog, yaml-cpp, grpc, protobuf")

include(CPack)

# ---------------------------------------------------------------------------
# Install
# ---------------------------------------------------------------------------
install(TARGETS arches-server RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# systemd unit for bench-host deployment (Task.md E-08). OPTIONAL so a source
# tree without it still installs, matching how Arches-Client installs its own.
install(FILES systemd/arches-server.service
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/systemd/system
  OPTIONAL
)
install(DIRECTORY "${CMAKE_SOURCE_DIR}/migrations/"
        DESTINATION "${CMAKE_INSTALL_DATAROOTDIR}/arches/migrations")
