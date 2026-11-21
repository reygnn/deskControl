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
	pointer(NULL, &(struct mtk_pointer){ .kind = MTK_ENTER, .x = x, .y = y }, NULL);
	pointer(NULL, &(struct mtk_pointer){ .kind = MTK_PRESS, .x = x, .y = y,
		.button = BTN_LEFT }, NULL);
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
		pool[i].has_coord = 1, pool[i].x = i;
	pool[1].x = 9;
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

	/* A 2x2 grid, announced row by row: (0,0) (1,0) (0,1) (1,1). The buttons
	 * follow it, 1 2 over 3 4, not column by column. */
	desktops(4);
	for (int i = 0; i < 4; i++)
		pool[i].has_coord = 1, pool[i].x = i % 2, pool[i].y = i / 2;
	mgr_done(NULL, NULL);
	layout();
	expect(wss[0] == &pool[0] && wss[1] == &pool[1] && wss[2] == &pool[2] &&
		wss[3] == &pool[3], "a 2x2 grid keeps its rows");
	pool[0].x = 1, pool[1].x = 0;
	mgr_done(NULL, NULL);
	expect(wss[0] == &pool[1] && wss[1] == &pool[0], "and x orders within a row");

	/* 3 of 5 can't be switched to: the arrows and Home/End pass over it, a press
	 * on it is not taken. */
	desktops(5);
	pool[2].caps = 0;
	expect(step_target(1) == 1, "from 1 the next is 2");
	pool[0].state = 0;
	pool[1].state = EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE;
	expect(step_target(1) == 3, "from 2 Right passes over 3 to 4");
	pool[1].state = 0;
	pool[3].state = EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE;
	expect(step_target(-1) == 1, "from 4 Left passes over 3 to 2");
	pool[3].state = 0;
	pool[4].state = EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE;
	expect(step_target(1) == -1, "at the last one Right stops");
	pool[0].caps = pool[4].caps = 0;
	expect(next_switchable(-1, 1) == 1 && next_switchable(n_ws, -1) == 3,
		"Home and End to the first and last that can be switched to");
	pool[4].state = 0;
	expect(step_target(1) == 1 && step_target(-1) == 3,
		"none current: the first or the last that can be");
	r = wss[2]->r;
	expect(left_click(r.x + r.w / 2, r.y + r.h / 2) == -1,
		"a press on one that can't be switched to is not taken");

	/* As in digitalClock, pixel for pixel; mix(bg, fg, 0.6) was 4.38:1. */
	expect(xc_dim == 0x3c3c3c, "dim on the default face: 3c3c3c, at least 4.5:1");

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
		expect(theme.inactive.bg == 0xaaaaaa, "the tail of a too long line is no setting");
		expect(theme.active.bg == 0x123456, "a long line is still a setting");
		unlink(path);
		snprintf(path, sizeof path, "%s/mwc", dir);
		rmdir(path);
		rmdir(dir);
	}
	setenv("XDG_CONFIG_HOME", "/nonexistent", 1);
	theme_init();
	expect(xc_sel_fg == 0x000000, "without it, black on mwc's light stone, as Motif picks");
	/* #8a0303 has 4.31:1 on the grey: moved towards the text as far as 4.5:1 needs. */
	expect(xc_err == 0x830303, "the default look's red reads");
	/* The text as Motif picks it, as mwc's titles. What is made of it reads at least
	 * as well: 4.5:1, or the text's own where that is less. Attention is a face of
	 * deskControl's own, at 4.5:1. All greys and some hues, as background and as
	 * active colour. */
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
		uint32_t bg = theme.inactive.bg;
		float need = MIN(4.5f, mtk_contrast_ratio(bg, xc_fg));
		const char *bad = xc_fg != mtk_text_color(bg) ? "text" :
			xc_sel_fg != mtk_text_color(theme.active.bg) ? "the current one" :
			mtk_contrast_ratio(bg, xc_dim) < need ? "dim text" :
			mtk_contrast_ratio(bg, xc_err) < need ? "the error" :
			mtk_contrast_ratio(px_urgent, xc_urgent_fg) < 4.5f ? "attention" :
			mtk_contrast_ratio(px_urgent, xc_urgent_dim) < 4.5f ? "dim attention" :
			NULL;
		if (bad && !short_of++)
			snprintf(what, sizeof what, ", first %s on %s", bad, hex);
	}
	unsetenv("MWC_INACTIVE");
	unsetenv("MWC_ACTIVE");
	char line[200];
	snprintf(line, sizeof line, "every text as Motif has it, or readable (%d short%s)",
		short_of, what);
	expect(!short_of, line);

	setenv("MWC_INACTIVE", "#303030", 1);
	setenv("MWC_ACTIVE", "rgb:a/b/c", 1);
	theme_init();
	expect(theme.inactive.bg == 0x303030 && xc_fg == 0xffffff,
		"MWC_INACTIVE: dark, white text");
	expect(theme.active.bg == 0xaabbcc, "MWC_ACTIVE, rgb: with one digit");
	unsetenv("MWC_INACTIVE");
	unsetenv("MWC_ACTIVE");

	/* Without a button the placeholder spans the inside, and text_centred() cuts
	 * what is wider than that: the window must be wide enough for all of it. */
	theme_init();
	for (int list = 0; list < 2; list++) {
		desktops(0);
		have_list = list;
		layout();
		int maxw = W - 2 * FRAME - 2 * BEV - 4;
		char what[80];
		snprintf(what, sizeof what, "\"%s\" fits: %d of %d", placeholder(),
			textw(placeholder()), maxw);
		expect(textw(placeholder()) <= maxw, what);
	}
	expect(!strcmp(placeholder(), "no desktops"), "with the list: no desktops");

	puts(failed ? "FAIL: layout" : "PASS: layout");
	return failed;
}
