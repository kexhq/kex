#!/usr/bin/env bash
# Regenerate the full docs site: every released tag plus the current
# (unreleased) checkout, for the prelude/stdlib and for tey.
#
#   tools/docgen/build-all.sh [output-dir]
#
# output-dir defaults to ../kdocs relative to the repo root — the kdocs
# checkout that is published to docs.kex.run. Each released tag is built
# from a temporary git worktree; the unreleased build uses the working
# tree as-is. Builds run through the CURRENT toolchain: docgen parses old
# sources with the current compiler, and files that no longer parse are
# skipped with a warning rather than failing the release.
set -uo pipefail

ROOT="$(git rev-parse --show-toplevel)"
OUT="${1:-$(cd "$ROOT/.." && pwd)/kdocs}"
TEY_RUN="$ROOT/tey-run"
WORKTREES="$ROOT/.cache/docgen-tags"

if [ ! -x "$TEY_RUN" ]; then
  echo "build-all: no ./tey-run — run: make build-tey" >&2
  exit 1
fi

mkdir -p "$OUT" "$WORKTREES"

cleanup() {
  git -C "$ROOT" worktree list --porcelain | grep "^worktree $WORKTREES/" | cut -d' ' -f2- | while read -r wt; do
    git -C "$ROOT" worktree remove --force "$wt" 2>/dev/null || true
  done
}
trap cleanup EXIT

# build_docs <source-dir> <package> <label> [version]
build_docs() {
  local source="$1" package="$2" label="$3" version="${4:-}"
  if [ ! -d "$source" ]; then
    echo "build-all: skip $package (no $source)"
    return 0
  fi
  local args=(docs -- build --source "$source" --out "$OUT" --package "$package" --label "$label")
  if [ -n "$version" ]; then
    args+=(--version "$version")
  fi
  echo "build-all: $package $version"
  (cd "$ROOT/tools/docgen" && "$TEY_RUN" "${args[@]}") || \
    echo "build-all: WARNING $package $version failed — keeping previous output" >&2
}

for tag in $(git -C "$ROOT" tag --sort=v:refname); do
  version="${tag#v}"
  wt="$WORKTREES/$version"
  git -C "$ROOT" worktree add --detach --force "$wt" "$tag" >/dev/null 2>&1 || {
    echo "build-all: WARNING cannot worktree $tag — skipped" >&2
    continue
  }
  build_docs "$wt/src/stdlib" prelude "Prelude" "$version"
  build_docs "$wt/tey/src" tey "Tey" "$version"
  git -C "$ROOT" worktree remove --force "$wt" >/dev/null 2>&1 || true
done

# The unreleased build: version defaults to the compiler's own.
build_docs "$ROOT/src/stdlib" prelude "Prelude"
build_docs "$ROOT/tey/src" tey "Tey"

echo "build-all: done -> $OUT"
