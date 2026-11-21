/*
 * deskControl - switches the virtual desktops of mwc (https://github.com/reygnn/mwc)
 * on Wayland, in the Motif look of mwc, mfm and pulseControl.
 *
 * One file on mtk (mtk/), the toolkit of mwc's clients, which brings the window,
 * the seat, the cursor, the colours and the bevels. It speaks ext-workspace-v1, so
 * it works with any compositor that offers it: one button per workspace, in their
 * order, in two rows. The current one is pushed in, in the active colour; one with
 * a window that wants attention is marked. It follows when something else
 * switches. It reads the same colours as mwc: MWC_INACTIVE / MWC_ACTIVE from the
 * environment, else Mwm*background and Mwm*activeBackground from
 * ~/.config/mwc/mwmrc, else the Motif defaults. The text takes Mwm*foreground and
 * Mwm*activeForeground, else black or white as Motif picks it.
 *
 * It asks for no server-side decoration and draws only mwm's thin border, which
 * takes the active colour while it has the keyboard, as mwc's frames do.
 *
 * Keys:   1 to 9        that desktop
 *         Left / Right  the one before or after, also Up / Down
 *         Home / End    the first or the last
 *         Esc / q / Ctrl+Q / Ctrl+W  quit
 * Mouse:  click a button, or turn the wheel over the window.
 *
 * In mwc it belongs on every desktop: Mwm*deskControl*allDesktops: true.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "ext-workspace-v1-client-protocol.h"
#include "mtk.h"
#include "version.h"

#define BEV MTK_BEVEL
#define PAD 6
#define GAP 4           /* between two buttons */
#define FRAME MTK_BORDER        /* mwm's border without resize handles */
#define ROWS 2          /* mwc has an even number of desktops */
#define MAX_WS 32
#ifndef MAX            /* glib has them too */
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

struct rect { int x, y, w, h; };

/* ---- state -------------------------------------------------------------- */

struct ws {
	struct ext_workspace_handle_v1 *handle;
	char name[64];
	uint32_t x, y;      /* its coordinates, if it has them; y is 0 in one dimension */
	int has_coord;
	int seq;            /* the order they came in, for those without coordinates */
	uint32_t state;
	uint32_t caps;
	struct rect r;      /* its button */
};

static struct ext_workspace_manager_v1 *ws_mgr;
static struct ws *all_ws[MAX_WS];       /* as they came, hidden ones too */
static struct ws *wss[MAX_WS];  /* the shown ones, in the order of the buttons */
static int n_all, n_ws, ws_seq;
static int have_list;           /* the first done has come */

/* Whether the compositor lets it be switched to: dimmed, passed over and not
 * pressed where it doesn't. */
static int can_activate(const struct ws *w)
{
	return w->caps & EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE;
}
static struct ws *armed;        /* the desktop the left button went down on */

static struct mtk *m;
static struct mtk_window *win;  /* NULL in tests/layout.c */
static int ptr_in, ptr_x, ptr_y;

static int W, H, btn_w;
static int focused;             /* it has the keyboard: the active frame */
static struct mtk_theme theme;
static uint32_t px_urgent;
static uint32_t xc_fg, xc_dim, xc_sel_fg, xc_urgent_fg, xc_urgent_dim, xc_err;
static cairo_t *cr;             /* what fill, bevel and text draw on */
static PangoLayout *plo;
static int font_asc, font_desc;
static int barh;

static void redraw(void)
{
	if (win)
		mtk_window_redraw(win);
}

/* ---- the theme ------------------------------------------------------------ */

static void theme_init(void)
{
	mtk_theme_load(&theme);
	uint32_t bg = theme.inactive.bg;
	/* Text as mwc's titles: the resource if set, else as Motif picks it. */
	xc_fg = theme.inactive.fg;
	xc_sel_fg = theme.active.fg;
	xc_dim = mtk_dimmed(bg, xc_fg);
	/* Attention: amber, as far from both of them as it gets. mwc has no such face
	 * and no text colour for it: black or white by WCAG, dimmed on it. */
	px_urgent = mtk_mix(bg, 0xffa000, 0.7f);
	xc_urgent_fg = mtk_best_contrast(px_urgent);
	xc_urgent_dim = mtk_dimmed(px_urgent, xc_urgent_fg);
	/* Light on dark themes; moved towards the text where a mid-tone needs it. */
	xc_err = mtk_readable(bg, mtk_best_contrast(bg) == 0xffffff ? 0xffa0a0 : 0x8a0303,
		xc_fg);

	if (plo)
		g_object_unref(plo);
	PangoFontDescription *desc = mtk_dock_font("deskControl");
	plo = mtk_layout_new(desc);
	/* Left to right as a paragraph: text_centred() places the text at the left of
	 * its box, and a right-to-left name would otherwise be flushed to its right. */
	pango_layout_set_auto_dir(plo, FALSE);
	PangoFontMetrics *fm = pango_context_get_metrics(pango_layout_get_context(plo),
		desc, NULL);
	font_asc = PANGO_PIXELS_CEIL(pango_font_metrics_get_ascent(fm));
	font_desc = PANGO_PIXELS_CEIL(pango_font_metrics_get_descent(fm));
	pango_font_metrics_unref(fm);
	pango_font_description_free(desc);
	barh = font_asc + font_desc + 10;
}

