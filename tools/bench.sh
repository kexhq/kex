#!/usr/bin/env bash
# Repeat-and-median benchmark harness for the kex toolchain — the
# `make bench` entry point of docs/compiler-refactor-plan.md's measurement
# methodology. Informational only: it reports numbers, it does not gate.
#
#   tools/bench.sh [scenario ...]      # default: all, below
#
# Scenarios (compile = --run of a trivial program, so program cost ~ 0):
#   compile-warm      kex-owned caches present (steady state)
#   compile-cold-kex  every kex cache wiped before each run. The OS page
#                     cache is NOT dropped — this is cold-kex, not cold.
#   compile-dev-loop  only the prelude-interface cache wiped before each
#                     run: the state a developer pays after every prelude
#                     or compiler change.
#   fib               fib(BENCH_FIB_N) through --run
#   loop              BENCH_LOOP_N while-loop acc+1 iterations through --run
#   rebuild           touch $BENCH_TOUCH, time an incremental build (Phase 1
#                     closing criteria). NOT in the default set: run it
#                     explicitly, it rebuilds the tree per run.
#   clean-build       one full configure+build in a scratch dir (slow).
#                     NOT in the default set: run it explicitly.
#
# Environment:
#   BENCH_RUNS    runs per scenario (default 10; methodology wants >= 10)
#   BENCH_FIB_N   fib argument (default 35)
#   BENCH_LOOP_N  loop iterations (default 100000000)
#   KEX           kex binary (default build/kex)
#   KEX_BUILD_DIR cmake build dir for rebuild scenarios (default build)
#   BENCH_JOBS    build parallelism (default: ncpu)
#   BENCH_TOUCH   file to touch for the rebuild scenario
#                 (default src/ir/lowering_expr.cxx: one lowering TU, per
#                 the Phase 1 closing criteria; pass BENCH_TOUCH to measure
#                 the shared lowering.hxx instead)
#   BENCH_REBUILD_RUNS  runs for the rebuild scenario (default 3)
#
# Exits 0 unless a REQUESTED scenario produced zero successful runs — a
# broken harness or a broken kex, not a perf result. All caches are
# redirected into a scratch dir via XDG_CACHE_HOME: the real ~/.cache/kex
# is never touched (and KEX_CACHE cannot be used for this — the prelude
# interface cache reads XDG_CACHE_HOME/HOME directly and ignores it).
# Every timed kex run must exit 0 AND print its scenario's expected
# marker; a fast failure is a failure, not a data point.
#
# Output: median + IQR (nearest-rank) + min/max per scenario, the commit
# measured at, the machine and build flags, and one KEX_TIMINGS=1 phase
# dump of a warm compile.
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel)"
KEX="${KEX:-$ROOT/build/kex}"
RUNS="${BENCH_RUNS:-10}"
FIB_N="${BENCH_FIB_N:-35}"
LOOP_N="${BENCH_LOOP_N:-100000000}"
BUILD_DIR="${KEX_BUILD_DIR:-$ROOT/build}"
if [ -n "${BENCH_JOBS:-}" ]; then
  JOBS="$BENCH_JOBS"
elif command -v nproc >/dev/null 2>&1; then
  JOBS="$(nproc)"
else
  JOBS="$(sysctl -n hw.ncpu)"
fi
TOUCH_FILE="${BENCH_TOUCH:-src/ir/lowering_expr.cxx}"
REBUILD_RUNS="${BENCH_REBUILD_RUNS:-3}"

if [ ! -x "$KEX" ]; then
  echo "bench: no executable kex at $KEX — run: make build" >&2
  exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/kex-bench.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$WORK/src"

# bash's `time` builtin gives wall seconds at millisecond precision
# (TIMEFORMAT %3R) without spawning a timer process per run. A run only
# counts when kex exits 0 — `time` prints a number even for a crash, so
# the exit status (not the number's shape) is the success signal. Stdout
# goes to $WORK/run.out for the caller's marker check.
timed_run() { # <cache-dir> <kex-args...>
  local cache="$1"
  shift
  local out rc
  out="$({ TIMEFORMAT=%3R; time XDG_CACHE_HOME="$cache" "$KEX" "$@" \
           >"$WORK/run.out" 2>/dev/null; } 2>&1)"
  rc=$?
  case "$out.$rc" in
    *.*.0) printf '%s\n' "$out" ;;
    *)     echo "failed" ;;
  esac
}

# ── Sources ─────────────────────────────────────────────────────────────────
cat > "$WORK/src/trivial.kex" <<'EOF'
main do
  IO.printLine("ok")
