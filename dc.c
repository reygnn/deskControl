/*
 * deskControl - switches the virtual desktops of mwc (https://github.com/reygnn/mwc)
 * on Wayland, in the Motif look of mwc, mfm and pulseControl.
 *
 * libwayland, xkbcommon, Pango and Cairo, one file, no toolkit. It speaks
 * ext-workspace-v1, so it works with any compositor that offers it: one button per
 * workspace, in their order. The current one is pushed in, in the active colour;
 * one with a window that wants attention is marked. It follows when something else
 * switches. It reads the same colours as mwc: MWC_INACTIVE / MWC_ACTIVE from the
 * environment, else Mwm*background and Mwm*activeBackground from
 * ~/.config/mwc/mwmrc, else the Motif defaults.
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
#include <cairo.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <pango/pangocairo.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include <wayland-cursor.h>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "ext-workspace-v1-client-protocol.h"
#include "version.h"
#include "xdg-decoration-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define BEV 2           /* bevel width, as in mwm */
#define PAD 6
#define GAP 4           /* between two buttons */
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
	int coord, has_coord;
	int seq;            /* the order they came in, for those without coordinates */
	uint32_t state;
	uint32_t caps;
	struct rect r;      /* its button */
};

static struct ext_workspace_manager_v1 *ws_mgr;
static struct ws *wss[MAX_WS];  /* in the order of the buttons */
static int n_ws, ws_seq;
static int have_list;           /* the first done has come */
static int pressed = -1;        /* the button the left button went down on */
static int dirty = 1;

/* ---- Wayland ------------------------------------------------------------ */

/* One wl_shm buffer with a Cairo context on it. */
struct shmbuf {
	struct wl_buffer *wl;
	cairo_surface_t *cs;
	cairo_t *cr;
	void *data;
	size_t size;
	int w, h;
	int busy;       /* the compositor still reads it */
	int stale;      /* wrong size: free it as soon as it is released */
};

static struct wl_display *dpy;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct xdg_wm_base *wm_base;
static struct zxdg_decoration_manager_v1 *deco_mgr;
static struct wl_surface *surface;
static struct wl_callback *frame;       /* a frame is on its way */
static struct shmbuf *bufs[2];
static struct xdg_surface *main_xdg;
static struct xdg_toplevel *toplevel;
static int configured;          /* the first configure has arrived */
static int need_commit;         /* a configure waits for its commit */

static struct wl_pointer *pointer;
static struct wl_surface *ptr_surface;
static int ptr_x, ptr_y;
static double axis_acc;
static int axis_steps;
static struct wl_surface *cursor_surf;
static struct wl_cursor_image *cursor_img;

static struct wl_keyboard *keyboard;
static struct xkb_context *xkb;
static struct xkb_keymap *kb_map;
static struct xkb_state *kb_state;

static int W, H, btn_w, sized_w;
static uint32_t px_bg, px_top, px_bot, px_sel, px_urgent;
static uint32_t xc_fg, xc_dim, xc_err;
static cairo_t *cr;             /* what fill, bevel and text draw on */
static PangoLayout *plo;
static int font_asc, font_desc;
static int barh;

/* ---- buffers ------------------------------------------------------------ */

static void buf_free(struct shmbuf *b)
{
	cairo_destroy(b->cr);
	cairo_surface_destroy(b->cs);
	wl_buffer_destroy(b->wl);
	munmap(b->data, b->size);
	free(b);
}

static void buf_release(void *data, struct wl_buffer *wl)
{
	struct shmbuf *b = data;
	b->busy = 0;
	if (b->stale)
		buf_free(b);
}

static const struct wl_buffer_listener buf_listener = { buf_release };

