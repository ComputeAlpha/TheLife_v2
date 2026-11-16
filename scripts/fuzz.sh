#!/usr/bin/env bash
# Fuzz the collector's parsers (collector_lld.md §8) with libFuzzer + ASan/UBSan.
#   scripts/fuzz.sh [SECONDS_PER_TARGET] [TARGET...]      default: 60 s, all targets
# New interesting inputs go to build/fuzz/corpus/<target>; crashes to build/fuzz/crashes/. Copy a crash input into
# collector/fuzz/corpus/<target>/ once fixed: the normal test run replays it from then on.
set -euo pipefail
cd "$(dirname "$0")/.."
secs="${1:-60}"; shift || true
targets=("$@"); [[ ${#targets[@]} -gt 0 ]] || targets=(server_config notifications local_config)
[[ -f build/fuzz/CMakeCache.txt ]] || VCPKG_ROOT="${VCPKG_ROOT:-$HOME/vcpkg}" cmake --preset fuzz \
  -DCMAKE_TOOLCHAIN_FILE="${VCPKG_ROOT:-$HOME/vcpkg}/scripts/buildsystems/vcpkg.cmake"
cmake --build build/fuzz -j"$(nproc)" --target "${targets[@]/#/collector_fuzz_}"
mkdir -p build/fuzz/crashes
for t in "${targets[@]}"; do
  mkdir -p "build/fuzz/corpus/$t"
  echo "== $t (${secs}s)"
  build/fuzz/collector/collector_fuzz_"$t" -max_total_time="$secs" -max_len=65536 -timeout=5 \
    -artifact_prefix="build/fuzz/crashes/$t-" "build/fuzz/corpus/$t" "collector/fuzz/corpus/$t"
done
