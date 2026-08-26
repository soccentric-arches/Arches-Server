# ---------------------------------------------------------------------------
# src/config/CMakeLists.txt — env-var Settings loader. Header-only struct + one
# .cc for load_settings(). Used by main.cpp (Step 7) and
# HalServiceImpl/GrpcServerRunner (Step 6, header-only ref).
# ---------------------------------------------------------------------------
add_library(arches_config STATIC Settings.cc)
target_include_directories(arches_config PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
set_compiler_warnings(arches_config)
