.PHONY: build test spec spec-prelude test-all clean repl run check install uninstall help build-wasm test-wasm web-demo

BUILD_DIR = build
KEX = $(BUILD_DIR)/kex
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
STDLIBDIR ?= $(PREFIX)/share/kex/prelude

WASM_BUILD_DIR = build-wasm

help:
	@echo "Kex Language Compiler"
	@echo ""
	@echo "  make build        Build the compiler"
	@echo "  make test         Run all unit tests (C++ test binaries)"
	@echo "  make spec         Run spec programs and verify output"
	@echo "  make test-all     Run unit tests + spec suite (used by CI)"
	@echo "  make parse        Parse all examples (syntax check)"
	@echo "  make repl         Start the REPL"
	@echo "  make install      Install kex to $(BINDIR)"
	@echo "  make uninstall    Remove kex from $(BINDIR)"
	@echo "  make clean        Clean build artifacts"
	@echo "  make run F=<file>  Run a .kex file"
	@echo "  make check F=<file>  Semantic check a .kex file"
	@echo "  make build-wasm   Build the Emscripten/wasm target (requires emsdk"
	@echo "                    active, pinned per third_party/gmp-wasm/README.md,"
	@echo "                    and a prebuilt third_party/gmp-wasm/{include,lib})"
	@echo "  make test-wasm    Build the wasm target + run its test suite via Node"
	@echo "  make web-demo     Build the wasm target and serve web/index.html locally"
	@echo "                    (in-browser REPL test page) — Ctrl-C to stop"
	@echo "  make spec-beam    Run the spec suite through the BEAM backend (-R) and"
	@echo "                    report how many match the tree-walker's golden output."
	@echo "                    Informational only — never fails the build (BEAM is a"
	@echo "                    secondary backend, kept non-gating by design)."
	@echo "  make spec-wasm    Same, but through the wasm-built kex CLI via Node"
	@echo "                    (requires build-wasm; expected to match closely,"
	@echo "                    since it's the same tree-walker as native)."
	@echo ""

build:
	@cmake -B $(BUILD_DIR) -G "Unix Makefiles" 2>/dev/null | tail -1
	@cmake --build $(BUILD_DIR)

test: build
	@ctest --test-dir $(BUILD_DIR) --output-on-failure

# See third_party/gmp-wasm/README.md — requires `emsdk` active (pinned to
# 5.0.7; newer versions have a real Asyncify+exceptions regression) and a
# prebuilt third_party/gmp-wasm/{include,lib} (not checked in — rebuild
# locally per that README, or see .github/workflows/ci.yml for how CI
# builds and caches it).
build-wasm:
	@emcmake cmake -B $(WASM_BUILD_DIR) 2>/dev/null | tail -1
	@cmake --build $(WASM_BUILD_DIR)

# Only interpreter_test is run here — it's the suite this project has
# actually verified passes under wasm (145/145, matching the native suite)
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
	@python3 -m http.server 8743

test-all: test spec spec-prelude

SHELL := /bin/bash

spec: build
	@echo "Running spec programs..."
	@failed=0; passed=0; \
	for f in spec/*.kex; do \
		exp_file="$${f%.kex}.expected"; \
		if [ ! -f "$$exp_file" ]; then continue; fi; \
		kex_flags="--no-colors"; \
		if grep -q "# kex: no-check" "$$f" 2>/dev/null; then kex_flags="$$kex_flags --no-check"; fi; \
		if grep -q "# kex: check-only" "$$f" 2>/dev/null; then kex_flags="-C --no-colors"; fi; \
		if grep -q "# kex: run-beam" "$$f" 2>/dev/null; then kex_flags="-R --no-colors"; fi; \
		if grep -q "# kex: compile-run" "$$f" 2>/dev/null; then \
			tmpdir=$$(mktemp -d /tmp/kex_spec_cr_XXXXXX); \
			$(KEX) -c --no-colors -o "$$tmpdir" "$$f" > /dev/null 2>&1; \
			beamfile="$$tmpdir/$$(basename "$${f%.kex}").kx.beam"; \
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

spec-prelude-beam: build
	@echo "Running prelude spec suite through BEAM (-R)..."
	@failed=0; passed=0; \
	for f in spec/prelude/*.kex; do \
		output=$$(timeout 8 $(KEX) -R --no-check --no-colors "$$f" 2>&1); \
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
	echo "  $$passed passed, $$failed failed"

# Runs the whole spec suite through -R (BEAM) instead of the per-file tag
# system `spec` uses, and diffs against the SAME .expected golden files —
# any mismatch means the two backends produce different output for that
# program. Skips check-only specs (those are about semantic checking, not
# runtime execution — same exclusion `spec` doesn't need since it already
# dispatches per-tag). Informational only — never fails the build (BEAM is a
# secondary backend, kept non-gating by design).
# Strings are UTF-8 binaries and Chars are tagged {'Char', N}
# tuples on BEAM, so the old charlist ambiguities ([] vs "", [Int] vs
# String, Char vs Int) are gone.
spec-beam: build
	@echo "Running spec suite through BEAM (-R)..."
	@failed=0; passed=0; \
	for f in spec/*.kex; do \
		exp_file="$${f%.kex}.expected"; \
		if [ ! -f "$$exp_file" ]; then continue; fi; \
		if grep -q "# kex: check-only" "$$f" 2>/dev/null; then continue; fi; \
		actual=$$(timeout 8 $(KEX) -R --no-colors "$$f" 2>&1); \
		expected=$$(cat "$$exp_file"); \
		if [ "$$actual" = "$$expected" ]; then \
			printf "  \033[32m✓\033[0m %s\n" "$$(basename $$f)"; \
			passed=$$((passed + 1)); \
		else \
			printf "  \033[33m~\033[0m %s (BEAM output differs)\n" "$$(basename $$f)"; \
			failed=$$((failed + 1)); \
		fi; \
	done; \
	echo ""; \
	echo "  $$passed matching, $$failed differing (informational — not a build failure)"

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
		if grep -q "# kex: run-beam" "$$f" 2>/dev/null; then continue; fi; \
		actual=$$(timeout 8 node $(WASM_BUILD_DIR)/kex.js $$kex_flags "$$f" 2>&1); \
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
	@install -m 644 src/prelude/*.kex "$(STDLIBDIR)/"
	@if [ -d "$(BUILD_DIR)/runtime/beam" ]; then \
		mkdir -p "$(PREFIX)/share/kex/runtime"; \
		install -m 644 "$(BUILD_DIR)"/runtime/beam/*.beam "$(PREFIX)/share/kex/runtime/"; \
	fi
	@echo "Installed kex to $(BINDIR)/kex and stdlib to $(STDLIBDIR)"

uninstall:
	@rm -f "$(BINDIR)/kex"
	@rm -f "$(STDLIBDIR)"/*.kex
	@rm -f "$(PREFIX)/share/kex/runtime"/*.beam
	@rmdir "$(STDLIBDIR)" 2>/dev/null || true
	@echo "Removed kex from $(BINDIR) and stdlib from $(STDLIBDIR)"

clean:
	@rm -rf $(BUILD_DIR)
