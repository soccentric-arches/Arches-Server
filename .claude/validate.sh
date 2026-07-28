#!/usr/bin/env bash
# Arches-Server — per-repo validation gate.  See .claude/lib/validate-lib.sh.
#
#   quick   seconds. Repo-shape, migrations, static analysis.
#   full    minutes. Build + the label-filtered CTest suite.
#
# Server runs on an x86 host, not the Jetson. It links ../Arches-HAL for the
# shared generated protos — so a sibling-repo check comes before anything else.
set -uo pipefail
# shellcheck source=lib/validate-lib.sh
source "$(dirname "${BASH_SOURCE[0]}")/lib/validate-lib.sh"

TIER="$(tier_or_die "${1:-quick}")"

# --- quick -----------------------------------------------------------------

# Without the sibling checkout, CMake fails deep in configure with an error that
# reads like a code problem. Catch it here, where the message is honest.
assert "sibling Arches-HAL checkout present" \
  'test -d ../Arches-HAL/protos || { echo "../Arches-HAL missing — add_subdirectory will fail"; exit 1; }'

assert "server never regenerates HAL protos" \
  'hits=$(cat CMakeLists.txt $(find src -type f 2>/dev/null) 2>/dev/null \
            | strip_comments \
            | grep -nE "protobuf_generate|grpc_generate|protoc ")
   [ -z "$hits" ] || { echo "regenerating protos here causes ODR/duplicate-symbol link errors:"; echo "$hits"; exit 1; }'

assert "migrations are ordered and unique" \
  'ls migrations/*.sql >/dev/null 2>&1 || { echo "no migrations found"; exit 1; }
   dupes=$(ls migrations/*.sql | xargs -n1 basename | cut -d_ -f1 | sort | uniq -d)
   [ -z "$dupes" ] || { echo "duplicate migration prefixes: $dupes"; exit 1; }'

check "cmake configure parses" "cmake ninja" \
  'cmake -GNinja -DBUILD_TESTS=ON -S . -B build/native >/dev/null'

# `make lint` here chains cppcheck (wrapped in `|| true`) then clang-tidy, and
# both print "not installed; skipping" and exit 0 when absent. That is a green
# light that analysed nothing. Require the tools rather than trust the exit code.
check "cppcheck static analysis" "cppcheck cmake ninja" 'make cppcheck'
check "clang-tidy" "clang-tidy cmake ninja" \
  'make lint 2>&1 | tee /dev/stderr | grep -qv "clang-tidy not installed"'

check "clang-format clean" "clang-format" \
  'find src -name "*.cc" -o -name "*.cpp" -o -name "*.h" 2>/dev/null \
     | xargs -r clang-format --dry-run --Werror'

[ "$TIER" = "quick" ] && { report "$TIER"; exit $?; }

# --- full ------------------------------------------------------------------

check "builds clean" "cmake ninja" 'make build'

# Never a bare ctest here: HAL's enable_testing() registers ~36 test executables
# that aren't built downstream, so an unfiltered run errors on missing binaries
# and looks like a code failure. `make test` applies -L arches_server.
check "ctest (label=arches_server)" "cmake ninja ctest" 'make test'

report "$TIER"
