# Caveats

Limits of deskControl that are known and left as they are.


## Layout

Two rows suit an even number of desktops, which mwc has. With an odd number the second
row is one button short.

The keys walk the desktops in their order, not the grid: Down goes to the next one, not
to the one below.

Only the first nine desktops have a number key, and at most 32 get a button.

There is no output scale: on a scaled output the window is scaled up and blurred.


## Tests

mwc has four desktops, so the test sees only the layout 1 2 over 3 4.
