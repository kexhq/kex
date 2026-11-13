#!/usr/bin/env bash
# Run the metamorphic search with LLVM source coverage for the compiler.
set -euo pipefail

build_dir=${METAMORPHIC_COVERAGE_BUILD:-build-metamorphic-coverage}
out_dir=${METAMORPHIC_COVERAGE_OUT:-/tmp/kex-metamorphic-coverage}
seed=${METAMORPHIC_SEED:-}
cases=${METAMORPHIC_CASES:-30}
jobs=${JOBS:-2}

cmake -B "$build_dir" -G 'Unix Makefiles' \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_BUILD_TYPE=Debug \
  '-DCMAKE_CXX_FLAGS=-fprofile-instr-generate -fcoverage-mapping' \
  '-DCMAKE_EXE_LINKER_FLAGS=-fprofile-instr-generate'
cmake --build "$build_dir" --target kex_prelude_beam --parallel "$jobs"
mkdir -p "$out_dir"
case_dir=$(mktemp -d "$out_dir/cases.XXXXXX")

set +e
LLVM_PROFILE_FILE="$out_dir/profile-%p.profraw" "$build_dir/kex" \
  --run-walker --no-colors -- tools/metamorphic.kex \
  "$seed" "$cases" "$build_dir/kex" "$case_dir" 15000 search
search_status=$?
set -e

# Re-run each saved side separately. The two TOTAL rows show whether the
# transformations reach compiler code that the original programs did not.
original_profiles=$(mktemp -d "$out_dir/original-profiles.XXXXXX")
changed_profiles=$(mktemp -d "$out_dir/changed-profiles.XXXXXX")
for side in before after; do
  if [ "$side" = before ]; then profile_dir=$original_profiles; else profile_dir=$changed_profiles; fi
  for source in "$case_dir"/case_*/"$side".kex; do
    [ -f "$source" ] || continue
    case_root=${source%/*}
    LLVM_PROFILE_FILE="$profile_dir/profile-%p.profraw" "$build_dir/kex" \
      --run --no-colors --source-root "$case_root" "$source" \
      > /dev/null 2>&1 || true
  done
  llvm-profdata merge -sparse "$profile_dir"/*.profraw -o "$profile_dir/merged.profdata"
  llvm-cov report "$build_dir/kex" \
    -instr-profile="$profile_dir/merged.profdata" \
    -ignore-filename-regex='third_party|build-metamorphic-coverage' \
    > "$out_dir/$side-coverage.txt"
done
printf 'Original compiler coverage: '
grep '^TOTAL' "$out_dir/before-coverage.txt"
printf 'Transformed compiler coverage: '
grep '^TOTAL' "$out_dir/after-coverage.txt"
exit "$search_status"
