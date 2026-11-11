#!/usr/bin/env bash
# Duplication and magic-constant audit for src/. `make dup-audit` runs it.
#
# The counts are heuristics, not a proof. They catch the copy-paste shapes
# this codebase keeps growing, so duplication is found by the process instead
# of in review:
#
#   duplicate-helpers   free functions with a BODY in more than one file
#                       (a declaration in x.hxx plus its definition in x.cxx
#                       does not count)
#   literal-compares    `== "..."` / `!= "..."` against a string literal:
#                       names that want a constant or an enum
#   type-recursions     functions that spell out >= 5 semantic Type kinds
#                       by hand (wants forEachTypeChild / mapTypeChildren)
#   expr-walkers        functions that spell out >= 6 AST Expr kinds by hand
#                       (wants ast::forEachChild, unless it is a real pass)
#   long-functions      functions over 400 lines
#   big-files           source files over 2000 lines
#   lowering-rederives  places lowering reads an expression's static type
#                       (`hir->typeOf`, a `staticTypeOf`, a printed type) to
#                       decide something itself instead of reading the
#                       checker's decision (pass plan, Stage 3)
#   walker-rederives    places the walker reads an expression's static type
#                       (`staticTypeOf`) to decide something itself (Stage 4)
#   lowering-resolves   places lowering resolves a name, reads a written
#                       type annotation, or reaches the checker's own
#                       machinery (a `src/semantic` include, type equality,
#                       trait satisfaction, a semantic type) instead of
#                       reading the checked program's answers through
#                       `hir/hir.hxx` (docs/declaration-identity-plan.md)
#
#   tools/dup-audit.sh            report counts, then the details
#   tools/dup-audit.sh --check    also fail when a count exceeds the recorded
#                                 tools/dup-audit.baseline (counts may only fall)
#   tools/dup-audit.sh --record   rewrite tools/dup-audit.baseline
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel)"
cd "$ROOT" || exit 1
BASELINE="tools/dup-audit.baseline"
MODE="${1:-report}"

FILES="$(git ls-files src | grep -E '\.(cxx|hxx)$')"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/kex-dup-audit.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

