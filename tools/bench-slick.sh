#!/usr/bin/env bash
# Benchmarks kex on Slick — a real application (~135 .kex files, ~9.5k
# lines, harmat/metro/shakespeare dependencies through tey) — instead of a
# microbenchmark. `make bench-slick` is the entry point; tools/bench.sh keeps
# the fib/loop micro numbers.
#
#   tools/bench-slick.sh [scenario ...]   # default: cold noop incremental devloop specs
#
# Scenarios (each timed through `tey`, so it is what a developer pays):
#   cold         `tey build` with every kex + tey build cache and ebin/ wiped
#                (package sources stay: no network inside a timing)
#   noop         `tey build` again with nothing changed
#   incremental  touch $BENCH_SLICK_TOUCH, then `tey build`
#   devloop      wipe only the prelude-interface cache, then `tey build`
#                (what every compiler or prelude change costs)
#   specs        `tey test spec/*.spec.kex` after a warm build (the
#                non-browser specs; every run must report 0 failed)
#
# Comparing compilers: BENCH_KEX="main=/path/to/kex branch=build/kex" runs
# every scenario for each, interleaved run by run so machine drift hits
# both sides alike. Each compiler gets its own copy of Slick and its own
# caches; your checkout and ~/.cache are never written.
#
# Environment:
#   SLICK_DIR          the Slick checkout (default: ../slick beside this repo)
#   BENCH_KEX          "label=path ..." (default: branch=build/kex)
#   BENCH_RUNS         runs per scenario (default 5; builds take seconds)
#   BENCH_COLD_RUNS    runs for `cold` (default 3)
#   BENCH_SLICK_TOUCH  file touched by `incremental` (default app/models/team.kex)
#   BENCH_ERL          erl for kex and tey (default: Homebrew's, else PATH's);
#                      Slick's ODBC port program is built for that OTP
#   BENCH_TEY_SEED     tey cache whose package sources are copied in
#                      (default ~/.cache/tey; needs `tey install` done once)
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel)"
# shellcheck source=tools/bench_lib.sh
source "$ROOT/tools/bench_lib.sh"

SLICK_DIR="${SLICK_DIR:-$ROOT/../slick}"
RUNS="${BENCH_RUNS:-5}"
COLD_RUNS="${BENCH_COLD_RUNS:-3}"
TOUCH="${BENCH_SLICK_TOUCH:-app/models/team.kex}"
SEED="${BENCH_TEY_SEED:-$HOME/.cache/tey}"
TEY="$(command -v tey || true)"

if [ -z "${BENCH_ERL:-}" ]; then
  if [ -x /opt/homebrew/bin/erl ]; then BENCH_ERL=/opt/homebrew/bin/erl
  else BENCH_ERL="$(command -v erl || true)"; fi
fi

die() { echo "bench-slick: $*" >&2; exit 1; }
[ -f "$SLICK_DIR/package.kex" ] || die "no Slick checkout at $SLICK_DIR (set SLICK_DIR)"
[ -n "$TEY" ] || die "tey is not on PATH"
[ -x "$BENCH_ERL" ] || die "no erl (set BENCH_ERL)"
[ -d "$SEED/src" ] || die "no tey package sources in $SEED (run \`tey install\` in Slick once)"

read -r -a SPECS <<<"${BENCH_KEX:-branch=$ROOT/build/kex}"
LABELS=()
for spec in "${SPECS[@]}"; do
  label="${spec%%=*}"
  kex="${spec#*=}"
  case "$kex" in /*) ;; *) kex="$ROOT/$kex" ;; esac
  [ -x "$kex" ] || die "no executable kex for '$label' at $kex"
  LABELS+=("$label")
  printf -v "KEX_$label" '%s' "$kex"
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/kex-bench-slick.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# One private Slick copy + cache tree per compiler. Only the top-level ebin/
# is skipped: db/odbc/*/ebin is the ODBC port program the specs load.
for label in "${LABELS[@]}"; do
  mkdir -p "$WORK/$label/cache/tey"
  rsync -a --exclude /.git --exclude /ebin --exclude '/storage*' \
    --exclude /erl_crash.dump "$SLICK_DIR/" "$WORK/$label/slick/"
  for part in src downloads snapshots; do
    [ -d "$SEED/$part" ] && cp -R "$SEED/$part" "$WORK/$label/cache/tey/"
  done