/* ---- drawing ------------------------------------------------------------ */

static void fill(uint32_t px, int x, int y, int w, int h)
{
	mtk_fill(cr, px, x, y, w, h);
}

static void bevel(struct rect r, int raised)
{
	mtk_shadow(cr, r.x, r.y, r.w, r.h, BEV, &theme.inactive, !raised);
}

static int textw(const char *s)
{
	return mtk_text_width(plo, s);
}

/* What the window says while it has no button: "..." until the compositor's list
 * has come, then why it is empty. */
static const char *placeholder(void)
{
	return have_list ? "no desktops" : "...";
}

/* s centred in r, at most its width (with "..." at the end if it is longer). */
static void text_centred(uint32_t c, struct rect r, int off, const char *s)
{
	int maxw = r.w - 2 * BEV - 4;
	int w = MIN(textw(s), maxw);
	int base = r.y + off + (r.h - (font_asc + font_desc)) / 2 + font_asc;
	pango_layout_set_text(plo, s, -1);
	pango_layout_set_width(plo, maxw * PANGO_SCALE);
	pango_layout_set_ellipsize(plo, PANGO_ELLIPSIZE_END);
	mtk_set_rgb(cr, c);
	cairo_move_to(cr, r.x + off + (r.w - w) / 2,
		base - pango_layout_get_baseline(plo) / PANGO_SCALE);
	pango_cairo_show_layout(cr, plo);
	pango_layout_set_width(plo, -1);
	pango_layout_set_ellipsize(plo, PANGO_ELLIPSIZE_NONE);
}

/* Two rows, 1 2 over 3 4, 1 2 3 over 4 5: the first row gets the odd one. Always two
 * buttons high, as high as pulseControl and digitalClock beside it, also with a single
 * desktop, whose button is centred. All as wide as the widest name needs and at least
 * a square and a half, inside the frame. The window is that size and no other. */
static void layout(void)
{
	btn_w = barh * 3 / 2;
	for (int i = 0; i < n_ws; i++)
		btn_w = MAX(btn_w, textw(wss[i]->name) + 2 * BEV + 2 * PAD);
	/* Without buttons the placeholder is drawn over the whole inside: room for it. */
	if (!n_ws)
		btn_w = MAX(btn_w, textw(placeholder()) + 2 * BEV + 4 - 2 * PAD);
	int n = MAX(n_ws, 1);
	int rows = ROWS, cols = (n + ROWS - 1) / ROWS;
	W = 2 * (FRAME + PAD) + cols * btn_w + (cols - 1) * GAP;
	H = 2 * (FRAME + PAD) + rows * barh + (rows - 1) * GAP;
	if (win)
		mtk_window_set_size(win, W, H);
	for (int i = 0; i < n_ws; i++)
		wss[i]->r = (struct rect){
			FRAME + PAD + i % cols * (btn_w + GAP),
			FRAME + PAD + i / cols * (barh + GAP), btn_w, barh };
	if (n_ws == 1)
		wss[0]->r.y += (barh + GAP) / 2;
}

