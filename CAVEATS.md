# Caveats

Limits of deskControl that are known and left as they are.


## Layout

Two rows suit an even number of desktops, which mwc has. With 3, 5 or more odd ones the
second row is one button short; a single desktop is centred between the two.

The keys walk the desktops in their order, not the grid: Down goes to the next one, not
to the one below.

Only the first nine desktops have a number key, and at most 32 get a button.

It draws at the integer scale the compositor asks for with wl_compositor 6. A
fractional scale is drawn at the next integer and scaled down by the compositor; an
older compositor never asks, and there the window stays at scale 1 and is blurred.
Below wl_compositor 4 deskControl does not start, below wl_seat 3 it takes no input.

The cursor comes in the theme's sizes only. When the scale doesn't divide that size,
a viewport shows it at its size over the scale. Without wp_viewporter it gets the
largest buffer scale that does divide it and is that much larger than elsewhere.


## Tests

mwc has four desktops, so `tests/ui.sh` sees only 1 2 over 3 4. Other numbers are
checked by `make layout-test`, without a compositor: the layout, not the drawing.