static struct shmbuf *buf_new(int w, int h)
{
	int stride = cairo_format_stride_for_width(CAIRO_FORMAT_RGB24, w);
	size_t size = (size_t)stride * h;
	struct shmbuf *b = calloc(1, sizeof *b);
	int fd = memfd_create("deskControl", MFD_CLOEXEC);
	void *data = MAP_FAILED;
	if (b && fd >= 0 && ftruncate(fd, size) == 0)
		data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		fprintf(stderr, "deskControl: no memory for a %dx%d buffer: %s\n", w, h,
			strerror(errno));
		exit(1);
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	b->wl = wl_shm_pool_create_buffer(pool, 0, w, h, stride,
		WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	wl_buffer_add_listener(b->wl, &buf_listener, b);
	b->data = data;
	b->size = size;
	b->w = w;
	b->h = h;
	b->cs = cairo_image_surface_create_for_data(data, CAIRO_FORMAT_RGB24, w, h,
		stride);
	b->cr = cairo_create(b->cs);
	cairo_set_antialias(b->cr, CAIRO_ANTIALIAS_NONE);       /* not the text */
	return b;
}

/* A buffer of this size to draw on, or NULL while the compositor holds both;
 * its release wakes the main loop. */
static struct shmbuf *get_buffer(int w, int h)
{
	for (int i = 0; i < 2; i++) {
		struct shmbuf *b = bufs[i];
		if (!b || (b->w == w && b->h == h))
			continue;
		if (b->busy)
			b->stale = 1;
		else
			buf_free(b);
		bufs[i] = NULL;
	}
	for (int i = 0; i < 2; i++)
		if (bufs[i] && !bufs[i]->busy)
			return bufs[i];
	for (int i = 0; i < 2; i++)
		if (!bufs[i])
			return bufs[i] = buf_new(w, h);
	return NULL;
}

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
	wl_callback_destroy(cb);
	frame = NULL;
}

static const struct wl_callback_listener frame_listener = { frame_done };

/* ---- colours (same rules as mwm's draw.c) ------------------------------- */

static uint32_t mix(uint32_t rgb, uint32_t to, float f)
{
	uint32_t out = 0;
	for (int sh = 0; sh <= 16; sh += 8) {
		float c = (rgb >> sh) & 0xff, t = (to >> sh) & 0xff;
		out |= (uint32_t)(c + (t - c) * f + 0.5f) << sh;
	}
	return out;
}

static uint32_t contrast(uint32_t bg)
{
	unsigned r = (bg >> 16) & 0xff, g = (bg >> 8) & 0xff, b = bg & 0xff;
	return 299 * r + 587 * g + 114 * b > 140000 ? 0x000000 : 0xffffff;
}

/* What mwc reads: "#rrggbb", names like "gold", and rgb:r/g/b with 1 to 4 hex
 * digits per channel. */
static int parse_color(const char *s, uint32_t *out)
{
	PangoColor c;
	if (!s || !*s)
		return 0;
	if (!strncasecmp(s, "rgb:", 4)) {
		uint32_t rgb = 0;
		s += 4;
		for (int i = 0; i < 3; i++) {
			char *end;
			unsigned long v = strtoul(s, &end, 16);
			int digits = end - s;
			if (digits < 1 || digits > 4 || *end != (i < 2 ? '/' : '\0'))
				return 0;
			unsigned long max = (1ul << (4 * digits)) - 1;
			rgb = rgb << 8 | (v * 255 + max / 2) / max;
			s = end + 1;
		}
		*out = rgb;
		return 1;
	}
	if (!pango_color_parse(&c, s))
		return 0;
	*out = (uint32_t)(c.red >> 8) << 16 | (c.green >> 8) << 8 | (c.blue >> 8);
	return 1;
}

