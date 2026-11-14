# deskControl

deskControl switches virtual desktops, with one button per desktop in the look of the
Motif window manager. It is made for the dock of [mwc](https://github.com/reygnn/mwc),
beside [pulseControl](https://github.com/reygnn/pulseControl) and
[digitalClock](https://github.com/reygnn/digitalClock), and runs on any Wayland
compositor with xdg-shell and ext-workspace-v1. One C file on libwayland, xkbcommon,
Cairo and Pango, no toolkit.


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

The buttons fill two rows in the order of the desktops: 1 2 over 3 4, 1 2 3 over 4 5 6.
The window is always two buttons high, as high as pulseControl and digitalClock with the
same font, so the three line up in the dock. All buttons are as wide as the longest
name needs, and the window is as large as its buttons.

The current desktop is pushed in, in the active colour. A desktop with a window that
wants attention is marked in amber until it is switched to or the window closes. When
another program switches, deskControl follows.

It draws only mwm's thin border, no title bar, buttons or resize handles. The border
takes the active colour while deskControl has the keyboard. The colours are mwc's:
`MWC_INACTIVE` and `MWC_ACTIVE` from the environment, else `Mwm*background` and
`Mwm*activeBackground` from the mwmrc, else the Motif defaults.


## Keys

| Keys | Action |
|---|---|
| click | that desktop |
| wheel down, up | the next, the previous one |
| 1 to 9 | that desktop |
| Right, Down; Left, Up | the next, the previous one; it stops at the ends |
| Home, End | the first, the last one |
| Esc, q, Ctrl+Q, Ctrl+W | quit |

A button acts on the release: a press dragged off the button does nothing. With Alt,
Super or Ctrl held no key acts, except Ctrl+Q and Ctrl+W. Keys do not repeat.


## Configuration

| Variable | Default | |
|---|---|---|
| `DC_FONT` | `Sans 10` | Pango font name, e.g. `Sans Bold 11` |
| `MWC_INACTIVE`, `MWC_ACTIVE` | `#aeb2c3`, `#7387ad` | background, current desktop and border with the keyboard |


## How this is built

deskControl is developed with Claude (Anthropic) as a coding assistant. I set the
scope and review the changes; Claude writes most of the code and the tests. `make
test` drives deskControl in a headless mwc and checks it in mwc's state and in
screenshots. What is open is written down in CAVEATS.


## Testing

    make test

runs deskControl in a headless mwc with a simulated mouse and keyboard and checks its
border and focus, the layout, switching by click, wheel and keys, following a switch
from outside, the attention mark, and quitting. It needs a built mwc with
`build/vinput`, `build/pngtool`, `build/workspacec`, `build/xclient` and `build/xdgc`
in `MWC_DIR` (by default `../mwc`), grim and Xwayland. mwc's development box has all
of it:

    distrobox enter mwc-dev -- make test
