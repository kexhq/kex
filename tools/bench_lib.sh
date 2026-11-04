# Shared helpers for the benchmark harnesses (tools/bench.sh,
# tools/bench-slick.sh). Source it; it defines functions and the RESULTS /
# FAILED globals, and runs nothing.
#
# Statistics follow docs/compiler-refactor-plan.md's methodology: nearest-rank
# median + IQR + min/max, and a scenario with zero successful runs is
# reported FAILED (an integrity signal, never a perf verdict).

declare -a RESULTS=()
FAILED=0

# Wall seconds (millisecond precision) of one command, or "failed" when it
# exits non-zero: `time` prints a number even for a crash, so the exit
# status is the success signal. The command's output goes to <log>.
time_command() { # <log> <command...>
  local log="$1"
  shift
  local out rc
  out="$({ TIMEFORMAT=%3R; time "$@" >"$log" 2>&1; } 2>&1)"
  rc=$?
  case "$out.$rc" in
    *.*.0) printf '%s\n' "$out" ;;
    *)     echo "failed" ;;
  esac
}

# Appends one row to RESULTS: nearest-rank median + quartiles over values.
summarize() { # <name> <ok> <runs> <values...>
  local name="$1" ok="$2" runs="$3"
  shift 3
  if [ "$ok" -eq 0 ]; then
    RESULTS+=("$(printf '%-22s %8s %8s %8s %8s' "$name" \
      'FAILED' '-' '-' '-')")
    FAILED=1
    return
  fi
  local sorted line
  local -a S=()
  sorted="$(printf '%s\n' "$@" | sort -n)"
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
  RESULTS+=("$(printf '%-22s %8s %8s %8s %8s  %d/%d ok' "$name" \
    "$median" "$iqr" "${S[0]}" "${S[$((n - 1))]}" "$ok" "$runs")")
}

print_results() {
  printf '%-22s %8s %8s %8s %8s\n' scenario median IQR min max
  local row
  for row in "${RESULTS[@]}"; do
    echo "$row" | sed 's/  [0-9]*\/[0-9]* ok$//'
  done
}

# "<short sha>[+dirty]" of the repository at <root>.
commit_of() { # <root>
  local commit
  commit="$(git -C "$1" rev-parse --short HEAD 2>/dev/null || echo unknown)"
  [ -n "$(git -C "$1" status --porcelain 2>/dev/null)" ] && commit="$commit+dirty"
  printf '%s\n' "$commit"
}

machine_of() {
  uname -srm 2>/dev/null || uname -sm
}

# The build flavour recorded in a CMake build dir's cache.
build_type_of() { # <cmake build dir>
  local cache="$1/CMakeCache.txt" buildtype cxxflags
  buildtype="$(grep '^CMAKE_BUILD_TYPE:STRING=' "$cache" 2>/dev/null | cut -d= -f2-)"
  if [ -n "$buildtype" ]; then
    printf '%s\n' "$buildtype"
    return
  fi
  cxxflags="$(grep '^CMAKE_CXX_FLAGS:STRING=' "$cache" 2>/dev/null | cut -d= -f2-)"
  if [ -n "$cxxflags" ]; then
    printf 'default [%s]\n' "$cxxflags"
  else
    echo "default (no -O flag)"
  fi
}