/* Last global "Mwm*key: value" line of ~/.config/mwc/mwmrc. */
static char *mwmrc_get(const char *key, char *val, size_t n)
{
	char path[PATH_MAX], line[512];
	const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
	int len;
	if (xdg && *xdg)
		len = snprintf(path, sizeof path, "%s/mwc/mwmrc", xdg);
	else
		len = snprintf(path, sizeof path, "%s/.config/mwc/mwmrc",
			home ? home : "");
	if (len < 0 || (size_t)len >= sizeof path)
		return NULL;
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char *found = NULL;
	size_t kl = strlen(key);
	while (fgets(line, sizeof line, f)) {
		char *p = line;
		while (isspace((unsigned char)*p))
			p++;
		/* As mwc reads it: any case, "." or "*", blanks before the ":". */
		char *colon = strchr(p, ':');
		if (!colon)
			continue;
		char *ke = colon;
		while (ke > p && isspace((unsigned char)ke[-1]))
			ke--;
		if (strncasecmp(p, "mwm", 3) || (p[3] != '*' && p[3] != '.') ||
		    (size_t)(ke - (p + 4)) != kl || strncasecmp(p + 4, key, kl))
			continue;
		p = colon + 1;
		while (isspace((unsigned char)*p))
			p++;
		char *e = p + strlen(p);
		while (e > p && isspace((unsigned char)e[-1]))
			*--e = 0;
		if ((size_t)snprintf(val, n, "%s", p) < n)
			found = val;
	}
	fclose(f);
	return found;
}

static uint32_t setting(const char *env, const char *res, uint32_t def)
{
	uint32_t rgb;
	char v[256];
	if (parse_color(getenv(env), &rgb))
		return rgb;
	if (parse_color(mwmrc_get(res, v, sizeof v), &rgb))
		return rgb;
	return def;
}

static void theme_init(void)
{
	uint32_t bg = setting("MWC_INACTIVE", "background", 0xaeb2c3);
	uint32_t fg = contrast(bg);

	px_bg = bg;
	px_top = mix(bg, 0xffffff, 0.55f);
	px_bot = mix(bg, 0x000000, 0.5f);
	px_sel = setting("MWC_ACTIVE", "activeBackground", 0x7387ad);
	/* Attention: amber, as far from both of them as it gets. */
	px_urgent = mix(bg, 0xffa000, 0.7f);
	xc_fg = fg;
	xc_dim = mix(bg, fg, 0.6f);
	xc_err = fg == 0xffffff ? 0xffa0a0 : 0x8a0303; /* readable on dark themes */

	const char *f = getenv("DC_FONT");
	PangoFontDescription *desc =
		pango_font_description_from_string(f && *f ? f : "Sans 10");
	/* "Monospace" has no size, and metrics without one are all zero. */
	if (pango_font_description_get_size(desc) <= 0)
		pango_font_description_set_size(desc, 10 * PANGO_SCALE);
	PangoContext *pctx = pango_font_map_create_context(
		pango_cairo_font_map_get_default());
	plo = pango_layout_new(pctx);
	pango_layout_set_font_description(plo, desc);
	pango_layout_set_single_paragraph_mode(plo, TRUE);
	PangoFontMetrics *m = pango_context_get_metrics(pctx, desc, NULL);
	font_asc = PANGO_PIXELS_CEIL(pango_font_metrics_get_ascent(m));
	font_desc = PANGO_PIXELS_CEIL(pango_font_metrics_get_descent(m));
	pango_font_metrics_unref(m);
	pango_font_description_free(desc);
	barh = font_asc + font_desc + 10;
	H = 2 * PAD + barh;
}

/* ---- drawing ------------------------------------------------------------ */

static void set_rgb(uint32_t c)
{
	cairo_set_source_rgb(cr, ((c >> 16) & 0xff) / 255.0,
		((c >> 8) & 0xff) / 255.0, (c & 0xff) / 255.0);
}

static void fill(uint32_t px, int x, int y, int w, int h)
{
	if (w <= 0 || h <= 0)
		return;
	set_rgb(px);
	cairo_rectangle(cr, x, y, w, h);
	cairo_fill(cr);
}

