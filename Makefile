# MSKman build and install rules.
PREFIX  ?= /usr/local
BINDIR  ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
MANDIR  ?= $(DATADIR)/man
DESTDIR ?=

VERSION := $(shell cat VERSION)
CC      ?= cc
CFLAGS  ?= -O2 -g
PKG_CONFIG ?= pkg-config
SDL_CFLAGS := $(shell $(PKG_CONFIG) --cflags sdl2)
SDL_LIBS   := $(shell $(PKG_CONFIG) --libs sdl2)

ALL_CFLAGS = -std=c99 -D_DEFAULT_SOURCE -Wall -Wextra $(CFLAGS)

SRC  = src/main.c src/audio.c src/mazegen.c
HDRS = src/audio.h src/font.h src/mazegen.h src/mazes.h
DISTFILES = Makefile VERSION LICENSE README.md src data debian packaging

all: mskman

mskman: $(SRC) $(HDRS) VERSION
	$(CC) $(CPPFLAGS) $(ALL_CFLAGS) -DVERSION=\"$(VERSION)\" $(SDL_CFLAGS) -o $@ $(SRC) $(LDFLAGS) $(SDL_LIBS) -lm

# Headless smoke test: plays a demo game with dummy video/audio drivers.
check: mskman
	./mskman --version
	./mskman --check-mazes 2000
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./mskman --autoplay --frames 7200
	SDL_VIDEODRIVER=dummy SDL_AUDIODRIVER=dummy ./mskman --autoplay --players 4 --level 3 --frames 7200

install: mskman
	install -Dm755 mskman $(DESTDIR)$(BINDIR)/mskman
	install -Dm644 data/mskman.desktop $(DESTDIR)$(DATADIR)/applications/mskman.desktop
	install -Dm644 data/mskman.svg $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/mskman.svg
	install -Dm644 data/mskman.6 $(DESTDIR)$(MANDIR)/man6/mskman.6

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/mskman \
	      $(DESTDIR)$(DATADIR)/applications/mskman.desktop \
	      $(DESTDIR)$(DATADIR)/icons/hicolor/scalable/apps/mskman.svg \
	      $(DESTDIR)$(MANDIR)/man6/mskman.6

dist:
	tar --exclude='*.o' -czf mskman-$(VERSION).tar.gz \
	    --transform 's,^,mskman-$(VERSION)/,' $(DISTFILES)

clean:
	rm -f mskman mskman-*.tar.gz

.PHONY: all check install uninstall dist clean