end
EOF
cat > "$WORK/src/fib.kex" <<EOF
let fib(n: Int) -> Int do
  return n if n < 2

  fib(n - 1) + fib(n - 2)
end

main do
  let n = $FIB_N
  IO.printLine("fib(\${n}) = \${fib(n)}")
end
EOF
cat > "$WORK/src/loop.kex" <<EOF
main do
  var acc = 0
  var i = $LOOP_N
  while i > 0 do
    acc = acc + 1
    i = i - 1
  end
  IO.printLine("acc = \${acc}")
end
EOF

# Prime a cache dir with one untimed run of <source>.
prime() { # <cache-dir> <source>
  XDG_CACHE_HOME="$1" "$KEX" --run "$2" >/dev/null 2>&1
}

# ── Scenarios ───────────────────────────────────────────────────────────────
declare -a RESULTS=()

# Nearest-rank median + quartiles over the collected values.
summarize() { # <name> <ok> <runs> <values...>
  local name="$1" ok="$2" runs="$3"
  shift 3
  if [ "$ok" -eq 0 ]; then
    RESULTS+=("$(printf '%-18s %8s %8s %8s %8s' "$name" \
      'FAILED' '-' '-' '-')")
    FAILED=1
    return
  fi
  local sorted line
  sorted="$(printf '%s\n' "$@" | sort -n)"
  S=()
  while IFS= read -r line; do S+=("$line"); done <<<"$sorted"
  local n=${#S[@]}
  local mid=$(( (n - 1) / 2 )) q1=$(( (n - 1) / 4 )) q3=$(( 3 * (n - 1) / 4 ))
  local median
  if [ $((n % 2)) -eq 0 ]; then
    median="$(awk -v a="${S[$mid]}" -v b="${S[$((mid + 1))]}" \
      'BEGIN { printf "%.3f", (a + b) / 2 }')"
  else
    median="${S[$mid]}"
  fi
  local iqr
  iqr="$(awk -v a="${S[$q1]}" -v b="${S[$q3]}" \
    'BEGIN { printf "%.3f", b - a }')"
  RESULTS+=("$(printf '%-18s %8s %8s %8s %8s  %d/%d ok' "$name" \
    "$median" "$iqr" "${S[0]}" "${S[$((n - 1))]}" "$ok" "$runs")")
}

run_scenario() { # <name> <cache-dir> <mode> <source> <marker>
  local name="$1" cache="$2" mode="$3" src="$4" marker="$5"
  prime "$cache" "$WORK/src/trivial.kex"
  prime "$cache" "$src"
  prime "$cache" "$src"
  local values=() v ok=0
  for _ in $(seq "$RUNS"); do
    case "$mode" in
      cold-kex) rm -rf "$cache/kex" ;;
      dev-loop) rm -rf "$cache/kex/prelude-interface" ;;
    esac
    v="$(timed_run "$cache" --run "$src")"
    if [ "$v" != "failed" ] && grep -q -- "$marker" "$WORK/run.out"; then
      values+=("$v")
      ok=$((ok + 1))
    fi
  done
  summarize "$name" "$ok" "$RUNS" "${values[@]:-}"
}

# C++ rebuild scenarios (Phase 1 closing criteria).
REBUILT=0
run_rebuild() {
  REBUILT=1
  if [ ! -f "$ROOT/$TOUCH_FILE" ]; then
    echo "bench: rebuild touch target missing: $ROOT/$TOUCH_FILE" >&2
    summarize "rebuild" 0 "$REBUILD_RUNS"
    return
  fi
  local values=() v rc ok=0
  for _ in $(seq "$REBUILD_RUNS"); do
    touch "$ROOT/$TOUCH_FILE"
    v="$({ TIMEFORMAT=%3R; time cmake --build "$BUILD_DIR" \
           --parallel "$JOBS" >"$WORK/rebuild.log" 2>&1; } 2>&1)"
    rc=$?
    case "$v.$rc" in
      *.*.0) values+=("$v"); ok=$((ok + 1)) ;;
      *)     echo "bench: rebuild failed — tail of build log:" >&2
             tail -20 "$WORK/rebuild.log" >&2 ;;
    esac
  done
  summarize "rebuild" "$ok" "$REBUILD_RUNS" "${values[@]:-}"
}