static void bevel(struct rect r, int raised)
{
	uint32_t t = raised ? px_top : px_bot;
	uint32_t b = raised ? px_bot : px_top;
	for (int i = 0; i < BEV; i++) {
		fill(t, r.x + i, r.y + i, r.w - 2 * i, 1);
		fill(t, r.x + i, r.y + i, 1, r.h - 2 * i);
		fill(b, r.x + i + 1, r.y + r.h - 1 - i, r.w - 2 * i - 1, 1);
		fill(b, r.x + r.w - 1 - i, r.y + i + 1, 1, r.h - 2 * i - 1);
	}
}

static int textw(const char *s)
{
	int w;
	pango_layout_set_text(plo, s, -1);
	pango_layout_get_pixel_size(plo, &w, NULL);
	return w;
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
	set_rgb(c);
	cairo_move_to(cr, r.x + off + (r.w - w) / 2,
		base - pango_layout_get_baseline(plo) / PANGO_SCALE);
	pango_cairo_show_layout(cr, plo);
	pango_layout_set_width(plo, -1);
	pango_layout_set_ellipsize(plo, PANGO_ELLIPSIZE_NONE);
}

/* All buttons as wide as the widest name needs, and at least a square and a half. */
static void layout(void)
{
	btn_w = barh * 3 / 2;
	for (int i = 0; i < n_ws; i++)
		btn_w = MAX(btn_w, textw(wss[i]->name) + 2 * BEV + 2 * PAD);
	int n = MAX(n_ws, 1);
	int want = 2 * PAD + n * btn_w + (n - 1) * GAP;
	if (want != sized_w) {
		/* Its size is the buttons': the window can't be made larger or smaller. */
		sized_w = want;
		if (toplevel) {
			xdg_toplevel_set_min_size(toplevel, want, H);
			xdg_toplevel_set_max_size(toplevel, want, H);
		}
	}
	W = want;
	for (int i = 0; i < n_ws; i++)
		wss[i]->r = (struct rect){ PAD + i * (btn_w + GAP), PAD, btn_w, barh };
}

static void paint(void)
{
	layout();
	fill(px_bg, 0, 0, W, H);
	if (!n_ws) {
		struct rect all = { 0, 0, W, H };
		text_centred(have_list ? xc_err : xc_dim, all, 0,
			have_list ? "no desktops" : "...");
		return;
	}
	for (int i = 0; i < n_ws; i++) {
		struct ws *w = wss[i];
		int active = w->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE;
		int urgent = w->state & EXT_WORKSPACE_HANDLE_V1_STATE_URGENT;
		/* Pushed in while it is current, and while the mouse holds it. */
		int in = active || pressed == i;
		uint32_t face = active ? px_sel : urgent ? px_urgent : px_bg;
		fill(face, w->r.x + BEV, w->r.y + BEV, w->r.w - 2 * BEV, w->r.h - 2 * BEV);
		bevel(w->r, !in);
		uint32_t ink = contrast(face);
		if (!(w->caps & EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE) &&
		    !active)
			ink = xc_dim; /* can't be switched to */
		text_centred(ink, w->r, in ? 1 : 0, w->name);
	}
}

/* 0 if the compositor holds both buffers: try again after a release. */
static int draw(void)
{
	layout();
	struct shmbuf *b = get_buffer(W, H);
	if (!b)
		return 0;
	cr = b->cr;
	paint();
	cairo_surface_flush(b->cs);
	if (frame)
		wl_callback_destroy(frame);
	frame = wl_surface_frame(surface);
	wl_callback_add_listener(frame, &frame_listener, NULL);
	wl_surface_attach(surface, b->wl, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, b->w, b->h);
	wl_surface_commit(surface);
	b->busy = 1;
	return 1;
}

/* ---- the workspaces ------------------------------------------------------- */

static int by_place(const void *a, const void *b)
{
	const struct ws *x = *(struct ws *const *)a, *y = *(struct ws *const *)b;
	if (x->has_coord != y->has_coord)
		return y->has_coord - x->has_coord;
	if (x->has_coord && x->coord != y->coord)
		return (x->coord > y->coord) - (x->coord < y->coord);
	return (x->seq > y->seq) - (x->seq < y->seq);
}