done

# Runs tey for <label> with that compiler, its caches and the chosen erl.
in_slick() { # <label> <tey args...>
  local label="$1" kexvar="KEX_$1"
  shift
  (cd "$WORK/$label/slick" &&
    env PATH="$(dirname "$BENCH_ERL"):$PATH" KEX_ERL="$BENCH_ERL" \
      TEY_ERL="$BENCH_ERL" TEY_KEX="${!kexvar}" \
      KEX_CACHE="$WORK/$label/cache/kex" XDG_CACHE_HOME="$WORK/$label/cache" \
      TEY_CACHE="$WORK/$label/cache/tey" "$TEY" "$@")
}

wipe_builds() { # <label>
  rm -rf "$WORK/$1/cache/kex" "$WORK/$1/cache/tey/build" "$WORK/$1/slick/ebin"
}

# Every spec file's summary line says "N passed, 0 failed".
specs_green() { # <log>
  grep -E '[0-9]+ passed, [0-9]+ failed' "$1" >/dev/null &&
    ! grep -E '[0-9]+ passed, [1-9][0-9]* failed' "$1" >/dev/null
}

run_scenario() { # <scenario> <runs>
  local scenario="$1" runs="$2" label i v log
  for label in "${LABELS[@]}"; do
    eval "VALUES_$label=()"
    eval "OK_$label=0"
    in_slick "$label" build >/dev/null 2>&1   # warm, untimed
  done
  for i in $(seq "$runs"); do
    for label in "${LABELS[@]}"; do
      log="$WORK/$label/run.log"
      case "$scenario" in
        cold)        wipe_builds "$label" ;;
        incremental) touch "$WORK/$label/slick/$TOUCH" ;;
        devloop)     rm -rf "$WORK/$label/cache/kex/prelude-interface" ;;
      esac
      if [ "$scenario" = specs ]; then
        v="$(time_command "$log" in_slick "$label" test \
          $(cd "$WORK/$label/slick" && ls spec/*.spec.kex))"
        specs_green "$log" || v=failed
      else
        v="$(time_command "$log" in_slick "$label" build)"
      fi
      if [ "$v" != failed ]; then
        eval "VALUES_$label+=(\"\$v\")"
        eval "OK_$label=\$((OK_$label + 1))"
      else
        echo "bench-slick: $label $scenario run $i failed — tail of log:" >&2
        tail -5 "$log" >&2
      fi
    done
  done
  for label in "${LABELS[@]}"; do
    local -n values="VALUES_$label"
    local okvar="OK_$label"
    summarize "$scenario [$label]" "${!okvar}" "$runs" "${values[@]:-}"
    unset -n values
  done
}

# The phase marks tey prints on a no-op build (expansion / check / lower /
# erlc cache), as seconds into the build.
phase_breakdown() { # <label>
  in_slick "$1" build >"$WORK/$1/phases.log" 2>&1
  grep -E 'Resolving|Checking|Lowering|Cached|Compiled [0-9]+/[0-9]+ modules$|Finished' \
    "$WORK/$1/phases.log" | grep -vE 'Compiled [0-9]+/' | sed 's/^/  /'
}

if [ $# -eq 0 ]; then
  set -- cold noop incremental devloop specs
fi
for scenario in "$@"; do
  case "$scenario" in
    cold)                              run_scenario cold "$COLD_RUNS" ;;
    noop | incremental | devloop | specs) run_scenario "$scenario" "$RUNS" ;;
    *) die "unknown scenario '$scenario'" ;;
  esac
done

echo "kex bench-slick — kex $(commit_of "$ROOT"), slick $(commit_of "$SLICK_DIR") — seconds (bash time %3R)"
for label in "${LABELS[@]}"; do
  kexvar="KEX_$label"
  echo "  $label: ${!kexvar}"
done
echo "machine: $(machine_of)  erl: $BENCH_ERL  runs: $RUNS (cold $COLD_RUNS)"
echo ""
print_results
echo ""
for label in "${LABELS[@]}"; do
  echo "no-op build phases [$label]:"
  phase_breakdown "$label"
done

[ "$FAILED" -eq 0 ]
