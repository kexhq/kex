KEX ?= ../build/kex
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib/kex/tey
EBIN := ebin

# The .beam files below are produced by whichever `erlc` kex finds on PATH, and
# a BEAM file cannot be loaded by an older runtime than the one that built it.
# Bake the sibling `erl` into bin/tey so the launcher can never end up on a
# different OTP than the compiler was (see the comment in bin/tey).
ERL ?= $(shell dir=$$(dirname "$$(command -v erlc 2>/dev/null || echo erlc)"); \
         if [ -x "$$dir/erl" ]; then echo "$$dir/erl"; else command -v erl; fi)

.PHONY: build install clean

build:
	mkdir -p $(EBIN)
	$(KEX) --compile -o $(EBIN) src/main.kex

install: build
	install -d $(DESTDIR)$(LIBDIR)/ebin $(DESTDIR)$(BINDIR)
	install -m 644 $(EBIN)/*.beam $(DESTDIR)$(LIBDIR)/ebin/
	sed 's|@ERL@|$(ERL)|' bin/tey > $(EBIN)/tey.launcher
	install -m 755 $(EBIN)/tey.launcher $(DESTDIR)$(BINDIR)/tey

clean:
	rm -rf $(EBIN)