static int current(void)
{
	for (int i = 0; i < n_ws; i++)
		if (wss[i]->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE)
			return i;
	return -1;
}

static void activate(int i)
{
	if (i < 0 || i >= n_ws || !ws_mgr ||
	    !(wss[i]->caps & EXT_WORKSPACE_HANDLE_V1_WORKSPACE_CAPABILITIES_ACTIVATE))
		return;
	if (wss[i]->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE)
		return;
	ext_workspace_handle_v1_activate(wss[i]->handle);
	ext_workspace_manager_v1_commit(ws_mgr);
}

/* The one d places before or after the current one, stopping at the ends. */
static void step(int d)
{
	int c = current();
	activate(c < 0 ? (d > 0 ? 0 : n_ws - 1) : MAX(MIN(c + d, n_ws - 1), 0));
}

static void ws_id(void *data, struct ext_workspace_handle_v1 *h, const char *id)
{
}

static void ws_name(void *data, struct ext_workspace_handle_v1 *h, const char *name)
{
	struct ws *w = data;
	snprintf(w->name, sizeof w->name, "%s", name);
}

static void ws_coordinates(void *data, struct ext_workspace_handle_v1 *h,
	struct wl_array *coordinates)
{
	struct ws *w = data;
	w->has_coord = coordinates->size >= sizeof(uint32_t);
	if (w->has_coord)
		w->coord = *(uint32_t *)coordinates->data;
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
	for (int i = 0; i < n_ws; i++) {
		if (wss[i] == w) {
			memmove(wss + i, wss + i + 1, (n_ws - i - 1) * sizeof *wss);
			n_ws--;
			break;
		}
	}
	pressed = -1;
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
	struct ws *w = n_ws < MAX_WS ? calloc(1, sizeof *w) : NULL;
	if (!w) {
		ext_workspace_handle_v1_destroy(h);
		return;
	}
	w->handle = h;
	w->seq = ws_seq++;
	snprintf(w->name, sizeof w->name, "%d", n_ws + 1);
	wss[n_ws++] = w;
	ext_workspace_handle_v1_add_listener(h, &ws_listener, w);
}

