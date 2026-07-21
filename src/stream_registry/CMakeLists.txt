# ---------------------------------------------------------------------------
# src/stream_registry/CMakeLists.txt — board_id -> outbound command queue.
#
# arches_stream_registry is the base layer of the services<->grpc stack: it has
# NO dependency on the service layer, so RunService (arches_services) can depend
# on it, and ClientServiceImpl (arches_grpc) can depend on both without a cycle.
#
# Layering:
#   arches_stream_registry  (here)   -> hal_protos_shared (ServerMessage) only
#   arches_services         (services/) -> arches_repos + arches_stream_registry
#   arches_grpc             (grpc_server/) -> arches_services + arches_stream_registry
#
# StreamBackpressure.h (BoundedQueue template) is header-only and lives in
# grpc_server/; it is reached via the src/ include root (grpc_server/StreamBackpressure.h).
# ---------------------------------------------------------------------------
add_library(arches_stream_registry STATIC
    ClientStreamRegistry.cc
)
target_include_directories(arches_stream_registry PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(arches_stream_registry PUBLIC
    hal_protos_shared
    spdlog::spdlog
)
set_compiler_warnings(arches_stream_registry)