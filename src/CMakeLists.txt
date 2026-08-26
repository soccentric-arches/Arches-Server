# ---------------------------------------------------------------------------
# src/CMakeLists.txt Arches-Server sources. Subsystem static libs are added via
# their own CMakeLists.txt files under src/. Step 2 added the SQLite DB layer
# (arches_db); Step 3 adds portable util (arches_util) + SQLite-backed
# repositories (arches_repos). Later steps add services, grpc_server, registry,
# config.
# ---------------------------------------------------------------------------

add_subdirectory(util)
add_subdirectory(config)
add_subdirectory(db)
add_subdirectory(repositories)
add_subdirectory(registry)
# Order matters for the layered libs (no cycles): arches_stream_registry
# (stream_registry) -> protos only arches_services        (services)         ->
# arches_repos + arches_stream_registry arches_grpc            (grpc_server) ->
# arches_services + arches_stream_registry
add_subdirectory(stream_registry)
add_subdirectory(services)
add_subdirectory(grpc_server)

add_executable(arches-server main.cpp)

target_include_directories(
  arches-server PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
                        ${CMAKE_SOURCE_DIR}/../Arches-HAL/src)

# gRPC runtime + protobuf are carried transitively by hal_protos_shared. Step
# 5/6 add the gRPC + services libs; Step 7 (main.cpp wiring) will actually use
# them. Linked now so the libs are part of the build graph and any link-time
# issues surface early.
target_link_libraries(
  arches-server
  PRIVATE hal_protos_shared
          arches_grpc
          arches_services
          arches_stream_registry
          arches_registry
          arches_repos
          arches_util
          arches_db
          arches_config
          spdlog::spdlog
          yaml-cpp::yaml-cpp)

target_compile_definitions(arches-server
                           PRIVATE ARCHES_SERVER_VERSION="${PROJECT_VERSION}")

set_compiler_warnings(arches-server)