# ── duplicate helpers ───────────────────────────────────────────────────────
# A definition is `auto name(` at the start of a line (not `Class::name`)
# whose signature reaches `{` before `;`.
for f in $FILES; do
  awk -v F="$f" '
    pending != "" {
      if ($0 ~ /;[[:space:]]*$/ && $0 !~ /\{/) { pending = ""; next }
      if ($0 ~ /\{/) { print pending, F; pending = ""; next }
      if (++lines > 6) pending = ""
      next
    }
    match($0, /^[[:space:]]*((static|inline|constexpr)[[:space:]]+)*auto[[:space:]]+[A-Za-z_][A-Za-z0-9_]*\(/) {
      sig = substr($0, RSTART, RLENGTH)
      sub(/\($/, "", sig); n = split(sig, parts, /[[:space:]]+/); name = parts[n]
      if ($0 ~ /\{/) { print name, F; next }
      if ($0 ~ /;[[:space:]]*$/) next
      pending = name; lines = 0
    }' "$f"
done | sort -u >"$WORK/defs"
# Names defined in 2+ files (x.hxx + x.cxx count once), ignoring ubiquitous
# member-ish names.
awk '{ stem = $2; sub(/\.(cxx|hxx)$/, "", stem)
       if (!(($1, stem) in seen)) { seen[$1, stem] = 1; n[$1]++ }
       files[$1] = files[$1] " " $2 }
     END { for (k in n) if (n[k] > 1) print n[k], k, files[k] }' "$WORK/defs" |
  grep -vE ' (main|clone|run|get|set|lookup|define|each|visit|operator|begin|end|size|empty|push|pop|reset|clear|str|toString|hash|parse|emit|lower|check|analyze|record|tuple|list|atom|error|none|just|unit|integer|floating|boolean|binary|string|variant|claim|atEnd|warning|pushScope|popScope) ' |
  sort -rn >"$WORK/duplicate-helpers"

# ── literal compares ────────────────────────────────────────────────────────
for f in $FILES; do
  c="$(grep -cE '[=!]= "[^"]*"' "$f")"
  [ "$c" -gt 0 ] && echo "$c $f"
done | sort -rn >"$WORK/literal-compares"

# ── hand-written recursions, long functions ─────────────────────────────────
# One awk pass per file: split at function heads, count distinct kind names
# and lines per function.
TYPE_KINDS='PrimitiveType|SizedIntType|SizedFloatType|NamedType|FuncType|TupleType|ListType|MapType|OptionalType|UnionType|IntersectionType|RecordType|TypeVar|UnknownType|VoidType|ConstrainedType'
EXPR_KINDS='MethodCall|FunctionCall|TaggedLiteral|RecordConstruction|BinaryOp|UnaryOp|TupleExpr|ListExpr|MapExpr|RangeExpr|IfExpr|MatchExpr|ReceiveExpr|LoopExpr|WhileExpr|LetExpr|LambdaExpr|BlockExpr|TryExpr'
for f in $FILES; do
  awk -v F="$f" -v TK="$TYPE_KINDS" -v EK="$EXPR_KINDS" '
    function flush() {
      if (name == "") return
      if (nt >= 5) print "type", nt, F ":" start, name
      if (ne >= 6) print "expr", ne, F ":" start, name
      if (NR - start > 400) print "long", NR - start, F ":" start, name
    }
    /^[[:space:]]{0,8}((static|inline|template[^>]*>)[[:space:]]+)*(auto|void|bool|int)[[:space:]]+[A-Za-z_][A-Za-z0-9_:]*\(/ && !/;[[:space:]]*$/ ||
    /std::function<.*> *[A-Za-z_]+ *=/ {
      flush(); name = $0; sub(/^[[:space:]]+/, "", name); name = substr(name, 1, 90)
      start = NR; nt = 0; ne = 0; split("", seenT); split("", seenE)
      # A one-line body ends where it starts.
      if ($0 ~ /\{.*\}[[:space:]]*;?[[:space:]]*$/) oneLine = 1; else oneLine = 0
    }
    {
      s = $0
      while (match(s, "(" TK ")")) { k = substr(s, RSTART, RLENGTH); if (!(k in seenT)) { seenT[k] = 1; nt++ }; s = substr(s, RSTART + RLENGTH) }
      s = $0
      while (match(s, "(" EK ")")) { k = substr(s, RSTART, RLENGTH); if (!(k in seenE)) { seenE[k] = 1; ne++ }; s = substr(s, RSTART + RLENGTH) }
    }
    oneLine { flush(); name = ""; oneLine = 0 }
    END { NR = NR + 1; flush() }' "$f"
done >"$WORK/functions"
grep '^type ' "$WORK/functions" | cut -d' ' -f2- | sort -rn >"$WORK/type-recursions"
grep '^expr ' "$WORK/functions" | cut -d' ' -f2- | sort -rn >"$WORK/expr-walkers"
grep '^long ' "$WORK/functions" | cut -d' ' -f2- | sort -rn >"$WORK/long-functions"
# shellcheck disable=SC2086
wc -l $FILES | awk '$2 != "total" && $1 > 2000 { print $1, $2 }' | sort -rn >"$WORK/big-files"
grep -nE "expressionTypes->find\(|hir->typeOf\(|staticTypeOf\(|typeToString\(" src/ir/*.cxx >"$WORK/lowering-rederives" || true
grep -n "staticTypeOf(" src/interpreter/*.cxx >"$WORK/walker-rederives" || true
grep -nE 'typeText\(|structuredTypeOfDeclared|expandDeclaredAliases|resolve_core::|resolveTraitName|resolveInScopes|matchSuffix|#include "\.\./semantic/|TraitRegistry|typesEqual|argMatchesParam|\.satisfies\(|semantic::TypePtr|semantic::Type::' src/ir/*.cxx src/ir/*.hxx >"$WORK/lowering-resolves" || true

# ── report ──────────────────────────────────────────────────────────────────
METRICS="duplicate-helpers literal-compares type-recursions expr-walkers long-functions big-files lowering-rederives walker-rederives lowering-resolves"
count_of() { # <metric>
  if [ "$1" = literal-compares ]; then
    awk '{ s += $1 } END { print s + 0 }' "$WORK/$1"
  else
    wc -l <"$WORK/$1" | tr -d ' '
  fi
}

echo "kex dup-audit — $(git rev-parse --short HEAD)"
printf '%-20s %8s %10s\n' metric count baseline
status=0
for m in $METRICS; do
  now="$(count_of "$m")"
  base="$(awk -v m="$m" '$1 == m { print $2 }' "$BASELINE" 2>/dev/null)"
  flag=""
  if [ -n "$base" ] && [ "$now" -gt "$base" ]; then
    flag="  ROSE"
    [ "$MODE" = --check ] && status=1
  fi
  printf '%-20s %8s %10s%s\n' "$m" "$now" "${base:--}" "$flag"
done

if [ "$MODE" = --record ]; then
  for m in $METRICS; do echo "$m $(count_of "$m")"; done >"$BASELINE"
  echo "recorded $BASELINE"
fi

if [ "$MODE" != --check ] || [ "$status" -ne 0 ]; then
  for m in $METRICS; do
    echo ""
    echo "── $m (top 15)"
    head -15 "$WORK/$m" | cut -c1-160
  done
fi
exit "$status"
