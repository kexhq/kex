KEX ?= ../build/kex
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib/kex/tey
EBIN := ebin

.PHONY: build install clean

build:
	mkdir -p $(EBIN)
	for source in src/tey/*.kex; do $(KEX) --compile --no-check -o $(EBIN) $$source || exit; done
	$(KEX) --compile -o $(EBIN) src/main.kex
	cp $(EBIN)/main.kx.beam $(EBIN)/kex_main.beam

install: build
	install -d $(DESTDIR)$(LIBDIR)/ebin $(DESTDIR)$(BINDIR)
	install -m 644 $(EBIN)/*.beam $(DESTDIR)$(LIBDIR)/ebin/
	install -m 755 bin/tey $(DESTDIR)$(BINDIR)/tey

clean:
	rm -rf $(EBIN)
