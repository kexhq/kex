.PHONY: build test spec clean repl run check help

BUILD_DIR = build
KEX = $(BUILD_DIR)/kex

help:
	@echo "Kex Language Compiler"
	@echo ""
	@echo "  make build    Build the compiler"
	@echo "  make test     Run all unit tests"
	@echo "  make spec     Run spec programs and verify output"
	@echo "  make parse    Parse all examples (syntax check)"
	@echo "  make repl     Start the REPL"
	@echo "  make clean    Clean build artifacts"
	@echo "  make run F=<file>  Run a .kex file"
	@echo "  make check F=<file>  Semantic check a .kex file"
	@echo ""

build:
	@cmake -B $(BUILD_DIR) -G "Unix Makefiles" 2>/dev/null | tail -1
	@cmake --build $(BUILD_DIR)

test: build
	@ctest --test-dir $(BUILD_DIR) --output-on-failure

SHELL := /bin/bash

spec: build
	@echo "Running spec programs..."
	@failed=0; passed=0; \
	for f in spec/*.kex; do \
		exp_file="$${f%.kex}.expected"; \
		if [ ! -f "$$exp_file" ]; then continue; fi; \
		actual=$$($(KEX) "$$f" 2>&1); \
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

clean:
	@rm -rf $(BUILD_DIR)
