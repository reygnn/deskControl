/*
 * The layout and the single desktop without a compositor: dc.c compiled in, its
 * layout() and pointer handler called directly. mwc has four desktops, so
 * tests/ui.sh sees only 1 2 over 3 4.
 */
#define main dc_main
#include "../dc.c"
#undef main

static int failed;

static void expect(int ok, const char *what)
{
	printf("%s: %s\n", ok ? "ok" : "FAIL", what);
	failed |= !ok;
}

static struct ws pool[MAX_WS];

static void desktops(int n)
{
	n_ws = n;
	for (int i = 0; i < n; i++) {
		pool[i] = (struct ws){ .caps =
			EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE };
		snprintf(pool[i].name, sizeof pool[i].name, "%d", i + 1);
		wss[i] = &pool[i];
	}
	pool[0].state = EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE;
	layout();
}

/* How many buttons are in the row at y. */
static int in_row(int y)
{
	int k = 0;
	for (int i = 0; i < n_ws; i++)
		k += wss[i]->r.y == y;
	return k;
}

static int left_click(int x, int y)
{
	static int surf;
	ptr_surface = (struct wl_surface *)&surf; /* only compared to NULL */
	ptr_x = x;
	ptr_y = y;
	ptr_button(NULL, NULL, 0, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
	int was = pressed;
	pressed = -1;
	return was;
}

int main(void)
{
	setenv("XDG_CONFIG_HOME", "/nonexistent", 1);
	unsetenv("MWC_INACTIVE");
	unsetenv("MWC_ACTIVE");
	theme_init();
	int top = FRAME + PAD, bottom = top + barh + GAP;

	desktops(4);
	int h4 = H;
	expect(in_row(top) == 2 && in_row(bottom) == 2, "4 desktops: 2 over 2");

	desktops(5);
	expect(in_row(top) == 3 && in_row(bottom) == 2, "5 desktops: 3 over 2");
	expect(wss[2]->r.y == top && wss[3]->r.y == bottom, "5 desktops: 3 is up, 4 down");
	expect(H == h4, "5 desktops: as high as 4");

	desktops(3);
	expect(in_row(top) == 2 && in_row(bottom) == 1, "3 desktops: 2 over 1");

	desktops(1);
	struct rect r = wss[0]->r;
	expect(H == h4, "1 desktop: still two buttons high");
	/* Within a pixel: barh + GAP may be odd. */
	expect(r.x == W - r.x - r.w && abs(r.y - (H - r.y - r.h)) <= 1,
		"1 desktop: the button is centred");
	expect(left_click(r.x + r.w / 2, r.y + r.h / 2) == -1,
		"1 desktop: a press does nothing, the button is read only");

	desktops(2);
	r = wss[1]->r;
	expect(left_click(r.x + r.w / 2, r.y + r.h / 2) == 1, "2 desktops: a press takes");

	puts(failed ? "FAIL: layout" : "PASS: layout");
	return failed;
}
