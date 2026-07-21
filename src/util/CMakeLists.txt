# ---------------------------------------------------------------------------
# src/util/CMakeLists.txt — portable utility helpers.
# Time, Uuid (compiled); Errors (header-only); ResultConvert (Step 4).
# ResultConvert pulls in the proto types + reflection (hal_protos_shared) and
# the Result struct (header-only from repositories/ResultRepository.h — no link
# dep on arches_repos, avoiding a circular dependency: arches_repos -> arches_util
# for Time/Uuid, arches_util -> hal_protos_shared only).
# ---------------------------------------------------------------------------

add_library(arches_util STATIC
    Time.cc
    Uuid.cc
    ResultConvert.cc
)

target_include_directories(arches_util PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# proto types (hal::common::TestResult) + DescriptorPool/DynamicMessageFactory
# for the §5 reflection-based unpack.
target_link_libraries(arches_util PUBLIC
    hal_protos_shared
)

set_compiler_warnings(arches_util)