static int in(struct rect r, int x, int y)
{
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void paint(struct mtk_window *w, cairo_t *c, int width, int height, void *data)
{
	cr = c;
	fill(theme.inactive.bg, 0, 0, W, H);
	mtk_border(cr, W, H, focused ? &theme.active : &theme.inactive);
	if (!n_ws) {
		struct rect all = { FRAME, FRAME, W - 2 * FRAME, H - 2 * FRAME };
		text_centred(have_list ? xc_err : xc_dim, all, 0, placeholder());
		return;
	}
	for (int i = 0; i < n_ws; i++) {
		struct ws *ws = wss[i];
		int active = ws->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE;
		int urgent = ws->state & EXT_WORKSPACE_HANDLE_V1_STATE_URGENT;
		/* Pushed in while it is current, and while the mouse holds it. */
		int down = active || (ws == armed && in(ws->r, ptr_x, ptr_y));
		uint32_t face = active ? theme.active.bg : urgent ? px_urgent :
			theme.inactive.bg;
		fill(face, ws->r.x + BEV, ws->r.y + BEV, ws->r.w - 2 * BEV, ws->r.h - 2 * BEV);
		bevel(ws->r, !down);
		uint32_t ink = active ? xc_sel_fg : urgent ? xc_urgent_fg : xc_fg;
		if (!can_activate(ws) && !active)
			ink = urgent ? xc_urgent_dim : xc_dim; /* can't be switched to */
		text_centred(ink, ws->r, down ? 1 : 0, ws->name);
	}
}

/* ---- the workspaces ------------------------------------------------------- */

static int by_place(const void *a, const void *b)
{
	const struct ws *p = *(struct ws *const *)a, *q = *(struct ws *const *)b;
	if (p->has_coord != q->has_coord)
		return q->has_coord - p->has_coord;
	/* Row by row: in two dimensions the first is x, the second y. */
	if (p->has_coord && p->y != q->y)
		return (p->y > q->y) - (p->y < q->y);
	if (p->has_coord && p->x != q->x)
		return (p->x > q->x) - (p->x < q->x);
	return (p->seq > q->seq) - (p->seq < q->seq);
}

static int current(void)
{
	for (int i = 0; i < n_ws; i++)
		if (wss[i]->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE)
			return i;
	return -1;
}

/* A single desktop is only shown: there is nothing to switch to. */
static int read_only(void)
{
	return n_ws == 1;
}

/* Whether button i can be switched to: a single desktop is only shown. */
static int switchable(int i)
{
	return i >= 0 && i < n_ws && !read_only() && can_activate(wss[i]);
}

static void activate(int i)
{
	if (!switchable(i) || !ws_mgr)
		return;
	if (wss[i]->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE)
		return;
	ext_workspace_handle_v1_activate(wss[i]->handle);
	ext_workspace_manager_v1_commit(ws_mgr);
}

/* The first button after from in direction d that can be switched to, or -1:
 * one that can't is passed over, not a wall. */
static int next_switchable(int from, int d)
{
	for (int i = from + d; i >= 0 && i < n_ws; i += d)
		if (switchable(i))
			return i;
	return -1;
}

/* The next one before or after the current one, stopping at the ends; without a
 * current one, the first or the last. */
static int step_target(int d)
{
	int c = current();
	return next_switchable(c >= 0 ? c : d > 0 ? -1 : n_ws, d);
}

static void step(int d)
{
	activate(step_target(d));
}

static void ws_id(void *data, struct ext_workspace_handle_v1 *h, const char *id)
{
}

static void ws_name(void *data, struct ext_workspace_handle_v1 *h, const char *name)
{
	struct ws *w = data;
	size_t n = strlen(name);
	if (n >= sizeof w->name) {
		/* Cut before a whole character, not inside one. */
		n = sizeof w->name - 1;
		while (n > 0 && ((unsigned char)name[n] & 0xc0) == 0x80)
			n--;
	}
	memcpy(w->name, name, n);
	w->name[n] = '\0';
}

static void ws_coordinates(void *data, struct ext_workspace_handle_v1 *h,
	struct wl_array *coordinates)
{
	struct ws *w = data;
	const uint32_t *c = coordinates->data;
	w->has_coord = coordinates->size >= sizeof *c;
	w->x = w->has_coord ? c[0] : 0;
	w->y = coordinates->size >= 2 * sizeof *c ? c[1] : 0;
}

static void ws_state(void *data, struct ext_workspace_handle_v1 *h, uint32_t state)
{
	struct ws *w = data;
	w->state = state;
}

static void ws_capabilities(void *data, struct ext_workspace_handle_v1 *h,
	uint32_t caps)
{
	struct ws *w = data;
	w->caps = caps;
}

static void ws_removed(void *data, struct ext_workspace_handle_v1 *h)
{
	struct ws *w = data;
	for (int i = 0; i < n_all; i++) {
		if (all_ws[i] == w) {
			memmove(all_ws + i, all_ws + i + 1, (n_all - i - 1) * sizeof *all_ws);
			n_all--;
			break;
		}
	}
	for (int i = 0; i < n_ws; i++) {
		if (wss[i] == w) {
			memmove(wss + i, wss + i + 1, (n_ws - i - 1) * sizeof *wss);
			n_ws--;
			break;
		}
	}
	if (armed == w)
		armed = NULL;
	ext_workspace_handle_v1_destroy(h);
	free(w);
}

static const struct ext_workspace_handle_v1_listener ws_listener = {
	.id = ws_id,
	.name = ws_name,
	.coordinates = ws_coordinates,
	.state = ws_state,
	.capabilities = ws_capabilities,
	.removed = ws_removed,
};

/* Groups say which outputs their workspaces are for. All of them are shown. */
static void group_capabilities(void *data, struct ext_workspace_group_handle_v1 *g,
	uint32_t caps)
{
}

static void group_output_enter(void *data, struct ext_workspace_group_handle_v1 *g,
	struct wl_output *output)
{
}

static void group_output_leave(void *data, struct ext_workspace_group_handle_v1 *g,
	struct wl_output *output)
{
}

static void group_workspace_enter(void *data, struct ext_workspace_group_handle_v1 *g,
	struct ext_workspace_handle_v1 *w)
{
}

static void group_workspace_leave(void *data, struct ext_workspace_group_handle_v1 *g,
	struct ext_workspace_handle_v1 *w)
{
}

static void group_removed(void *data, struct ext_workspace_group_handle_v1 *g)
{
	ext_workspace_group_handle_v1_destroy(g);
}

static const struct ext_workspace_group_handle_v1_listener group_listener = {
	.capabilities = group_capabilities,
	.output_enter = group_output_enter,
	.output_leave = group_output_leave,
	.workspace_enter = group_workspace_enter,
	.workspace_leave = group_workspace_leave,
	.removed = group_removed,
};

static void mgr_group(void *data, struct ext_workspace_manager_v1 *m,
	struct ext_workspace_group_handle_v1 *g)
{
	ext_workspace_group_handle_v1_add_listener(g, &group_listener, NULL);
}

static void mgr_workspace(void *data, struct ext_workspace_manager_v1 *m,
	struct ext_workspace_handle_v1 *h)
{
	struct ws *w = n_all < MAX_WS ? calloc(1, sizeof *w) : NULL;
	if (!w) {
		ext_workspace_handle_v1_destroy(h);
		return;
	}
	w->handle = h;
	w->seq = ws_seq++;
	snprintf(w->name, sizeof w->name, "%d", n_all + 1);
	all_ws[n_all++] = w;
	ext_workspace_handle_v1_add_listener(h, &ws_listener, w);
}

/* Everything since the last one belongs together: now it is shown. */
static void mgr_done(void *data, struct ext_workspace_manager_v1 *m)
{
	/* The spec: hidden ones should not be shown. */
	n_ws = 0;
	for (int i = 0; i < n_all; i++)
		if (!(all_ws[i]->state & EXT_WORKSPACE_HANDLE_V1_STATE_HIDDEN))
			wss[n_ws++] = all_ws[i];
	qsort(wss, n_ws, sizeof *wss, by_place);
	have_list = 1;
	layout();
	redraw();
}

static void mgr_finished(void *data, struct ext_workspace_manager_v1 *m)
{
	fprintf(stderr, "deskControl: the compositor has finished with the desktops\n");
	exit(1);
}

static const struct ext_workspace_manager_v1_listener mgr_listener = {
	.workspace_group = mgr_group,
	.workspace = mgr_workspace,
	.done = mgr_done,
	.finished = mgr_finished,
};

/* ---- input -------------------------------------------------------------- */

static int button_at(int x, int y)
{
	for (int i = 0; i < n_ws; i++)
		if (in(wss[i]->r, x, y))
			return i;
	return -1;
}

static void key(struct mtk_window *w, const struct mtk_key *k, void *data)
{
	xkb_keysym_t ks = k->sym;
	if (mtk_key_quits(ks, k->mods)) {
		mtk_quit(m, 0);
		return;
	}
	/* Alt, Super and Ctrl make a key mean something else. */
	if (k->mods & (MTK_MOD_ALT | MTK_MOD_LOGO | MTK_MOD_CTRL))
		return;
	if (ks >= XKB_KEY_1 && ks <= XKB_KEY_9) {
		activate(ks - XKB_KEY_1);
		return;
	}
	if (ks >= XKB_KEY_KP_1 && ks <= XKB_KEY_KP_9) {
		activate(ks - XKB_KEY_KP_1);
		return;
	}
	switch (ks) {
	case XKB_KEY_Left: case XKB_KEY_KP_Left:
	case XKB_KEY_Up: case XKB_KEY_KP_Up:
		step(-1);
		break;
	case XKB_KEY_Right: case XKB_KEY_KP_Right:
	case XKB_KEY_Down: case XKB_KEY_KP_Down:
		step(1);
		break;
	case XKB_KEY_Home: case XKB_KEY_KP_Home:
		activate(next_switchable(-1, 1));
		break;
	case XKB_KEY_End: case XKB_KEY_KP_End:
		activate(next_switchable(n_ws, -1));
		break;
	}
}

/* No repeat: holding Right walks to the end at once otherwise. */
static int key_repeats(struct mtk_window *w, const struct mtk_key *k, void *data)
{
	return 0;
}

static void focus(struct mtk_window *w, int on, void *data)
{
	focused = on;
	redraw();
}

static void unpress(void)
{
	if (armed) {
		armed = NULL;
		redraw();
	}
}

/* The pointer is over the desktop the press went down on. */
static int on_armed(int x, int y)
{
	int at = button_at(x, y);
	return armed && at >= 0 && wss[at] == armed;
}

/* As a Motif push button: in while the pointer is on it, out while it is off, and
 * a click counts on the release, over the button it went down on. Wheels and
 * touchpads step: down to the next desktop, up to the one before. */
static void pointer(struct mtk_window *w, const struct mtk_pointer *p, void *data)
{
	int was = on_armed(ptr_x, ptr_y);
	ptr_x = (int)p->x;
	ptr_y = (int)p->y;
	switch (p->kind) {
	case MTK_ENTER:
		ptr_in = 1;
		break;
	case MTK_LEAVE:
		ptr_in = 0;
		unpress();
		break;
	case MTK_MOTION:
		if (on_armed(ptr_x, ptr_y) != was)
			redraw();
		break;
	case MTK_PRESS:
	case MTK_RELEASE: {
		if (p->button != BTN_LEFT || !ptr_in)
			break;
		int at = button_at(ptr_x, ptr_y);
		if (p->kind == MTK_PRESS) {
			/* Not pushed in where the release can't switch. */
			armed = switchable(at) ? wss[at] : NULL;
			redraw();
			break;
		}
		/* A pointer, not an index: the list may have been re-sorted meanwhile. */
		int take = on_armed(ptr_x, ptr_y);
		unpress();
		if (take)
			activate(at);
		break;
	}
	case MTK_SCROLL:
		if (p->steps_y && !armed)
			step(p->steps_y > 0 ? 1 : -1);
		break;
	}
}

static const struct mtk_window_listener listener = {
	.paint = paint,
	.focus = focus,
	.key = key,
	.key_repeats = key_repeats,
	.pointer = pointer,
};

/* ---- main --------------------------------------------------------------- */

static void global(struct mtk *mm, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver, void *data)
{
	if (!strcmp(iface, ext_workspace_manager_v1_interface.name) && !ws_mgr) {
		ws_mgr = wl_registry_bind(r, name, &ext_workspace_manager_v1_interface, 1);
		ext_workspace_manager_v1_add_listener(ws_mgr, &mgr_listener, NULL);
	}
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		int version = argc == 2 && !strcmp(argv[1], "-v");
		/* Asked for, the usage goes to stdout, as mwc's; else to stderr. */
		int help = argc == 2 && !strcmp(argv[1], "-h");
		if (version)
			printf("deskControl %s\n", DESKCONTROL_VERSION);
		else
			fprintf(help ? stdout : stderr, "usage: deskControl [-v]\n"
				"Environment: MWC_INACTIVE, MWC_ACTIVE.\n");
		/* A full disk or a closed stdout is no success: fully buffered, the
		 * write only fails here. */
		if (fflush(stdout) == EOF || ferror(stdout)) {
			fprintf(stderr, "deskControl: stdout: %s\n", strerror(errno));
			return 1;
		}
		return version || help ? 0 : 1;
	}
	setlocale(LC_ALL, "");

	if (!(m = mtk_open(&(struct mtk_config){ .name = "deskControl", .global = global })))
		return 1;
	if (!ws_mgr) {
		fprintf(stderr, "deskControl: the compositor offers no desktops to "
			"switch (ext-workspace-v1)\n");
		return 1;
	}
	/* The font before the workspaces, which are laid out as they come; the
	 * workspaces before the window, so it opens at its size. */
	theme_init();
	struct wl_display *dpy = mtk_display(m);
	if (wl_display_roundtrip(dpy) < 0) {
		fprintf(stderr, "deskControl: lost the connection to the compositor: %s\n",
			strerror(wl_display_get_error(dpy)));
		return 1;
	}
	layout();
	win = mtk_window_new(m, &(struct mtk_window_config){
		.width = W, .height = H, .fixed = 1,
	}, &listener, NULL);
	int status = mtk_run(m);
	g_object_unref(plo);
	mtk_close(m);
	return status;
}
