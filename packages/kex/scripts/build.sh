#!/usr/bin/env bash
# Builds the wasm target (if not already built) and assembles this
# package's dist/ directory: the built kex_repl_wasm.js/.wasm/.data
# alongside a copy of src/index.mjs, all flat in one directory so
# index.mjs's import.meta.url-relative locateFile resolves correctly
# regardless of how the package ends up laid out by npm/a bundler.
#
# Run from anywhere; paths are resolved relative to this script.
set -euo pipefail

PACKAGE_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
REPO_ROOT="$(cd "$PACKAGE_DIR/../.." && pwd)"
WASM_BUILD_DIR="$REPO_ROOT/build-wasm"

if [ ! -f "$WASM_BUILD_DIR/kex_repl_wasm.js" ]; then
  echo "Building wasm target (requires emsdk active — see third_party/gmp-wasm/README.md)..."
  (cd "$REPO_ROOT" && make build-wasm)
fi

DIST_DIR="$PACKAGE_DIR/dist"
rm -rf "$DIST_DIR"
mkdir -p "$DIST_DIR"

cp "$WASM_BUILD_DIR/kex_repl_wasm.js" "$DIST_DIR/"
cp "$WASM_BUILD_DIR/kex_repl_wasm.wasm" "$DIST_DIR/"
cp "$WASM_BUILD_DIR/kex_repl_wasm.data" "$DIST_DIR/"
cp "$PACKAGE_DIR/src/index.mjs" "$DIST_DIR/"
cp "$PACKAGE_DIR/src/index.d.ts" "$DIST_DIR/"

echo "Built $DIST_DIR"