run_clean_build() {
  local dir="$WORK/cleanbuild" gen v rc
  gen="$(grep '^CMAKE_GENERATOR:' "$BUILD_DIR/CMakeCache.txt" 2>/dev/null \
    | cut -d= -f2-)"
  [ -n "$gen" ] || gen="Unix Makefiles"
  v="$({ TIMEFORMAT=%3R; time (
    cmake -S "$ROOT" -B "$dir" -G "$gen" >"$WORK/cleanbuild.log" 2>&1 &&
    cmake --build "$dir" --parallel "$JOBS" >>"$WORK/cleanbuild.log" 2>&1
  ); } 2>&1)"
  rc=$?
  case "$v.$rc" in
    *.*.0) summarize "clean-build" 1 1 "$v" ;;
    *)     echo "bench: clean-build failed — tail of log:" >&2
           tail -20 "$WORK/cleanbuild.log" >&2
           summarize "clean-build" 0 1 ;;
  esac
}

WARM="$WORK/xdg-warm"
COLD="$WORK/xdg-cold"
DEV="$WORK/xdg-dev"
FAILED=0

if [ $# -eq 0 ]; then
  set -- compile-warm compile-cold-kex compile-dev-loop fib loop
fi

for scenario in "$@"; do
  case "$scenario" in
    compile-warm)     run_scenario "$scenario" "$WARM" warm      "$WORK/src/trivial.kex" "^ok$" ;;
    compile-cold-kex) run_scenario "$scenario" "$COLD" cold-kex  "$WORK/src/trivial.kex" "^ok$" ;;
    compile-dev-loop) run_scenario "$scenario" "$DEV"  dev-loop  "$WORK/src/trivial.kex" "^ok$" ;;
    fib)              run_scenario "$scenario" "$WARM" warm      "$WORK/src/fib.kex" "^fib(" ;;
    loop)             run_scenario "$scenario" "$WARM" warm      "$WORK/src/loop.kex" "^acc = " ;;
    rebuild)          run_rebuild ;;
    clean-build)      run_clean_build ;;
    *)
      echo "bench: unknown scenario '$scenario'" >&2
      exit 1
      ;;
  esac
done

# ── Report ──────────────────────────────────────────────────────────────────
commit="$(git -C "$ROOT" rev-parse --short HEAD)"
git -C "$ROOT" --quiet diff --cached --quiet 2>/dev/null || \
  { [ -n "$(git -C "$ROOT" status --porcelain)" ] && commit="$commit+dirty"; }
machine="$(uname -srm 2>/dev/null || uname -sm)"
buildtype="$(grep '^CMAKE_BUILD_TYPE:STRING=' "$BUILD_DIR/CMakeCache.txt" \
  2>/dev/null | cut -d= -f2-)"
if [ -z "$buildtype" ]; then
  cxxflags="$(grep '^CMAKE_CXX_FLAGS:STRING=' "$BUILD_DIR/CMakeCache.txt" \
    2>/dev/null | cut -d= -f2-)"
  if [ -n "$cxxflags" ]; then
    buildtype="default [$cxxflags]"
  else
    buildtype="default (no -O flag)"
  fi
fi
echo "kex bench — $commit — $RUNS runs/scenario — seconds (bash time %3R)"
echo "binary: $KEX  fib N=$FIB_N  loop N=$LOOP_N"
echo "machine: $machine  build: $buildtype"
echo ""
printf '%-18s %8s %8s %8s %8s\n' scenario median IQR min max
for row in "${RESULTS[@]}"; do
  echo "$row" | sed 's/  [0-9]*\/[0-9]* ok$//'
done
echo ""
echo "KEX_TIMINGS phases, one warm compile (trivial.kex):"
if [ "${REBUILT:-0}" -eq 1 ]; then
  # A rebuild scenario minted a new binary mid-run, so the old warm entry
  # is (correctly) stale under the new fingerprint — re-prime for the dump.
  prime "$WARM" "$WORK/src/trivial.kex"
  prime "$WARM" "$WORK/src/trivial.kex"
fi
XDG_CACHE_HOME="$WARM" KEX_TIMINGS=1 "$KEX" --run "$WORK/src/trivial.kex" \
  >/dev/null 2>"$WORK/timings"
sed 's/^/  /' "$WORK/timings"
miss="$(grep -o '\[cache\] miss: [a-z-]*' "$WORK/timings" | head -1)"
if [ -n "$miss" ]; then
  echo "cache: MISS ($miss — a warm compile should hit)"
elif grep -q '\[cache\] decode' "$WORK/timings"; then
  echo "cache: HIT"
else
  echo "cache: unknown (no [cache] line under KEX_TIMINGS)"
fi

[ "${FAILED:-0}" -eq 0 ]
