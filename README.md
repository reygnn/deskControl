# deskControl

deskControl switches virtual desktops, with one button per desktop in the look of the
Motif window manager. It is made for the dock of [mwc](https://github.com/reygnn/mwc),
beside [pulseControl](https://github.com/reygnn/pulseControl) and
[digitalClock](https://github.com/reygnn/digitalClock), and runs on any Wayland
compositor with xdg-shell and ext-workspace-v1. One C file on
[mtk](https://github.com/reygnn/mtk), the toolkit of mwc's clients, vendored in `mtk/`:
libwayland, xkbcommon, Cairo and Pango.


## Build

On Ubuntu 26.04:

    sudo apt install build-essential pkg-config libwayland-dev wayland-protocols \
        libxkbcommon-dev libcairo2-dev libpango1.0-dev
    make
    sudo make install

`make install` puts `deskControl` in `/usr/local/bin` and adds a desktop entry.
wayland-protocols needs to be 1.40 or later; Ubuntu 26.04 has 1.47.


## Run

In `~/.config/mwc/mwmrc`:

    Mwm*deskControl*dock:   true

and in `~/.config/mwc/autostart`:

    deskControl &

A docked window is on every desktop. Outside the dock a normal window belongs to one
desktop and would be gone after the first switch; there it needs

    Mwm*deskControl*allDesktops:   true
    Mwm*deskControl*geometry:      -0+0

the second line puts it in the top right corner.

`-v` prints the version, which is set in `version.h`.


## Look

Two rows of buttons in the order of the desktops, 1 2 over 3 4, 1 2 3 over 4 5, as high
as pulseControl and digitalClock so the three line up in the dock. A single desktop
gets one button in the middle, which does nothing. The current desktop is pushed in, in
the active colour, also after a switch from elsewhere; one with a window that wants
attention is amber. Only mwm's thin border, in the active colour while deskControl has
the keyboard. The colours are mwc's, see Configuration.


## Keys

| Keys | Action |
|---|---|
| click | that desktop |
| wheel down, up | the next, the previous one |
| 1 to 9 | that desktop |
| Right, Down; Left, Up | the next, the previous one; it stops at the ends |
| Home, End | the first, the last one |
| Esc, q, Ctrl+Q, Ctrl+W | quit |

A button acts on the release: a press dragged off the button does nothing, dragged back
it counts again. With Alt, Super or Ctrl held no key acts, except Ctrl+Q and Ctrl+W.
Keys do not repeat. Desktops the compositor marks hidden get no button; those it won't
let be switched to are dimmed, and the arrows, the wheel, Home and End pass over them.


## Configuration

| Variable | Default | |
|---|---|---|
| `MWC_INACTIVE`, `MWC_ACTIVE` | `#aeb2c3`, `#7387ad` | background, current desktop and border with the keyboard |

Without the variables it reads `Mwm*background` and `Mwm*activeBackground` from
`~/.config/mwc/mwmrc`, else the Motif defaults. The labels take `Mwm*foreground` and
`Mwm*activeForeground` from there, else black or white as Motif picks it, as mwc does.

The font is fixed as DejaVu Sans 10, the same in deskControl, pulseControl, osDisplay
and digitalClock, so the four are as high as each other in the dock. Without it
fontconfig picks another font, and deskControl says so on stderr.


## How this is built

deskControl is developed with Claude (Anthropic) as a coding assistant. I set the scope
and the design and review the changes; Claude writes most of the code and the tests, see
Testing. Known limits are written down in CAVEATS.


## Testing

    make test

runs `make layout-test`, the layout for one to five desktops, the press, the colours and
their parsing, the scale limit and the list of seats without a compositor, and then
deskControl in a headless mwc with a simulated mouse and keyboard and checks its border
and focus, the layout, switching by click, wheel and keys, following a switch from
outside, the attention mark, quitting, the sharpness at scale 2, the cursor at scales 4
and 2, and `-v`. It needs a built mwc with `build/vinput`, `build/pngtool`,
`build/workspacec` and `build/xdgc` in `MWC_DIR` (by default `../mwc`), grim and the
Adwaita cursor theme. mwc's development box has all of it:

    distrobox enter mwc-dev -- make test
