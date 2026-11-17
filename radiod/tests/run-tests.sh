#!/usr/bin/env bash
# Build and run the radiod tests (library, ring, daemon end-to-end).
#
# Usage: radiod/tests/run-tests.sh [--asan] [--keep-going]
#   --asan        use the sanitizer build tree (build-asan, -DHH_SANITIZE=ON)
#   --keep-going  do not stop at the first failing test
# Output of the build goes to <build>/radiod-tests.log; only a summary is shown.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
build="$root/build"
cmake_args=(-DCMAKE_BUILD_TYPE=Debug)
ctest_args=(--output-on-failure)

for a in "$@"; do
  case "$a" in
    --asan)       build="$root/build-asan"; cmake_args=(-DHH_SANITIZE=ON) ;;
    --keep-going) ;;
    *) echo "unknown option: $a" >&2; exit 2 ;;
  esac
done
[[ " $* " == *" --keep-going "* ]] || ctest_args+=(--stop-on-failure)

mkdir -p "$build"
log="$build/radiod-tests.log"
echo "configure + build (log: $log)"
{ cmake -S "$root" -B "$build" "${cmake_args[@]}" &&
  cmake --build "$build" -j"$(nproc)"; } >"$log" 2>&1 ||
  { tail -n 30 "$log"; echo "BUILD FAILED" >&2; exit 1; }

# test_mock_backend and test_ring do not have 'radiod' in their name.
ctest --test-dir "$build" -R 'radiod|mock_backend|ocpi_backend|^test_ring$' "${ctest_args[@]}"
