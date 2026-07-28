# ---------------------------------------------------------------------------
# src/http/CMakeLists.txt — minimal HTTP/SSE server (arches_http).
#
# NEW for the live-run-monitoring loop. A sibling lib to the three-layer
# arches_stream_registry -> arches_services -> arches_grpc stack; it does NOT
# depend on arches_grpc, keeping it out of that link graph.
#
# Depends on:
#   arches_services        — RunService for run state (get_run, list_results,
#                            count_results, get_module_summaries)
#   arches_stream_registry — RunStreamRegistry / BoundedEventQueue (viewer fan-out)
#   arches_util            — util::ResultConvert JSON serializers
#   arches_config          — config::Settings (http_listen)
#
# Vendored dependency: cpp-httplib (httplib.h, single-header, MIT). No SSL
# (CPPHTTPLIB_OPENSSL_SUPPORT is intentionally NOT defined), no JsonCpp, no
# OpenSSL — per the repo "Don't" list. The header is fully inline (header-only
# with inline definitions); it is #included only by HttpServer.cc, so its
# definitions live in exactly one object file.
# ---------------------------------------------------------------------------
add_library(arches_http STATIC
    HttpServer.cc
)
target_include_directories(arches_http PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)
target_link_libraries(arches_http PUBLIC
    arches_services
    arches_stream_registry
    arches_util
    arches_config
    spdlog::spdlog
    Threads::Threads
)
set_compiler_warnings(arches_http)