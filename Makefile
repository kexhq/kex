.PHONY: build-tey spec-tey build test spec spec-orphans spec-prelude spec-stdlib spec-beam spec-stdlib-beam spec-wasm test-all clean repl run check install uninstall help build-wasm test-wasm web-demo

BUILD_DIR = build
KEX = $(BUILD_DIR)/kex
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
STDLIBDIR ?= $(PREFIX)/share/kex/stdlib

WASM_BUILD_DIR = build-wasm

# Every `cmake --build` below is parallel. The tree is ~56 translation units
# and was compiling them one at a time, which is most of what a CI run spent
# its time on — the wasm job's build step alone was 10.6 of its 17 minutes,
# with the GMP and PCRE2 builds beside it already using -j"$(nproc)".
# `--parallel` with no number lets CMake use every core it finds, which is
# what a developer machine and a runner both want. Override to serialize when
# a compiler error's output is interleaved past reading: make JOBS=1.
JOBS ?=
CMAKE_BUILD_JOBS = --parallel $(JOBS)

# GNU `timeout` bounds a backend that hangs, but macOS ships without it
# (coreutils installs it as `gtimeout`). Fall back to running unbounded rather
# than failing every spec with "timeout: command not found" — which is what the
# macOS CI job did the moment `spec-beam` started gating.
TIMEOUT_CMD := $(shell command -v timeout 2>/dev/null || command -v gtimeout 2>/dev/null)
# 8s was tuned on a warm dev machine, where a BEAM spec takes ~0.35s. A cold CI
# runner is far slower, and on Linux `timeout` signals the whole process group —
# so a spec that overran took erlc down with it and reported `erlc failed`,
# which reads like a codegen bug rather than a deadline. The bound exists to
# catch a HUNG backend, so it can be generous.
TIMEOUT_SPEC := $(if $(TIMEOUT_CMD),$(TIMEOUT_CMD) 30,)
TIMEOUT_SUITE := $(if $(TIMEOUT_CMD),$(TIMEOUT_CMD) 90,)

help:
	@echo "Kex Language Compiler"
	@echo ""
	@echo "  make build        Build the compiler"
	@echo "  make test         Run all unit tests (C++ test binaries)"
	@echo "  make spec         Run spec programs and verify output"
	@echo "  make test-all     Run unit tests + spec suites, incl. BEAM (used by CI)"
	@echo "  make build-tey    Compile Tey with the freshly built kex"
	@echo "  make tey-run      Write ./tey-run, a Tey launcher for this checkout"
	@echo "  make spec-tey     Run Tey own spec suite (requires erlc)"
	@echo "  make parse        Parse all examples (syntax check)"
	@echo "  make repl         Start the REPL"
	@echo "  make install      Install kex to $(BINDIR)"
	@echo "  make uninstall    Remove kex from $(BINDIR)"
	@echo "  make clean        Remove native, wasm, and package build artifacts"
	@echo "  make run F=<file>  Run a .kex file"
	@echo "  make check F=<file>  Semantic check a .kex file"
	@echo "  make build-wasm   Build the Emscripten/wasm target (requires emsdk"
	@echo "                    active, pinned per third_party/gmp-wasm/README.md,"
	@echo "                    and a prebuilt third_party/gmp-wasm/{include,lib})"
	@echo "  make test-wasm    Build the wasm target + run its test suite via Node"
	@echo "  make web-demo     Build the wasm target and serve web/index.html locally"
	@echo "                    (in-browser REPL test page) — Ctrl-C to stop"
	@echo "  make spec-beam    Run the spec suite through the BEAM backend (-R) and"
	@echo "                    check it against the tree-walker's golden output."
	@echo "                    FAILS on any difference — the two backends are"
	@echo "                    expected to agree on every spec."
	@echo "  make spec-wasm    Same, but through the wasm-built kex CLI via Node"
	@echo "                    (requires build-wasm; expected to match closely,"
	@echo "                    since it's the same tree-walker as native)."
	@echo ""

