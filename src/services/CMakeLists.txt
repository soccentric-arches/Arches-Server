# ---------------------------------------------------------------------------
# src/services/CMakeLists.txt — Phase-1 service layer (RunService + LogService).
#
# Layering (breaks the services<->grpc cycle): arches_stream_registry
# (grpc_server/)  — board_id -> outbound queue, no dependency on services.
# arches_services         (here)          — RunService/LogService; depends on
# arches_repos + arches_stream_registry. arches_grpc             (grpc_server/)
# — ClientServiceImpl/HalServiceImpl/ GrpcServerRunner; depends on
# arches_services + arches_stream_registry. RunService.h includes
# grpc_server/ClientStreamRegistry.h (a reference member), so arches_services
# PUBLIC-links arches_stream_registry (and transitively the protos it carries).
# ---------------------------------------------------------------------------
add_library(arches_services STATIC RunService.cc LogService.cc)
target_include_directories(arches_services
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(arches_services PUBLIC arches_repos
                                             arches_stream_registry)
# RunService.cc uses util::generate_uuid / proto_to_result / Errors (PRIVATE —
# the headers don't expose util types).
target_link_libraries(arches_services PRIVATE arches_util spdlog::spdlog)
set_compiler_warnings(arches_services)