/* Everything since the last one belongs together: now it is shown. */
static void mgr_done(void *data, struct ext_workspace_manager_v1 *m)
{
	qsort(wss, n_ws, sizeof *wss, by_place);
	have_list = 1;
	dirty = 1;
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

static int in(struct rect r, int x, int y)
{
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static int button_at(int x, int y)
{
	for (int i = 0; i < n_ws; i++)
		if (in(wss[i]->r, x, y))
			return i;
	return -1;
}

static int mod_on(const char *name)
{
	return xkb_state_mod_name_is_active(kb_state, name,
		XKB_STATE_MODS_EFFECTIVE) > 0;
}

/* Alt, Super and Ctrl make a key mean something else: Alt+Q must not quit. */
static int plain_key(void)
{
	return !mod_on(XKB_MOD_NAME_ALT) && !mod_on(XKB_MOD_NAME_LOGO) &&
		!mod_on(XKB_MOD_NAME_CTRL);
}

static void key_action(xkb_keysym_t ks)
{
	if (mod_on(XKB_MOD_NAME_CTRL) && !mod_on(XKB_MOD_NAME_ALT) &&
	    !mod_on(XKB_MOD_NAME_LOGO) &&
	    (ks == XKB_KEY_q || ks == XKB_KEY_Q || ks == XKB_KEY_w ||
	     ks == XKB_KEY_W))
		exit(0);
	if (!plain_key())
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
		activate(0);
		break;
	case XKB_KEY_End: case XKB_KEY_KP_End:
		activate(n_ws - 1);
		break;
	case XKB_KEY_q: case XKB_KEY_Q: case XKB_KEY_Escape:
		exit(0);
	}
}

static void kb_keymap(void *data, struct wl_keyboard *k, uint32_t format,
	int32_t fd, uint32_t size)
{
	if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1) {
		close(fd);
		return;
	}
	char *s = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
	close(fd);
	if (s == MAP_FAILED)
		return;
	struct xkb_keymap *map = xkb_keymap_new_from_string(xkb, s,
		XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
	munmap(s, size);
	struct xkb_state *state = map ? xkb_state_new(map) : NULL;
	if (!state) {
		xkb_keymap_unref(map);
		return;
	}
	xkb_state_unref(kb_state);
	xkb_keymap_unref(kb_map);
	kb_state = state;
	kb_map = map;
}

static void kb_enter(void *data, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *s, struct wl_array *keys)
{
}

static void kb_leave(void *data, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *s)
{
}

/* No repeat: holding Right walks to the end at once otherwise. */
static void kb_key(void *data, struct wl_keyboard *k, uint32_t serial,
	uint32_t time, uint32_t code, uint32_t state)
{
	if (kb_state && state == WL_KEYBOARD_KEY_STATE_PRESSED)
		key_action(xkb_state_key_get_one_sym(kb_state, code + 8));
}

static void kb_modifiers(void *data, struct wl_keyboard *k, uint32_t serial,
	uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
	if (kb_state)
		xkb_state_update_mask(kb_state, depressed, latched, locked, 0, 0,
			group);
}

static void kb_repeat_info(void *data, struct wl_keyboard *k, int32_t rate,
	int32_t delay)
{
}

static const struct wl_keyboard_listener keyboard_listener = {
	.keymap = kb_keymap,
	.enter = kb_enter,
	.leave = kb_leave,
	.key = kb_key,
	.modifiers = kb_modifiers,
	.repeat_info = kb_repeat_info,
};

static void ptr_enter(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	ptr_surface = s;
	ptr_x = wl_fixed_to_int(x);
	ptr_y = wl_fixed_to_int(y);
	if (cursor_img) {
		wl_pointer_set_cursor(p, serial, cursor_surf, cursor_img->hotspot_x,
			cursor_img->hotspot_y);
		wl_surface_attach(cursor_surf,
			wl_cursor_image_get_buffer(cursor_img), 0, 0);
		wl_surface_damage_buffer(cursor_surf, 0, 0, cursor_img->width,
			cursor_img->height);
		wl_surface_commit(cursor_surf);
	}
}

static void unpress(void)
{
	if (pressed >= 0) {
		pressed = -1;
		dirty = 1;
	}
}

static void ptr_leave(void *data, struct wl_pointer *p, uint32_t serial,
	struct wl_surface *s)
{
	ptr_surface = NULL;
	unpress();
}

/* As a Motif push button: out while the pointer is off it, in again when back. */
static void ptr_motion(void *data, struct wl_pointer *p, uint32_t time,
	wl_fixed_t x, wl_fixed_t y)
{
	ptr_x = wl_fixed_to_int(x);
	ptr_y = wl_fixed_to_int(y);
	if (pressed >= 0 && button_at(ptr_x, ptr_y) != pressed)
		unpress();
}

/* A click counts on the release, over the button it went down on. */
static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
	uint32_t time, uint32_t code, uint32_t state)
{
	if (code != BTN_LEFT || !ptr_surface)
		return;
	int at = button_at(ptr_x, ptr_y);
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		pressed = at;
		dirty = 1;
		return;
	}
	int was = pressed;
	unpress();
	if (was >= 0 && was == at)
		activate(at);
}

static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
	uint32_t axis, wl_fixed_t value)
{
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		axis_acc += wl_fixed_to_double(value);
}

static void ptr_axis_source(void *data, struct wl_pointer *p, uint32_t source)
{
}

static void ptr_axis_stop(void *data, struct wl_pointer *p, uint32_t time,
	uint32_t axis)
{
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		axis_acc = 0;
}

static void ptr_axis_discrete(void *data, struct wl_pointer *p, uint32_t axis,
	int32_t steps)
{
	if (axis == WL_POINTER_AXIS_VERTICAL_SCROLL)
		axis_steps += steps;
}

