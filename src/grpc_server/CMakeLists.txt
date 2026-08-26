# ---------------------------------------------------------------------------
# src/grpc_server/CMakeLists.txt — gRPC service implementations.
#
# arches_stream_registry (the board_id -> outbound queue base layer) lives in
# src/stream_registry/ and is added BEFORE services in src/CMakeLists.txt, so it
# is already available here. This lib (arches_grpc) is the top of the stack:
# arches_grpc -> arches_services + arches_stream_registry + arches_registry +
# protos
#
# StreamBackpressure.h (BoundedQueue template) is header-only and lives here;
# ClientServiceImpl uses BoundedQueue<IngestWork> + OutQueue (shared_ptr<
# BoundedQueue<ServerMessage>>). The stream_registry lib uses the same template
# via the src/ include root.
# ---------------------------------------------------------------------------
add_library(arches_grpc STATIC ClientServiceImpl.cc HalServiceImpl.cc
                               GrpcServerRunner.cc)
target_include_directories(arches_grpc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(
  arches_grpc PUBLIC arches_services arches_stream_registry arches_registry
                     hal_protos_shared spdlog::spdlog)
# .cc files use util::unpack_module_results / utc_now_iso8601 / generate_uuid.
target_link_libraries(arches_grpc PRIVATE arches_util)
set_compiler_warnings(arches_grpc)