# Configure quietly, but NEVER quietly: piping cmake into `tail` discarded its
# exit status, so a failed configure (a stale cache naming a compiler that
# cannot build for this machine, say) went unnoticed and the build ran on into
# a half-generated tree — surfacing as "Not a file: VerifyGlobs.cmake" or
# mismatched archives, neither of which names the real problem. On failure the
# whole configure output is printed and the build stops.
build:
	@output=$$(cmake -B $(BUILD_DIR) -G "Unix Makefiles" 2>&1) || { \
		echo "$$output"; \
		echo ""; \
		echo "cmake could not configure $(BUILD_DIR)."; \
		echo "If its cache names the wrong compiler, remove the directory and try again:"; \
		echo "    rm -rf $(BUILD_DIR) && make build"; \
		exit 1; \
	}; echo "$$output" | tail -1
	@cmake --build $(BUILD_DIR) $(CMAKE_BUILD_JOBS)

test: build
	@ctest --test-dir $(BUILD_DIR) --output-on-failure

# See third_party/gmp-wasm/README.md — requires `emsdk` active (pinned to
# 5.0.7; newer versions have a real Asyncify+exceptions regression) and a
# prebuilt third_party/gmp-wasm/{include,lib} (not checked in — rebuild
# locally per that README, or see .github/workflows/ci.yml for how CI
# builds and caches it).
# The wasm build reuses the NATIVE build's compiled stdlib unit
# (kex_prelude.beam and its Kex.* companions). Producing it means running the
# native kex through erlc, which a cross-compiled build cannot do — but
# READING it is pure parsing, and without it the checker falls back to
# extracting interfaces from .kex source, where inferred result types and
# trait bounds are simply absent. That made the same program type-check
# differently under wasm than natively. When no native build is present the
# fallback still applies, so this stays a soft dependency.
build-wasm:
	@emcmake cmake -B $(WASM_BUILD_DIR) \
		-DKEX_PREBUILT_RUNTIME_DIR=$(CURDIR)/$(BUILD_DIR)/runtime/beam
	@cmake --build $(WASM_BUILD_DIR) $(CMAKE_BUILD_JOBS)

# Only interpreter_test is run here — it's the suite this project has
# actually verified passes under wasm (187/187, matching the native suite)
# throughout the process-model/wasm work; the CLI-driving test binaries
# (repl_cli_test, color_cli_test) shell out to a native `kex` executable
# path that doesn't exist in a wasm build, so they're a native-only concern.
test-wasm: build-wasm
	@node $(WASM_BUILD_DIR)/interpreter_test.js

# Serves the repo root so web/index.html can load ../build-wasm/kex_repl_wasm.js
# (see that file's own relative import) — must run from the repo root, not
# from inside web/. Ctrl-C to stop.
web-demo: build-wasm
	@echo "Demo running at http://localhost:8743/web/index.html (Ctrl-C to stop)"
	@$(KEX) -R tools/serve.kex 8743

# Every suite gates, on both backends: a walker/BEAM difference is a build
# failure, not a note.
test-all: test spec-orphans spec spec-prelude spec-stdlib \
          spec-beam spec-prelude-beam spec-stdlib-beam spec-test-json

SHELL := /bin/bash

