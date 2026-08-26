# ---------------------------------------------------------------------------
# src/registry/CMakeLists.txt — HAL module registry (modules.yaml loader).
# Ported from the prior reference implementation (YAML, no DB). Static lib used
# by HalServiceImpl for module validation + ListModules. Carries result_type_url
# per module — the §5 dispatch key.
# ---------------------------------------------------------------------------
add_library(arches_registry STATIC ModuleRegistry.cc)
target_include_directories(arches_registry
                           PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(arches_registry PUBLIC yaml-cpp::yaml-cpp spdlog::spdlog)
set_compiler_warnings(arches_registry)
