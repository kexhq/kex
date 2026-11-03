#!/bin/sh
# Devirt correspondence check (Phase 4): every imported `name/Head`
# spelling the compiler applies must exist in the defining beam's
# exports at the called arity. Runs the spec corpus through
# --emit-core with KEX_DEVIRT_AUDIT, then checks each distinct
# audited call against `beam_lib:chunks(exports)`.
#
# Usage: tools/devirt-correspondence.sh <kex-binary> <spec.kex...>
set -u

KEX="$1"; shift
tmp=$(mktemp -d /tmp/kex-devirt-XXXXXX) || exit 1
trap 'rm -rf "$tmp"' EXIT INT TERM
audit="$tmp/audit.log"
core="$tmp/core"
mkdir -p "$core"

mkdir -p "$tmp/prelude"
"$KEX" --build-prelude "$tmp/prelude" >/dev/null 2>&1 || {
    echo "devirt-correspondence: --build-prelude failed" >&2
    exit 1
}
beam=$(find "$tmp/prelude" -name 'kex_prelude.beam' | head -1)
if [ -z "$beam" ]; then
    echo "devirt-correspondence: no kex_prelude.beam built" >&2
    exit 1
fi

compiled=0; skipped=0
for f in "$@"; do
    case "$f" in *.kex) ;; *) continue;; esac
    exp_file="${f%.kex}.expected"
    if [ ! -f "$exp_file" ]; then skipped=$((skipped + 1)); continue; fi
    if grep -q "# kex: check-only" "$f" 2>/dev/null; then
        skipped=$((skipped + 1)); continue
    fi
    if grep -q "# kex: types-only" "$f" 2>/dev/null; then
        skipped=$((skipped + 1)); continue
    fi
    if grep -q "# kex: skip-beam" "$f" 2>/dev/null; then
        skipped=$((skipped + 1)); continue
    fi
    if KEX_DEVIRT_AUDIT="$audit" "$KEX" --emit-core --no-colors \
            -o "$core" "$f" >/dev/null 2>&1; then
        compiled=$((compiled + 1))
    else
        skipped=$((skipped + 1))
    fi
done

if [ ! -f "$audit" ]; then
    echo "devirt-correspondence: no firing recorded over $compiled files" >&2
    echo "  (the devirt path looks dead — predicate or hookup regressed?)" >&2
    exit 1
fi
firings=$(wc -l < "$audit" | tr -d ' ')
distinct=$(sort -u "$audit" | wc -l | tr -d ' ')
echo "devirt-correspondence: $firings firings, $distinct distinct," \
    "$compiled compiled, $skipped skipped"

erl -noshell -eval "{ok, {_, [{exports, Ex}]}} = beam_lib:chunks(\"$beam\", [exports]), lists:foreach(fun({F, A}) -> io:format(\"~s ~w~n\", [F, A]) end, Ex)." -s init stop > "$tmp/exports.txt" || {
    echo "devirt-correspondence: beam_lib read failed" >&2
    exit 1
}

miss=0; foreign=0
sort -u "$audit" | while read -r module fn arity; do
    if [ "$module" != "kex_prelude" ]; then
        echo "  FOREIGN firing: $module $fn/$arity" >&2
        continue
    fi
    if ! grep -qxF "$fn $arity" "$tmp/exports.txt"; then
        echo "  MISSING export: kex_prelude $fn/$arity" >&2
        echo "   kex_prelude exports for that name:" >&2
        grep "^${fn%%/*} " "$tmp/exports.txt" | sed 's/^/    /' >&2 || true
    fi
done > "$tmp/report.txt" 2>&1
miss=$(grep -c "MISSING export" "$tmp/report.txt" || true)
foreign=$(grep -c "FOREIGN firing" "$tmp/report.txt" || true)
cat "$tmp/report.txt"
if [ "$miss" != "0" ] || [ "$foreign" != "0" ]; then
    echo "devirt-correspondence: $miss missing, $foreign foreign" >&2
    exit 1
fi
echo "devirt-correspondence: all $distinct distinct spellings exist"