/* Wheels report steps; touchpads only a distance, of which AXIS_STEP makes one
 * wheel step. Down goes to the next desktop, up to the one before. */
static void ptr_frame(void *data, struct wl_pointer *p)
{
	enum { AXIS_STEP = 15 };
	int steps = axis_steps;
	if (steps) {
		axis_acc = 0;
	} else {
		steps = (int)(axis_acc / AXIS_STEP);
		axis_acc -= steps * AXIS_STEP;
	}
	axis_steps = 0;
	if (steps && pressed < 0)
		step(steps > 0 ? 1 : -1);
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = ptr_enter,
	.leave = ptr_leave,
	.motion = ptr_motion,
	.button = ptr_button,
	.axis = ptr_axis,
	.frame = ptr_frame,
	.axis_source = ptr_axis_source,
	.axis_stop = ptr_axis_stop,
	.axis_discrete = ptr_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *s, uint32_t caps)
{
	int want_ptr = caps & WL_SEAT_CAPABILITY_POINTER;
	int want_kb = caps & WL_SEAT_CAPABILITY_KEYBOARD;
	if (want_ptr && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
	} else if (!want_ptr && pointer) {
		unpress();
		wl_pointer_release(pointer);
		pointer = NULL;
		ptr_surface = NULL;
	}
	if (want_kb && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	} else if (!want_kb && keyboard) {
		wl_keyboard_release(keyboard);
		keyboard = NULL;
	}
}

static void seat_name(void *data, struct wl_seat *s, const char *name)
{
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* ---- window setup ------------------------------------------------------- */

static void wm_ping(void *data, struct xdg_wm_base *b, uint32_t serial)
{
	xdg_wm_base_pong(b, serial);
}

static const struct xdg_wm_base_listener wm_base_listener = { wm_ping };

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	if (!strcmp(iface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(r, name, &wl_compositor_interface,
			MIN(ver, 4));
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, wl_seat_interface.name) && !seat) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, MIN(ver, 7));
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface,
			MIN(ver, 2));
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (!strcmp(iface, zxdg_decoration_manager_v1_interface.name)) {
		deco_mgr = wl_registry_bind(r, name,
			&zxdg_decoration_manager_v1_interface, 1);
	} else if (!strcmp(iface, ext_workspace_manager_v1_interface.name) && !ws_mgr) {
		ws_mgr = wl_registry_bind(r, name, &ext_workspace_manager_v1_interface, 1);
		ext_workspace_manager_v1_add_listener(ws_mgr, &mgr_listener, NULL);
	}
}

static void registry_remove(void *data, struct wl_registry *r, uint32_t name)
{
}

static const struct wl_registry_listener registry_listener = {
	.global = registry_global,
	.global_remove = registry_remove,
};

static void main_xdg_configure(void *data, struct xdg_surface *s,
	uint32_t serial)
{
	xdg_surface_ack_configure(s, serial);
	configured = 1;
	need_commit = 1;
	dirty = 1;
}

static const struct xdg_surface_listener main_xdg_listener = {
	main_xdg_configure,
};

/* Whatever size it is offered, it keeps its own: that of its buttons. */
static void top_configure(void *data, struct xdg_toplevel *t, int32_t w,
	int32_t h, struct wl_array *states)
{
}

static void top_close(void *data, struct xdg_toplevel *t)
{
	exit(0);
}

static const struct xdg_toplevel_listener toplevel_listener = {
	.configure = top_configure,
	.close = top_close,
};

static void cursor_init(void)
{
	const char *s = getenv("XCURSOR_SIZE");
	int size = s && atoi(s) > 0 ? atoi(s) : 24;
	struct wl_cursor_theme *theme =
		wl_cursor_theme_load(getenv("XCURSOR_THEME"), size, shm);
	if (!theme)
		return;
	struct wl_cursor *c = wl_cursor_theme_get_cursor(theme, "default");
	if (!c)
		c = wl_cursor_theme_get_cursor(theme, "left_ptr");
	if (!c)
		return;
	cursor_img = c->images[0];
	cursor_surf = wl_compositor_create_surface(compositor);
}

