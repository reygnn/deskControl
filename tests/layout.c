/*
 * The layout and the single desktop without a compositor: dc.c compiled in, its
 * layout() and pointer handler called directly. mwc has four desktops, so
 * tests/ui.sh sees only 1 2 over 3 4.
 */
#define main dc_main
#include "../dc.c"
#undef main
#include <sys/stat.h>

static int failed;

static void expect(int ok, const char *what)
{
	printf("%s: %s\n", ok ? "ok" : "FAIL", what);
	failed |= !ok;
}

static struct ws pool[MAX_WS];

static void desktops(int n)
{
	n_all = n;
	armed = NULL;
	for (int i = 0; i < n; i++) {
		pool[i] = (struct ws){ .seq = i, .caps =
			EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE };
		snprintf(pool[i].name, sizeof pool[i].name, "%d", i + 1);
		all_ws[i] = &pool[i];
	}
	pool[0].state = EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE;
	mgr_done(NULL, NULL);
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

/* The button pressed at x, y, or -1; the press stays held. */
static int left_down(int x, int y)
{
	static int surf;
	ptr_surface = (struct wl_surface *)&surf; /* only compared to NULL */
	ptr_x = x;
	ptr_y = y;
	ptr_button(NULL, NULL, 0, 0, BTN_LEFT, WL_POINTER_BUTTON_STATE_PRESSED);
	for (int i = 0; i < n_ws; i++)
		if (wss[i] == armed)
			return i;
	return -1;
}

static int left_click(int x, int y)
{
	int was = left_down(x, y);
	armed = NULL;
	return was;
}

static int rgb(const char *s)
{
	uint32_t c;
	return parse_color(s, &c) ? (int)c : -1;
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

	/* Held on 2, the list is re-sorted under the pointer: 2 moves, the press stays
	 * with it and does not jump to whoever is under the pointer now. */
	desktops(4);
	r = wss[1]->r;
	left_down(r.x + r.w / 2, r.y + r.h / 2);
	for (int i = 0; i < 4; i++)
		pool[i].has_coord = 1, pool[i].coord = i;
	pool[1].coord = 9;
	mgr_done(NULL, NULL);
	layout();
	expect(armed == &pool[1] && wss[3] == &pool[1], "re-sorted: the press stays on 2");
	expect(!on_armed(r.x + r.w / 2, r.y + r.h / 2),
		"re-sorted: the button now under the pointer is not the pressed one");

	desktops(4);
	pool[2].state = EXT_WORKSPACE_HANDLE_V1_STATE_HIDDEN;
	mgr_done(NULL, NULL);
	layout();
	expect(n_ws == 3 && wss[2] == &pool[3], "a hidden desktop gets no button");
	pool[2].state = 0;
	mgr_done(NULL, NULL);
	expect(n_ws == 4 && wss[2] == &pool[2], "shown again, it is back in its place");

	expect(rgb("rgb:ff/80/0") == 0xff8000, "rgb:ff/80/0");
	expect(rgb("rgb:ffff/0/0") == 0xff0000, "rgb: 4 digits");
	expect(rgb("rgb: ff/00/00") == -1, "rgb: a space is refused, as in mwc");
	expect(rgb("rgb:-1/0/0") == -1, "rgb: a sign is refused");
	expect(rgb("rgb:0x1/0/0") == -1, "rgb: 0x is refused");
	expect(rgb("rgb:ff/00/00/") == -1, "rgb: a trailing / is refused");

	/* As in digitalClock, pixel for pixel; mix(bg, fg, 0.6) was 4.38:1 and 3.35:1. */
	expect(xc_dim == 0x3d3e44, "dim on the default face: 3d3e44, at least 4.5:1");
	expect(dimmed(0x7387ad, contrast(0x7387ad)) == 0x171b23, "dim on 7387ad: 171b23");

	/* text_centred() puts the text at the left of a box maxw wide: a right-to-left
	 * name must start there too, not at its right end. */
	const char *names[] = { "12", "\u05d0\u05d1", "\u0645\u0631\u062d\u0628\u0627" };
	for (int i = 0; i < 3; i++) {
		PangoRectangle ink, log;
		int w = textw(names[i]);
		pango_layout_set_text(plo, names[i], -1);
		pango_layout_set_width(plo, (w + 40) * PANGO_SCALE);
		pango_layout_get_pixel_extents(plo, &ink, &log);
		pango_layout_set_width(plo, -1);
		char what[80];
		snprintf(what, sizeof what, "name %d starts at the left of its box (%d)", i, log.x);
		expect(log.x == 0, what);
	}

	/* Too old to bind: nothing is asked of the (absent) registry. */
	registry_global(NULL, NULL, 1, wl_compositor_interface.name, 3);
	expect(!compositor, "wl_compositor 3 is not taken");

	/* 31 two-byte characters fill 62 of the 63 bytes; the 32nd does not fit whole. */
	char longname[80] = "";
	for (int i = 0; i < 32; i++)
		strcat(longname, "\u00e4");
	ws_name(&pool[0], NULL, longname);
	expect(strlen(pool[0].name) == 62 && g_utf8_validate(pool[0].name, -1, NULL),
		"a long name is cut between characters");
	ws_name(&pool[0], NULL, "1");
	expect(!strcmp(pool[0].name, "1"), "a short name is taken as it is");

	/* The text colours from mwmrc, as mwc's titles take them. A line too long for
	 * the reader ends in what looks like a setting: it must not count. */
	char dir[] = "/tmp/dc-layout-XXXXXX";
	if (mkdtemp(dir)) {
		char path[PATH_MAX];
		snprintf(path, sizeof path, "%s/mwc", dir);
		mkdir(path, 0700);
		snprintf(path, sizeof path, "%s/mwc/mwmrc", dir);
		FILE *f = fopen(path, "w");
		fprintf(f, "Mwm*foreground: #112233\nMwm*activeForeground: rgb:44/55/66\n");
		fprintf(f, "Mwm*menu: %*s\n", 600, "Mwm*background: #ff0000");
		fprintf(f, "Mwm*activeBackground: %*s\n", 600, "#123456");
		/* The last foreground is too long to keep: as in mwc, it wins and is no
		 * colour, so the text is black on the default face again. */
		fprintf(f, "Mwm*foreground: #%0300d\n", 0);
		fclose(f);
		setenv("XDG_CONFIG_HOME", dir, 1);
		theme_init();
		expect(xc_fg == 0x000000, "a too long last value counts as none, as in mwc");
		expect(xc_sel_fg == 0x445566, "Mwm*activeForeground on the current one");
		expect(px_bg == 0xaeb2c3, "the tail of a too long line is no setting");
		expect(px_sel == 0x123456, "a long line is still a setting");
		unlink(path);
		snprintf(path, sizeof path, "%s/mwc", dir);
		rmdir(path);
		rmdir(dir);
	}
	setenv("XDG_CONFIG_HOME", "/nonexistent", 1);
	theme_init();
	expect(xc_sel_fg == 0x000000, "without it, black on the active colour");
	expect(xc_err == 0x8a0303, "the default look keeps its red");
	/* Every text colour on the face it is drawn on, at least 4.5:1: all greys and
	 * some hues, as background and as active colour. */
	static const uint32_t hues[] = { 0x008080, 0x808000, 0x800000, 0x000080,
		0x7387ad, 0xaeb2c3, 0x336699, 0xff0000, 0x00ff00, 0x0000ff };
	int n_hues = sizeof hues / sizeof *hues, short_of = 0;
	char what[160] = "";
	for (int i = 0; i < 256 + n_hues; i++) {
		uint32_t c = i < 256 ? (uint32_t)i * 0x010101 : hues[i - 256];
		char hex[8];
		snprintf(hex, sizeof hex, "#%06x", c);
		setenv("MWC_INACTIVE", hex, 1);
		setenv("MWC_ACTIVE", hex, 1);
		theme_init();
		const char *bad = contrast_ratio(px_bg, xc_fg) < 4.5f ? "text" :
			contrast_ratio(px_bg, xc_dim) < 4.5f ? "dim text" :
			contrast_ratio(px_bg, xc_err) < 4.5f ? "the error" :
			contrast_ratio(px_sel, xc_sel_fg) < 4.5f ? "the current one" :
			contrast_ratio(px_urgent, xc_urgent_fg) < 4.5f ? "attention" :
			contrast_ratio(px_urgent, xc_urgent_dim) < 4.5f ? "dim attention" : NULL;
		if (bad && !short_of++)
			snprintf(what, sizeof what, ", first %s on %s", bad, hex);
	}
	unsetenv("MWC_INACTIVE");
	unsetenv("MWC_ACTIVE");
	char line[200];
	snprintf(line, sizeof line, "every text has 4.5:1 on its face (%d short%s)",
		short_of, what);
	expect(!short_of, line);

	/* Both sides of the 0x76 grey, and a dark theme from the environment. */
	expect(contrast(0x777777) == 0x000000 && contrast(0x757575) == 0xffffff,
		"black from 0x77 up, white from 0x75 down");
	setenv("MWC_INACTIVE", "#303030", 1);
	setenv("MWC_ACTIVE", "rgb:a/b/c", 1);
	theme_init();
	expect(px_bg == 0x303030 && xc_fg == 0xffffff, "MWC_INACTIVE: dark, white text");
	expect(px_sel == 0xaabbcc, "MWC_ACTIVE, rgb: with one digit");
	unsetenv("MWC_INACTIVE");
	unsetenv("MWC_ACTIVE");

	unsetenv("XCURSOR_SIZE");
	expect(cursor_size() == 24, "XCURSOR_SIZE unset: 24");
	setenv("XCURSOR_SIZE", "1000000000", 1);
	expect(cursor_size() == 256, "XCURSOR_SIZE huge: 256, no overflow with the scale");
	setenv("XCURSOR_SIZE", "32x", 1);
	expect(cursor_size() == 24, "XCURSOR_SIZE not a number: 24");
	unsetenv("XCURSOR_SIZE");

	/* At most 16: the window's and the cursor's sizes times it stay in an int. */
	expect(buffer_scale(0) == 0 && buffer_scale(-3) == 0, "a scale below 1 is ignored");
	expect(buffer_scale(1) == 1 && buffer_scale(16) == 16, "scales 1 to 16 are kept");
	expect(buffer_scale(17) == 16 && buffer_scale(INT32_MAX) == 16, "larger ones are 16");

	/* Headless mwc has one seat: the list alone, not the binding. */
	struct seats l = { 0 };
	uint32_t name, ver;
	expect(!seats_next(&l, &name, &ver), "no seat: none to take");
	seats_add(&l, 10, 7);
	seats_add(&l, 11, 5);
	seats_add(&l, 12, 9);
	expect(seats_next(&l, &name, &ver) && name == 10 && ver == 7, "the oldest seat first");
	seats_remove(&l, 99);
	expect(l.n == 3, "another global going leaves the seats");
	seats_remove(&l, 10);
	expect(seats_next(&l, &name, &ver) && name == 11 && ver == 5,
		"the first one gone: the next oldest");
	seats_remove(&l, 12);
	expect(seats_next(&l, &name, &ver) && name == 11 && l.n == 1, "one from the end goes");
	seats_remove(&l, 11);
	expect(!seats_next(&l, &name, &ver), "all gone: none to take");
	for (uint32_t i = 0; i < MAX_SEATS; i++)
		seats_add(&l, 20 + i, 1);
	expect(!seats_add(&l, 99, 1) && l.n == MAX_SEATS, "a full list keeps no more");
	seats_remove(&l, 20);
	expect(seats_add(&l, 99, 1) && l.name[MAX_SEATS - 1] == 99,
		"one gone: room for the next, at the end");

	puts(failed ? "FAIL: layout" : "PASS: layout");
	return failed;
}
