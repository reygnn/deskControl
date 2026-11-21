# deskControl - switches the virtual desktops of mwc on Wayland (mwc look)

PREFIX ?= /usr/local
PKG_CONFIG ?= pkg-config
MTK_DIR = mtk

all: deskControl

include $(MTK_DIR)/mtk.mk

ifeq ($(MTK_PROTOCOLS)$(filter clean uninstall,$(MAKECMDGOALS)),)
$(error wayland-protocols 1.40 or later is needed (pkg-config finds none))
endif
# In wayland-protocols from 1.40 on.
EXT_WORKSPACE = $(MTK_PROTOCOLS)/staging/ext-workspace/ext-workspace-v1.xml
PROTO_H = build/ext-workspace-v1-client-protocol.h
PROTO_C = build/ext-workspace-v1-protocol.c

# CPPFLAGS, CFLAGS, LDFLAGS and LDLIBS stay the user's, also on the command line:
# `make CFLAGS=-O0` must not lose the include paths.
CFLAGS ?= -O2 -g
DC_CFLAGS = $(MTK_CFLAGS) -Ibuild

deskControl: dc.c version.h build/gitversion.h $(PROTO_H) $(PROTO_C) $(MTK_DEPS)
	$(CC) $(DC_CFLAGS) $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ dc.c $(PROTO_C) \
		$(MTK_SRC) $(MTK_LIBS) $(LDLIBS)

build:
	mkdir -p build

# The commit a build is from, after the version: +DATE.TIME.HASH, its commit time in
# UTC, and .dirty for changes not committed. Rewritten only when it changes, so
# deskControl is linked again only then; empty without git.
build/gitversion.h: FORCE | build
	@v=$$(TZ=UTC0 git log -1 --format=%cd.%h --date=format-local:%Y%m%d.%H%M 2>/dev/null); \
	if [ -n "$$v" ] && ! git diff --quiet HEAD 2>/dev/null; then v=$$v.dirty; fi; \
	line="#define DESKCONTROL_GIT \"$${v:++$$v}\""; \
	[ "$$(cat $@ 2>/dev/null)" = "$$line" ] || echo "$$line" >$@

FORCE:

build/ext-workspace-v1-client-protocol.h: $(EXT_WORKSPACE) | build
	$(MTK_SCANNER) client-header $(EXT_WORKSPACE) $@

build/ext-workspace-v1-protocol.c: $(EXT_WORKSPACE) | build
	$(MTK_SCANNER) private-code $(EXT_WORKSPACE) $@

install: deskControl
	install -Dm755 deskControl $(DESTDIR)$(PREFIX)/bin/deskControl
	install -d $(DESTDIR)$(PREFIX)/share/applications
	sed 's|^Exec=deskControl$$|Exec=$(PREFIX)/bin/deskControl|' data/deskControl.desktop \
		>$(DESTDIR)$(PREFIX)/share/applications/deskControl.desktop
	chmod 644 $(DESTDIR)$(PREFIX)/share/applications/deskControl.desktop

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/deskControl \
		$(DESTDIR)$(PREFIX)/share/applications/deskControl.desktop

build/layout-test: tests/layout.c dc.c version.h build/gitversion.h $(PROTO_H) $(PROTO_C) $(MTK_DEPS)
	$(CC) $(DC_CFLAGS) -I. $(CPPFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ tests/layout.c \
		$(PROTO_C) $(MTK_SRC) $(MTK_LIBS) $(LDLIBS)

# The layout alone, without a compositor.
layout-test: build/layout-test
	build/layout-test

# Needs a built mwc next to this directory, or in MWC_DIR, plus grim.
test: deskControl layout-test
	tests/ui.sh

clean:
	rm -rf deskControl build

.PHONY: all install uninstall layout-test test clean FORCE
