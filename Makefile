# deskControl - switches the virtual desktops of mwc on Wayland (mwc look)

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
PKGS = wayland-client wayland-cursor xkbcommon cairo pangocairo

WAYLAND_PROTOCOLS := $(shell $(PKG_CONFIG) --variable=pkgdatadir wayland-protocols)
WAYLAND_SCANNER := $(shell $(PKG_CONFIG) --variable=wayland_scanner wayland-scanner)
XDG_SHELL = $(WAYLAND_PROTOCOLS)/stable/xdg-shell/xdg-shell.xml
# In wayland-protocols from 1.40 on.
EXT_WORKSPACE = $(WAYLAND_PROTOCOLS)/staging/ext-workspace/ext-workspace-v1.xml

PROTO_H = build/xdg-shell-client-protocol.h build/ext-workspace-v1-client-protocol.h
PROTO_C = build/xdg-shell-protocol.c build/ext-workspace-v1-protocol.c

# CFLAGS and LDLIBS stay the user's, also on the command line: `make CFLAGS=-O0`
# must not lose the include paths.
CFLAGS ?= -O2 -g
DC_CFLAGS = -std=c11 -Wall -Wextra -Wno-unused-parameter -Ibuild \
	$(shell $(PKG_CONFIG) --cflags $(PKGS))
DC_LIBS = $(shell $(PKG_CONFIG) --libs $(PKGS))

all: deskControl

deskControl: dc.c version.h $(PROTO_H) $(PROTO_C)
	$(CC) $(DC_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ dc.c $(PROTO_C) $(DC_LIBS) $(LDLIBS)

build:
	mkdir -p build

build/xdg-shell-client-protocol.h: | build
	$(WAYLAND_SCANNER) client-header $(XDG_SHELL) $@

build/xdg-shell-protocol.c: | build
	$(WAYLAND_SCANNER) private-code $(XDG_SHELL) $@

build/ext-workspace-v1-client-protocol.h: | build
	$(WAYLAND_SCANNER) client-header $(EXT_WORKSPACE) $@

build/ext-workspace-v1-protocol.c: | build
	$(WAYLAND_SCANNER) private-code $(EXT_WORKSPACE) $@

install: deskControl
	install -Dm755 deskControl $(DESTDIR)$(PREFIX)/bin/deskControl
	mkdir -p $(DESTDIR)$(PREFIX)/share/applications
	sed 's|^Exec=deskControl$$|Exec=$(PREFIX)/bin/deskControl|' data/deskControl.desktop \
		>$(DESTDIR)$(PREFIX)/share/applications/deskControl.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/deskControl \
		$(DESTDIR)$(PREFIX)/share/applications/deskControl.desktop

# Needs a built mwc next to this directory, or in MWC_DIR, plus grim.
test: deskControl
	tests/ui.sh

clean:
	rm -rf deskControl build

.PHONY: all install uninstall test clean
