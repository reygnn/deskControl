# Caveats

Limits of deskControl that are known and left as they are.


## Layout

Two rows suit an even number of desktops, which mwc has. With 3, 5 or more odd ones the
second row is one button short; a single desktop is centred between the two.

The keys walk the desktops in their order, not the grid: Down goes to the next one, not
to the one below.

Only the first nine desktops have a number key, and at most 32 get a button.

It knows one workspace group, as mwc sends it. A compositor with several, such as one
per output, gets all their desktops in one set of buttons, ordered by coordinates that
are only unique within a group: the groups are mixed, and each group's current desktop
shows as current.

It draws at the integer scale the compositor asks for with wl_compositor 6. A
fractional scale is drawn at the next integer and scaled down by the compositor; an
older compositor never asks, and there the window stays at scale 1 and is blurred.
A scale above 16 is drawn at 16.
Below wl_compositor 4 deskControl does not start.

The cursor comes in the theme's sizes only. When the scale doesn't divide that size,
a viewport shows it at its size over the scale. Without wp_viewporter it gets the
largest buffer scale that does divide it and is that much larger than elsewhere.


## Colours

Without `Mwm*foreground` and `Mwm*activeForeground` the labels are black or white by
Motif's rule, as mwc's titles. On a mid-tone that can fall short of WCAG's 4.5:1: on
mwc's activeBackground `#7387ad` the current desktop's label is white at about 3.8:1.
Dimmed labels and the error are never harder to read than that. The amber of a
desktop that wants attention is deskControl's own and keeps 4.5:1.


## Tests

mwc has four desktops, so `tests/ui.sh` sees only 1 2 over 3 4. Other numbers are
checked by `make layout-test`, without a compositor: the layout, not the drawing.