static void setup_window(void)
{
	surface = wl_compositor_create_surface(compositor);
	main_xdg = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(main_xdg, &main_xdg_listener, NULL);
	toplevel = xdg_surface_get_toplevel(main_xdg);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_app_id(toplevel, "deskControl");
	xdg_toplevel_set_title(toplevel, "deskControl");
	sized_w = 0;
	layout();
	if (deco_mgr) {
		struct zxdg_toplevel_decoration_v1 *d =
			zxdg_decoration_manager_v1_get_toplevel_decoration(deco_mgr,
				toplevel);
		zxdg_toplevel_decoration_v1_set_mode(d,
			ZXDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
	}
}

/* ---- main loop ---------------------------------------------------------- */

static void lost_connection(void)
{
	fprintf(stderr, "deskControl: lost the connection to the compositor: %s\n",
		strerror(wl_display_get_error(dpy)));
	exit(1);
}

int main(int argc, char **argv)
{
	if (argc > 1) {
		if (!strcmp(argv[1], "-v")) {
			printf("deskControl %s\n", DESKCONTROL_VERSION);
			return 0;
		}
		int help = !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help");
		fprintf(stderr, "usage: deskControl [-v]\n"
			"Environment: DC_FONT (default \"Sans 10\"), "
			"MWC_INACTIVE, MWC_ACTIVE.\n");
		return help ? 0 : 1;
	}
	setlocale(LC_ALL, "");

	if (!(dpy = wl_display_connect(NULL))) {
		fprintf(stderr, "deskControl: cannot connect to a Wayland compositor\n");
		return 1;
	}
	if (!(xkb = xkb_context_new(XKB_CONTEXT_NO_FLAGS))) {
		fprintf(stderr, "deskControl: cannot set up xkbcommon\n");
		return 1;
	}
	struct wl_registry *registry = wl_display_get_registry(dpy);
	wl_registry_add_listener(registry, &registry_listener, NULL);
	wl_display_roundtrip(dpy);
	if (!compositor || !shm || !wm_base) {
		fprintf(stderr, "deskControl: the compositor lacks wl_compositor, wl_shm "
			"or xdg_wm_base\n");
		return 1;
	}
	if (!ws_mgr) {
		fprintf(stderr, "deskControl: the compositor offers no desktops to "
			"switch (ext-workspace-v1)\n");
		return 1;
	}
	/* The workspaces first, so the window opens at its size. */
	wl_display_roundtrip(dpy);
	cursor_init();
	theme_init();
	setup_window();
	wl_surface_commit(surface);

	for (;;) {
		if (wl_display_dispatch_pending(dpy) < 0)
			lost_connection();
		/* A frame at a time, except that a configure wants its commit even if
		 * the window is hidden and gets no frames. */
		if (dirty && configured && (!frame || need_commit) && draw()) {
			dirty = 0;
			need_commit = 0;
		}
		if (wl_display_prepare_read(dpy) != 0)
			continue;       /* events arrived in the meantime */
		struct pollfd pfd = { wl_display_get_fd(dpy), POLLIN, 0 };
		if (wl_display_flush(dpy) < 0) {
			if (errno != EAGAIN) {
				wl_display_cancel_read(dpy);
				lost_connection();
			}
			pfd.events |= POLLOUT;
		}
		if (poll(&pfd, 1, -1) < 0 && errno != EINTR) {
			wl_display_cancel_read(dpy);
			lost_connection();
		}
		if (pfd.revents & (POLLIN | POLLERR | POLLHUP)) {
			if (wl_display_read_events(dpy) < 0)
				lost_connection();
		} else {
			wl_display_cancel_read(dpy);
		}
	}
}
