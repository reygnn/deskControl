# deskControl

Project rules. Everything else: see the global CLAUDE.md, section "C projects".


## Build and test

`make` builds on the host. `make test` needs a built mwc (by default `../mwc`, else
`MWC_DIR`) with `build/vinput`, `build/pngtool`, `build/workspacec` and
`build/xdgc`, plus grim; run it in mwc's box:
`distrobox enter mwc-dev -- bash -lc 'cd ~/data/C-Projects/deskControl && make test'`.

| Target | |
|---|---|
| `make` | deskControl |
| `make layout-test` | `tests/layout.c`: `dc.c` compiled in, `layout()` and the pointer handler called for 1 to 5 desktops; runs on the host |
| `make test` | `make layout-test`, then `tests/ui.sh`: a headless mwc, simulated mouse and keyboard, log in `build/ui.log`, deskControl's own in `build/ui-deskControl.log`, its protocol log in `build/ui-wayland.log`, a failing screenshot in `build/ui-last.png` |

To check that a test fails without a fix, build the old `dc.c` elsewhere and run
`DC=/path/to/old/deskControl tests/ui.sh`.


## Rules

- One C file, `dc.c`, no toolkit. The look follows mwc: colours from `MWC_INACTIVE`,
  `MWC_ACTIVE` or mwc's mwmrc, the bevel rules of mwc's `draw.c`.
- A test client deskControl needs belongs in mwc's `tests/`, not here.
- C lines at most 100 columns, tabs as 8, as in mwc.