# A spec that no target runs looks exactly like a passing one. `spec` and
# `spec-beam` both skip any spec/*.kex with no .expected sibling, and the
# describe/it suites live in spec/stdlib/ — so a suite dropped into spec/
# with neither was silently never executed. Nine of them had accumulated,
# one of which did not compile on BEAM at all (kexhq/kex#231).
spec-orphans:
	@orphans=""; \
	for f in spec/*.kex; do \
		[ -e "$$f" ] || continue; \
		if [ -f "$${f%.kex}.expected" ]; then continue; fi; \
		orphans="$$orphans $$f"; \
	done; \
	if [ -n "$$orphans" ]; then \
		echo "error: spec files that no target runs:"; \
		for f in $$orphans; do echo "  $$f"; done; \
		echo ""; \
		echo "Give each one a .expected golden file, or move a describe/it"; \
		echo "suite to spec/stdlib/ where spec-stdlib runs it."; \
		exit 1; \
	fi; \
	echo "No orphaned specs."

spec: build
	@echo "Running spec programs..."
	@failed=0; passed=0; \
	for f in spec/*.kex; do \
		exp_file="$${f%.kex}.expected"; \
		if [ ! -f "$$exp_file" ]; then continue; fi; \
		kex_flags="--no-colors"; \
		if grep -q "# kex: no-check" "$$f" 2>/dev/null; then kex_flags="$$kex_flags --no-check"; fi; \
		if grep -q "# kex: check-only" "$$f" 2>/dev/null; then kex_flags="-C --no-colors"; fi; \
		if grep -q "# kex: types-only" "$$f" 2>/dev/null; then kex_flags="-C -t --no-colors"; fi; \
		if grep -q "# kex: run-beam" "$$f" 2>/dev/null; then \
			kex_flags="-R --no-colors"; \
			if grep -q "# kex: no-check" "$$f" 2>/dev/null; then kex_flags="$$kex_flags --no-check"; fi; \
		fi; \
		if grep -q "# kex: compile-run" "$$f" 2>/dev/null; then \
			tmpdir=$$(mktemp -d /tmp/kex_spec_cr_XXXXXX); \
			$(KEX) -c --no-colors -o "$$tmpdir" "$$f" > /dev/null 2>&1; \
			beamfile="$$tmpdir/kex_$$(basename "$${f%.kex}").beam"; \
			actual=$$($(KEX) "$$beamfile" 2>&1); \
			rm -rf "$$tmpdir"; \
		else \
		actual=$$($(KEX) $$kex_flags "$$f" 2>&1); \
		fi; \
		expected=$$(cat "$$exp_file"); \
		if [ "$$actual" = "$$expected" ]; then \
			printf "  \033[32m✓\033[0m %s\n" "$$(basename $$f)"; \
			passed=$$((passed + 1)); \
		else \
			printf "  \033[31m✗\033[0m %s\n" "$$(basename $$f)"; \
			diff <(echo "$$actual") <(echo "$$expected") | head -10 | sed 's/^/    /'; \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	echo ""; \
	echo "  $$passed passing, $$failed failing"; \
	[ $$failed -eq 0 ]

# `rc` is authoritative: a suite with failures now exits non-zero (see
# kex_test:maybe_print_summary and the walker's testsFailed check). The tallies
# are still scraped out of the output because this loop reports TOTALS across
# files, which no single exit status can carry.
spec-prelude: build
	@echo "Running prelude spec suite..."
	@failed=0; passed=0; \
	for f in spec/prelude/*.kex; do \
		output=$$($(KEX) --no-check --no-colors "$$f" 2>&1); \
		rc=$$?; \
		f_passed=$$(echo "$$output" | grep -oE '[0-9]+ passed' | grep -oE '[0-9]+'); \
		f_failed=$$(echo "$$output" | grep -oE '[0-9]+ failed' | grep -oE '[0-9]+'); \
		f_passed=$${f_passed:-0}; f_failed=$${f_failed:-0}; \
		if [ "$$rc" -eq 0 ] && [ "$$f_failed" -eq 0 ]; then \
			printf "  \033[32m✓\033[0m %s (%s passed)\n" "$$(basename $$f)" "$$f_passed"; \
		else \
			printf "  \033[31m✗\033[0m %s (%s passed, %s failed)\n" "$$(basename $$f)" "$$f_passed" "$$f_failed"; \
			echo "$$output" | grep '✗' | sed 's/^/    /'; \
		fi; \
		passed=$$((passed + f_passed)); \
		failed=$$((failed + f_failed)); \
	done; \
	echo ""; \
	echo "  $$passed passed, $$failed failed"; \
	[ $$failed -eq 0 ]

# Same gating rule as spec-beam. A spec whose subject IS the tree-walker (the
# backend predicates in kex.spec.kex, whose BEAM counterpart is
# spec/backend_predicates_beam.kex) marks itself `# kex: interpreter-only` and
# is skipped here rather than counted as a difference.
spec-prelude-beam: build
	@echo "Running prelude spec suite through BEAM (-R)..."
	@failed=0; passed=0; \
	for f in spec/prelude/*.kex; do \
		if grep -q "# kex: interpreter-only" "$$f" 2>/dev/null; then continue; fi; \
		output=$$($(TIMEOUT_SUITE) $(KEX) -R --no-check --no-colors "$$f" 2>&1); \
		rc=$$?; \
		f_passed=$$(echo "$$output" | grep -oE '[0-9]+ passed' | grep -oE '[0-9]+'); \
		f_failed=$$(echo "$$output" | grep -oE '[0-9]+ failed' | grep -oE '[0-9]+'); \
		f_passed=$${f_passed:-0}; f_failed=$${f_failed:-0}; \
		if [ "$$rc" -eq 0 ] && [ "$$f_failed" -eq 0 ]; then \
			printf "  \033[32m✓\033[0m %s (%s passed)\n" "$$(basename $$f)" "$$f_passed"; \
		else \
			printf "  \033[31m✗\033[0m %s (%s passed, %s failed)\n" "$$(basename $$f)" "$$f_passed" "$$f_failed"; \
			echo "$$output" | grep '✗' | sed 's/^/    /'; \
		fi; \
		passed=$$((passed + f_passed)); \
		failed=$$((failed + f_failed)); \
	done; \
	echo ""; \
	echo "  $$passed passed, $$failed failed"; \
	[ $$failed -eq 0 ]

# Runs the whole spec suite through -R (BEAM) instead of the per-file tag
# system `spec` uses, and diffs against the SAME .expected golden files —
# any mismatch means the two backends produce different output for that
# program. Skips check-only specs (those are about semantic checking, not
# runtime execution — same exclusion `spec` doesn't need since it already
# dispatches per-tag).
#
# GATING as of 2026-08-12: the suite has matched the walker on every spec for
# long enough that a new mismatch means a regression, not a known gap. The
# remaining walker/BEAM differences are representation-level (pid rendering, a
# Range printing as its item list) and live outside this suite.
# Strings are UTF-8 binaries and Chars are tagged {'Char', N}
# tuples on BEAM, so the old charlist ambiguities ([] vs "", [Int] vs
# String, Char vs Int) are gone.
#
# stdout and stderr are captured SEPARATELY and concatenated (stdout first)
# rather than merged with `2>&1`. A BEAM node writes the two streams through
# two independent ports whose flush order is not guaranteed, so a merged
# capture is a race: spec/module_scoped_make.kex produced its fatal
# `Internal error:` line between two stdout lines on one CI run and after
# them on another, from the same commit. The walker (plain C++ streams) has no
# such reordering, so the goldens all end with whatever went to stderr, and
# every spec in this suite matches under the separated capture.
spec-beam: build
	@echo "Running spec suite through BEAM (-R)..."
	@failed=0; passed=0; \
	beam_out=$$(mktemp); beam_err=$$(mktemp); \
	trap 'rm -f "$$beam_out" "$$beam_err"' EXIT INT TERM; \
	for f in spec/*.kex; do \
		exp_file="$${f%.kex}.expected"; \
		if [ ! -f "$$exp_file" ]; then continue; fi; \
		if grep -q "# kex: check-only" "$$f" 2>/dev/null; then continue; fi; \
		if grep -q "# kex: types-only" "$$f" 2>/dev/null; then continue; fi; \
		if grep -q "# kex: skip-beam" "$$f" 2>/dev/null; then continue; fi; \
		$(TIMEOUT_SPEC) $(KEX) -R --no-colors "$$f" >"$$beam_out" 2>"$$beam_err" || true; \
		actual=$$(cat "$$beam_out" "$$beam_err"); \
		expected=$$(cat "$$exp_file"); \
		if [ "$$actual" = "$$expected" ]; then \
			printf "  \033[32m✓\033[0m %s\n" "$$(basename $$f)"; \
			passed=$$((passed + 1)); \
		else \
			printf "  \033[31m✗\033[0m %s (BEAM output differs)\n" "$$(basename $$f)"; \
			diff <(echo "$$actual") <(echo "$$expected") | head -10 | sed 's/^/    /'; \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	echo ""; \
	echo "  $$passed matching, $$failed differing"; \
	[ $$failed -eq 0 ]

# Same idea, but through the wasm-built `kex` CLI (via Node, using
# NODERAWFS for real file access — see CMakeLists.txt) instead of BEAM —
# this is the SAME tree-walker as native, so it's expected to match very
# closely; any mismatch here is more likely a genuine wasm-specific
# regression worth investigating, not a documented feature gap.
spec-wasm: build-wasm
	@echo "Running spec suite through the wasm-built kex CLI (via Node)..."
	@failed=0; passed=0; \
	for f in spec/*.kex; do \
		exp_file="$${f%.kex}.expected"; \
		if [ ! -f "$$exp_file" ]; then continue; fi; \
		kex_flags="--no-colors"; \
		if grep -q "# kex: no-check" "$$f" 2>/dev/null; then kex_flags="$$kex_flags --no-check"; fi; \
		if grep -q "# kex: check-only" "$$f" 2>/dev/null; then kex_flags="-C --no-colors"; fi; \
		if grep -q "# kex: types-only" "$$f" 2>/dev/null; then kex_flags="-C -t --no-colors"; fi; \
		if grep -q "# kex: run-beam" "$$f" 2>/dev/null; then continue; fi; \
		if grep -q "# kex: skip-wasm" "$$f" 2>/dev/null; then continue; fi; \
		actual=$$($(TIMEOUT_SPEC) node $(WASM_BUILD_DIR)/kex.js $$kex_flags "$$f" 2>&1); \
		expected=$$(cat "$$exp_file"); \
		if [ "$$actual" = "$$expected" ]; then \
			printf "  \033[32m✓\033[0m %s\n" "$$(basename $$f)"; \
			passed=$$((passed + 1)); \
		else \
			printf "  \033[31m✗\033[0m %s\n" "$$(basename $$f)"; \
			diff <(echo "$$actual") <(echo "$$expected") | head -10 | sed 's/^/    /'; \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	echo ""; \
	echo "  $$passed passing, $$failed failing"; \
	[ $$failed -eq 0 ]

# The machine-readable reporting modes of kexhq/kex#199 — what an editor's
# test explorer reads. Each mode is checked against a golden AND, separately,
# the two backends are diffed against each other: the shape is a contract, and
# a tool cannot care which backend ran the suite. Durations are normalised
# away; nothing else is, deliberately — the source locations are the whole
# point of these records, so a drift in them must fail here.
#
# The fixture lives in spec/test_explorer/ rather than spec/, so the suites
# that glob `spec/*.kex` leave it alone: it fails two cases on purpose.
TEST_JSON_FIXTURE := spec/test_explorer/reporting.spec.kex
TEST_JSON_NORMALISE := sed -E 's/"durationMs":[0-9.]+/"durationMs":0/'

spec-test-json: build
	@echo "Checking the JSON test-reporting modes (--test-json/--test-list/--test-only)..."
	@failed=0; \
	for case in "json:--test-json" "list:--test-list" \
	            "only:--test-json --test-only 'reporting > nested'"; do \
		name=$${case%%:*}; flags=$${case#*:}; \
		golden="spec/test_explorer/reporting.$$name.expected"; \
		walker=$$(eval $(TIMEOUT_SPEC) $(KEX) --no-colors $$flags "$(TEST_JSON_FIXTURE)" 2>&1 | $(TEST_JSON_NORMALISE)); \
		beam=$$(eval $(TIMEOUT_SPEC) $(KEX) -R --no-colors $$flags "$(TEST_JSON_FIXTURE)" 2>&1 | $(TEST_JSON_NORMALISE)); \
		expected=$$(cat "$$golden"); \
		for backend in walker beam; do \
			actual=$$(if [ "$$backend" = walker ]; then echo "$$walker"; else echo "$$beam"; fi); \
			if [ "$$actual" = "$$expected" ]; then \
				printf "  \033[32m✓\033[0m %s (%s)\n" "$$name" "$$backend"; \
			else \
				printf "  \033[31m✗\033[0m %s (%s differs from %s)\n" "$$name" "$$backend" "$$golden"; \
				diff <(echo "$$actual") <(echo "$$expected") | head -10 | sed 's/^/    /'; \
				failed=$$((failed + 1)); \
			fi; \
		done; \
	done; \
	echo ""; \
	[ $$failed -eq 0 ]

spec-stdlib: build
	@echo "Running opt-in stdlib spec suite..."
	@failed=0; passed=0; \
	for f in spec/stdlib/*.kex; do \
		kex_flags="--no-colors"; \
		if grep -q "# kex: no-check" "$$f" 2>/dev/null; then kex_flags="$$kex_flags --no-check"; fi; \
		output=$$($(KEX) $$kex_flags "$$f" 2>&1); \
		rc=$$?; \
		f_passed=$$(echo "$$output" | grep -oE '[0-9]+ passed' | grep -oE '[0-9]+'); \
		f_failed=$$(echo "$$output" | grep -oE '[0-9]+ failed' | grep -oE '[0-9]+'); \
		f_passed=$${f_passed:-0}; f_failed=$${f_failed:-0}; \
		if [ "$$rc" -eq 0 ] && [ "$$f_failed" -eq 0 ]; then \
			printf "  \033[32m✓\033[0m %s (%s passed)\n" "$$(basename $$f)" "$$f_passed"; \
		else \
			printf "  \033[31m✗\033[0m %s (%s passed, %s failed)\n" "$$(basename $$f)" "$$f_passed" "$$f_failed"; \
			echo "$$output" | grep '✗' | sed 's/^/    /'; \
		fi; \
		passed=$$((passed + f_passed)); failed=$$((failed + f_failed)); \
	done; \
	echo ""; echo "  $$passed passed, $$failed failed"; [ $$failed -eq 0 ]

spec-stdlib-beam: build
	@echo "Running opt-in stdlib spec suite through BEAM (-R)..."
	@failed=0; passed=0; \
	for f in spec/stdlib/*.kex; do \
		if grep -q "# kex: interpreter-only" "$$f" 2>/dev/null; then continue; fi; \
		if grep -q "# kex: skip-beam" "$$f" 2>/dev/null; then continue; fi; \
		kex_flags="-R --no-colors"; \
		if grep -q "# kex: no-check" "$$f" 2>/dev/null; then kex_flags="$$kex_flags --no-check"; fi; \
		output=$$($(KEX) $$kex_flags "$$f" 2>&1); \
		rc=$$?; \
		f_passed=$$(echo "$$output" | grep -oE '[0-9]+ passed' | grep -oE '[0-9]+'); \
		f_failed=$$(echo "$$output" | grep -oE '[0-9]+ failed' | grep -oE '[0-9]+'); \
		f_passed=$${f_passed:-0}; f_failed=$${f_failed:-0}; \
		if [ "$$rc" -eq 0 ] && [ "$$f_failed" -eq 0 ]; then \
			printf "  \033[32m✓\033[0m %s (%s passed)\n" "$$(basename $$f)" "$$f_passed"; \
		else \
			printf "  \033[31m✗\033[0m %s (%s passed, %s failed)\n" "$$(basename $$f)" "$$f_passed" "$$f_failed"; \
			echo "$$output" | grep '✗' | sed 's/^/    /'; \
		fi; \
		passed=$$((passed + f_passed)); failed=$$((failed + f_failed)); \
	done; \
	echo ""; echo "  $$passed passed, $$failed failed"; [ $$failed -eq 0 ]

parse: build
	@echo "Parsing all examples..."
	@failed=0; passed=0; \
	for f in examples/*.kex; do \
		if $(KEX) -p "$$f" > /dev/null 2>&1; then \
			printf "  \033[32m✓\033[0m %s\n" "$$(basename $$f)"; \
			passed=$$((passed + 1)); \
		else \
			printf "  \033[31m✗\033[0m %s\n" "$$(basename $$f)"; \
			$(KEX) -p "$$f" 2>&1 | head -1 | sed 's/^/    /'; \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	echo ""; \
	echo "  $$passed passing, $$failed failing"; \
	[ $$failed -eq 0 ]

repl: build
	@$(KEX)

run: build
	@$(KEX) $(F)

check: build
	@$(KEX) --check $(F)

install:
	@test -x "$(KEX)" || { echo "Missing $(KEX). Run 'make build' first."; exit 1; }
	@mkdir -p "$(BINDIR)"
	@mkdir -p "$(STDLIBDIR)"
	@install -m 755 "$(KEX)" "$(BINDIR)/kex"
	@cp -R src/stdlib/. "$(STDLIBDIR)/"
	@if [ -d "$(BUILD_DIR)/runtime/beam" ]; then \
		mkdir -p "$(PREFIX)/share/kex/runtime"; \
		install -m 644 "$(BUILD_DIR)"/runtime/beam/*.beam "$(PREFIX)/share/kex/runtime/"; \
	fi
	@echo "Installed kex to $(BINDIR) and standard library to $(STDLIBDIR)"

uninstall:
	@rm -f "$(BINDIR)/kex"
	@rm -rf "$(STDLIBDIR)"
	@rm -f "$(PREFIX)/share/kex/runtime"/*.beam
	@rmdir "$(STDLIBDIR)" 2>/dev/null || true
	@echo "Removed kex from $(BINDIR) and standard library from $(STDLIBDIR)"

clean:
	@rm -rf "$(BUILD_DIR)" "$(WASM_BUILD_DIR)" packages/kex/dist

# Tey's own suites. Run through the BEAM backend (-R) because that is how Tey
# actually runs — `tey` is compiled to .beam by the release — and because the
# tree-walker cannot currently resolve a prelude module (FS) from inside a
# `foul module` loaded via --source-root, which is a walker gap rather than
# anything about Tey.
#
# --source-root tey/src puts Tey.* on the module path without a package
# install, so the suites run straight from a checkout.
#
# Not folded into `test-all`: Tey needs erlc, and `make test` is expected to
# work on a machine that only builds the compiler.
spec-tey: build
	@echo "Running Tey spec suite through BEAM (-R)..."
	@failed=0; passed=0; \
	for f in tey/spec/*.spec.kex; do \
		output=$$($(KEX) -R --no-colors --source-root tey/src "$$f" 2>&1); \
		rc=$$?; \
		f_passed=$$(echo "$$output" | grep -oE '[0-9]+ passed' | grep -oE '[0-9]+'); \
		f_failed=$$(echo "$$output" | grep -oE '[0-9]+ failed' | grep -oE '[0-9]+'); \
		f_passed=$${f_passed:-0}; f_failed=$${f_failed:-0}; \
		if [ "$$rc" -eq 0 ] && [ "$$f_failed" -eq 0 ]; then \
			printf "  \033[32m✓\033[0m %s (%s passed)\n" "$$(basename $$f)" "$$f_passed"; \
		else \
			printf "  \033[31m✗\033[0m %s (%s passed, %s failed)\n" "$$(basename $$f)" "$$f_passed" "$$f_failed"; \
			echo "$$output" | grep '✗' | sed 's/^/    /'; \
		fi; \
		passed=$$((passed + f_passed)); failed=$$((failed + f_failed)); \
	done; \
	echo ""; echo "  $$passed passed, $$failed failed"; [ $$failed -eq 0 ]

# Compiling Tey is itself a test: it is written in Kex, so a change to the
# compiler can break it, and until this existed the FIRST thing to find out
# was the release — `make -C tey install` runs in release.yml and nowhere
# else. A Tey that does not compile stopped a release rather than a pull
# request.
build-tey: build tey-run
	@$(MAKE) -C tey build KEX=../$(KEX)

# The Tey launcher for a checkout, as opposed to an install. `tey/bin/tey` is
# the INSTALLED one: it takes the directory above itself as $PREFIX and looks
# for the .beam files under $PREFIX/lib/kex/tey/ebin, which is the layout
# `make -C tey install` creates and nothing a checkout has. Run straight from
# the tree it finds no kex_main and reports that Tey "was built with a newer
# Erlang/OTP" — a hardcoded guess at the cause, and the wrong one here.
#
# So point it at the tree instead. TEY_EBIN and TEY_KEX are both `${VAR:-...}`
# defaults inside the scripts that read them, so exporting them here wins
# without touching either, and passing either one in still overrides this.
# TEY_KEX is what makes `./tey-run` use the compiler this checkout just built
# rather than whatever `tey kex use` selected globally.
ERL ?= $(shell dir=$$(dirname "$$(command -v erlc 2>/dev/null || echo erlc)"); \
         if [ -x "$$dir/erl" ]; then echo "$$dir/erl"; else command -v erl; fi)

tey-run: tey/bin/tey Makefile
	@printf '%s\n' \
	  '#!/bin/sh' \
	  '# GENERATED by the top-level Makefile — do not edit, run `make tey-run`.' \
	  '#' \
	  '# Runs Tey out of this checkout: the .beam files `make build-tey` wrote' \
	  '# to tey/ebin, driven by the kex in build/. Both are overridable —' \
	  '# TEY_EBIN, TEY_KEX and TEY_ERL are read here and by tey/bin/tey.' \
	  'set -eu' \
	  'ROOT=$$(CDPATH= cd -- "$$(dirname -- "$$0")" && pwd)' \
	  '' \
	  'TEY_EBIN=$${TEY_EBIN:-"$$ROOT/tey/ebin"}' \
	  'if [ ! -f "$$TEY_EBIN/kex_main.beam" ]; then' \
	  '  echo "tey-run: no Tey build in $$TEY_EBIN — run: make build-tey" >&2' \
	  '  exit 1' \
	  'fi' \
	  'export TEY_EBIN' \
	  '' \
	  '# The compiler this checkout builds, not the one `tey kex use` selected.' \
	  'TEY_KEX=$${TEY_KEX:-"$$ROOT/$(KEX)"}' \
	  'if [ ! -x "$$TEY_KEX" ]; then' \
	  '  echo "tey-run: no kex at $$TEY_KEX — run: make build" >&2' \
	  '  exit 1' \
	  'fi' \
	  'export TEY_KEX' \
	  '' \
	  '# The erl beside the erlc that compiled tey/ebin: a .beam cannot be' \
	  '# loaded by an older runtime than the one that produced it.' \
	  'TEY_ERL=$${TEY_ERL:-"$(ERL)"}' \
	  'export TEY_ERL' \
	  '' \
	  'exec "$$ROOT/tey/bin/tey" "$$@"' > $@
	@chmod +x $@
	@echo "Wrote ./tey-run (TEY_EBIN=tey/ebin, TEY_KEX=$(KEX), TEY_ERL=$(ERL))"
