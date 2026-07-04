# GMP, prebuilt for Emscripten (wasm32)

Static build of GMP 6.3.0 targeting `wasm32-unknown-emscripten`, used only by
the wasm build (see `docs/fiber-process-plan.md`'s phase 6 notes). The native
build is unaffected — it keeps finding GMP via Homebrew, dynamically linked
only, per CMakeLists.txt's existing GPL/LGPL note.

No prebuilt wasm GMP exists via Emscripten's port system or Homebrew, so this
was built from source, **using Emscripten 5.0.7 specifically** (via `emsdk`,
not the Homebrew-packaged 6.0.2) — see "Emscripten version" below for why
that pin matters and isn't just an arbitrary choice:

```
curl -LO https://gmplib.org/download/gmp/gmp-6.3.0.tar.xz
tar xf gmp-6.3.0.tar.xz
mkdir gmp-6.3.0/build-wasm && cd gmp-6.3.0/build-wasm
emconfigure ../configure \
  --host=wasm32-unknown-emscripten \
  --build=aarch64-apple-darwin \
  --disable-assembly \
  --disable-shared \
  --enable-static \
  --enable-cxx \
  --prefix=<install-dir> \
  CC_FOR_BUILD=clang \
  CFLAGS="-O2"
emmake make -j8
emmake make install
```

`--disable-assembly` is required — GMP's hand-written assembly optimizations
are architecture-specific and don't exist for wasm; the portable C fallback
path is used instead (slower per-operation than native GMP, but correctness
only matters here, not speed — Integer arithmetic isn't expected to be a
wasm/browser REPL's bottleneck). `--build=aarch64-apple-darwin` matters:
without an explicit build triple, GMP's configure can misdetect whether it's
actually cross-compiling (emcc-compiled binaries are transparently runnable
via Node on the same host that builds them) and then fail looking for a
separate build-time compiler.

Only `include/{gmp,gmpxx}.h` and `lib/lib{gmp,gmpxx}.a` are vendored here
(~1.5MB total) — not the full GMP source tree or build directory.

## Emscripten version

The wasm build **must** use Emscripten 5.0.7, not whatever Homebrew's
`emscripten` formula currently ships (6.0.2 at the time this was set up).
Confirmed empirically: 6.0.2 has a real regression where
`-sASYNCIFY` (required for `src/interpreter/fiber.cxx`'s wasm fiber backend)
combined with `-sNO_DISABLE_EXCEPTION_CATCHING` (required — the interpreter
uses C++ exceptions as its core control-flow mechanism) hangs on the very
first `emscripten_fiber_swap` call, even with zero exceptions ever thrown.
Reproduced in a ~20-line standalone repro with no Kex code involved, so
this isn't specific to anything in this project. 5.0.7 doesn't have this
problem — both fibers and exception catching work correctly together.

Install via `emsdk`, not Homebrew, to get this specific version:

```
git clone https://github.com/emscripten-core/emsdk.git
cd emsdk && ./emsdk install 5.0.7 && ./emsdk activate 5.0.7
source ./emsdk_env.sh   # do this in every shell before building
```

Worth periodically re-testing newer Emscripten releases against the minimal
repro above (a fiber swap + a `-sNO_DISABLE_EXCEPTION_CATCHING`-compiled
program, nothing else) before assuming this pin needs to stay forever — it
was a version-specific regression, not a fundamental incompatibility.
