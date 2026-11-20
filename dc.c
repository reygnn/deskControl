/*
 * deskControl - switches the virtual desktops of mwc (https://github.com/reygnn/mwc)
 * on Wayland, in the Motif look of mwc, mfm and pulseControl.
 *
 * libwayland, xkbcommon, Pango and Cairo, one file, no toolkit. It speaks
 * ext-workspace-v1, so it works with any compositor that offers it: one button per
 * workspace, in their order, in two rows. The current one is pushed in, in the
 * active colour; one with a window that wants attention is marked. It follows when
 * something else switches. It reads the same colours as mwc: MWC_INACTIVE /
 * MWC_ACTIVE from the environment, else Mwm*background and Mwm*activeBackground
 * from ~/.config/mwc/mwmrc, else the Motif defaults. The text takes
 * Mwm*foreground and Mwm*activeForeground, else black or white.
 *
 * It asks for no server-side decoration and draws its own frame: only mwm's thin
 * border, no title bar, no buttons, no resize handles. The border takes the active
 * colour while it has the keyboard, as mwc's frames do.
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
#include <math.h>
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
#include "viewporter-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#define BEV 2           /* bevel width, as in mwm */
#define PAD 6
#define GAP 4           /* between two buttons */
#define FRAME (BEV + 1) /* mwm's border without resize handles, as mwc draws it */
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
static uint32_t seat_global;    /* its name in the registry */
/* The seats the compositor has announced and not removed, oldest first, by their
 * name in the registry and version: one is bound, the others wait for it to go. */
#define MAX_SEATS 8
static struct seats {
	uint32_t name[MAX_SEATS], ver[MAX_SEATS];
	int n;
} seats;
static struct xdg_wm_base *wm_base;
static struct wl_surface *surface;
static struct wl_callback *frame;       /* a frame is on its way */
static struct shmbuf *bufs[2];
static struct xdg_surface *main_xdg;
static struct xdg_toplevel *toplevel;
static int configured;          /* the first configure has arrived */
static int scale = 1;           /* buffer pixels per surface pixel */
static int need_commit;         /* a configure waits for its commit */

static struct wl_pointer *pointer;
static struct wl_surface *ptr_surface;
static int ptr_x, ptr_y;
static double axis_acc;
static int axis_steps;
static struct wl_surface *cursor_surf;
static struct wl_cursor_theme *cursor_theme;
static struct wl_cursor_image *cursor_img;
static int cursor_scale;         /* the scale the theme was loaded for */
static struct wp_viewporter *viewporter;
static struct wp_viewport *cursor_vp;
static int cursor_vw, cursor_vh;  /* its size on the window, if the viewport sets it */
static int cursor_buf_scale;     /* its buffers' scale: one that divides their size */
static uint32_t ptr_serial;      /* of the last enter, for set_cursor */
static void cursor_load(void);
static int cursor_show(void);

static struct wl_keyboard *keyboard;
static struct xkb_context *xkb;
static struct xkb_keymap *kb_map;
static struct xkb_state *kb_state;

static int W, H, btn_w, sized_w, sized_h;
static int focused;             /* it has the keyboard: the active frame */
static uint32_t px_bg, px_top, px_bot, px_sel, px_urgent;
static uint32_t xc_fg, xc_dim, xc_sel_fg, xc_urgent_fg, xc_urgent_dim, xc_err;
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

/* An sRGB channel as linear light. */
static float linear(unsigned c)
{
	float v = c / 255.0f;
	return v <= 0.04045f ? v / 12.92f : powf((v + 0.055f) / 1.055f, 2.4f);
}

/* WCAG's relative luminance. */
static float luminance(uint32_t c)
{
	return 0.2126f * linear((c >> 16) & 0xff) + 0.7152f * linear((c >> 8) & 0xff) +
		0.0722f * linear(c & 0xff);
}

/* Black or white, whichever has the higher WCAG contrast on bg. The two are equal
 * at a relative luminance of sqrt(1.05 * 0.05) - 0.05, a grey of about 0x76: a
 * mid-tone such as mwc's activeBackground gets black. */
static uint32_t contrast(uint32_t bg)
{
	float l = luminance(bg);
	return (l + 0.05f) * (l + 0.05f) > 1.05f * 0.05f ? 0x000000 : 0xffffff;
}

static float contrast_ratio(uint32_t a, uint32_t b)
{
	float la = luminance(a) + 0.05f, lb = luminance(b) + 0.05f;
	return la > lb ? la / lb : lb / la;
}

/* Dimmed towards fg, but no further than WCAG AA's 4.5:1 for small text allows; fg
 * itself if even that falls short. The same steps as digitalClock. */
static uint32_t dimmed(uint32_t bg, uint32_t fg)
{
	for (int k = 12; k < 20; k++) {
		uint32_t c = mix(bg, fg, k / 20.0f);
		if (contrast_ratio(bg, c) >= 4.5f)
			return c;
	}
	return fg;
}

/* c as it is if it has 4.5:1 on bg, else moved towards fg just as far as that
 * needs; fg itself if even that falls short. The same as pulseControl. */
static uint32_t readable(uint32_t bg, uint32_t c, uint32_t fg)
{
	for (int k = 0; k < 20; k++) {
		uint32_t m = mix(c, fg, k / 20.0f);
		if (contrast_ratio(bg, m) >= 4.5f)
			return m;
	}
	return fg;
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
			/* Only hex digits, as in mwc: strtoul alone also takes spaces,
			 * signs and 0x. */
			int digits = strspn(s, "0123456789abcdefABCDEF");
			if (digits < 1 || digits > 4 || s[digits] != (i < 2 ? '/' : '\0'))
				return 0;
			unsigned long v = strtoul(s, NULL, 16);
			unsigned long max = (1ul << (4 * digits)) - 1;
			rgb = rgb << 8 | (v * 255 + max / 2) / max;
			s += digits + 1;
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
	char path[PATH_MAX];
	const char *home = getenv("HOME"), *xdg = getenv("XDG_CONFIG_HOME");
	int len;
	if (xdg && *xdg)
		len = snprintf(path, sizeof path, "%s/mwc/mwmrc", xdg);
	else if (home && *home)
		len = snprintf(path, sizeof path, "%s/.config/mwc/mwmrc", home);
	else
		return NULL; /* as mwc: no HOME, no mwmrc, not /.config */
	if (len < 0 || (size_t)len >= sizeof path)
		return NULL;
	FILE *f = fopen(path, "r");
	if (!f)
		return NULL;
	char *found = NULL, *line = NULL;	/* whole lines, however long, as osDisplay */
	size_t kl = strlen(key), cap = 0;
	while (getline(&line, &cap, f) > 0) {
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
		/* The last line wins, as in mwc: one too long to keep is no value,
		 * not a cut one and not the line before it. */
		found = NULL;
		if ((size_t)(e - p) < n) {
			memcpy(val, p, e - p + 1);
			found = val;
		}
	}
	free(line);
	fclose(f);
	return found;
}

static int mwmrc_color(const char *res, uint32_t *out)
{
	char v[256];
	return parse_color(mwmrc_get(res, v, sizeof v), out);
}

static uint32_t setting(const char *env, const char *res, uint32_t def)
{
	uint32_t rgb;
	if (parse_color(getenv(env), &rgb) || mwmrc_color(res, &rgb))
		return rgb;
	return def;
}

/* The dock's font, the same in deskControl, pulseControl, osDisplay and
 * digitalClock, and fixed, so that the four are always as high as each other. */
#define DOCK_FONT_FAMILY "DejaVu Sans"
#define DOCK_FONT_SIZE 10       /* points */

static PangoFontDescription *dock_font(PangoContext *pctx)
{
	PangoFontDescription *d = pango_font_description_new();
	pango_font_description_set_family(d, DOCK_FONT_FAMILY);
	pango_font_description_set_style(d, PANGO_STYLE_NORMAL);
	pango_font_description_set_variant(d, PANGO_VARIANT_NORMAL);
	pango_font_description_set_weight(d, PANGO_WEIGHT_NORMAL);
	pango_font_description_set_stretch(d, PANGO_STRETCH_NORMAL);
	pango_font_description_set_size(d, DOCK_FONT_SIZE * PANGO_SCALE);
	/* Without it fontconfig quietly takes another font, and the dock may not line
	 * up; say so. */
	PangoFont *pf = pango_context_load_font(pctx, d);
	PangoFontDescription *got = pf ? pango_font_describe(pf) : NULL;
	const char *fam = got ? pango_font_description_get_family(got) : NULL;
	if (!fam || g_ascii_strcasecmp(fam, DOCK_FONT_FAMILY))
		fprintf(stderr, "deskControl: font %s not found, using %s\n",
			DOCK_FONT_FAMILY, fam ? fam : "none");
	if (got)
		pango_font_description_free(got);
	if (pf)
		g_object_unref(pf);
	return d;
}

static void theme_init(void)
{
	uint32_t bg = setting("MWC_INACTIVE", "background", 0xaeb2c3);
	uint32_t fg;
	/* Text as mwc's titles: the resource if set, else black or white. */
	if (!mwmrc_color("foreground", &fg))
		fg = contrast(bg);

	px_bg = bg;
	px_top = mix(bg, 0xffffff, 0.55f);
	px_bot = mix(bg, 0x000000, 0.5f);
	px_sel = setting("MWC_ACTIVE", "activeBackground", 0x7387ad);
	/* Attention: amber, as far from both of them as it gets. */
	px_urgent = mix(bg, 0xffa000, 0.7f);
	if (!mwmrc_color("activeForeground", &xc_sel_fg))
		xc_sel_fg = contrast(px_sel);
	xc_fg = fg;
	xc_dim = dimmed(bg, fg);
	/* mwc has no text colour for attention: black or white, dimmed on it. */
	xc_urgent_fg = contrast(px_urgent);
	xc_urgent_dim = dimmed(px_urgent, xc_urgent_fg);
	/* Light on dark themes; moved towards the text where a mid-tone needs it. */
	xc_err = readable(bg, contrast(bg) == 0xffffff ? 0xffa0a0 : 0x8a0303, fg);

	PangoContext *pctx = pango_font_map_create_context(
		pango_cairo_font_map_get_default());
	PangoFontDescription *desc = dock_font(pctx);
	plo = pango_layout_new(pctx);
	pango_layout_set_font_description(plo, desc);
	pango_layout_set_single_paragraph_mode(plo, TRUE);
	/* Left to right as a paragraph: text_centred() places the text at the left of
	 * its box, and a right-to-left name would otherwise be flushed to its right. */
	pango_layout_set_auto_dir(plo, FALSE);
	PangoFontMetrics *m = pango_context_get_metrics(pctx, desc, NULL);
	font_asc = PANGO_PIXELS_CEIL(pango_font_metrics_get_ascent(m));
	font_desc = PANGO_PIXELS_CEIL(pango_font_metrics_get_descent(m));
	pango_font_metrics_unref(m);
	pango_font_description_free(desc);
	barh = font_asc + font_desc + 10;
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

static void bevel_in(struct rect r, uint32_t t, uint32_t b)
{
	for (int i = 0; i < BEV; i++) {
		fill(t, r.x + i, r.y + i, r.w - 2 * i, 1);
		fill(t, r.x + i, r.y + i, 1, r.h - 2 * i);
		fill(b, r.x + i + 1, r.y + r.h - 1 - i, r.w - 2 * i - 1, 1);
		fill(b, r.x + r.w - 1 - i, r.y + i + 1, 1, r.h - 2 * i - 1);
	}
}

static void bevel(struct rect r, int raised)
{
	bevel_in(r, raised ? px_top : px_bot, raised ? px_bot : px_top);
}

static int textw(const char *s)
{
	int w;
	pango_layout_set_text(plo, s, -1);
	pango_layout_get_pixel_size(plo, &w, NULL);
	return w;
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
	set_rgb(c);
	cairo_move_to(cr, r.x + off + (r.w - w) / 2,
		base - pango_layout_get_baseline(plo) / PANGO_SCALE);
	pango_cairo_show_layout(cr, plo);
	pango_layout_set_width(plo, -1);
	pango_layout_set_ellipsize(plo, PANGO_ELLIPSIZE_NONE);
}

/* Two rows, 1 2 over 3 4, 1 2 3 over 4 5: the first row gets the odd one. Always two
 * buttons high, as high as pulseControl and digitalClock beside it, also with a single
 * desktop, whose button is centred. All as wide as the widest name needs and at least
 * a square and a half, inside the frame. */
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
	int want_w = 2 * (FRAME + PAD) + cols * btn_w + (cols - 1) * GAP;
	int want_h = 2 * (FRAME + PAD) + rows * barh + (rows - 1) * GAP;
	if (want_w != sized_w || want_h != sized_h) {
		/* Its size is the buttons': the window can't be made larger or smaller. */
		sized_w = want_w;
		sized_h = want_h;
		if (toplevel) {
			xdg_toplevel_set_min_size(toplevel, want_w, want_h);
			xdg_toplevel_set_max_size(toplevel, want_w, want_h);
		}
	}
	W = want_w;
	H = want_h;
	for (int i = 0; i < n_ws; i++)
		wss[i]->r = (struct rect){
			FRAME + PAD + i % cols * (btn_w + GAP),
			FRAME + PAD + i / cols * (barh + GAP), btn_w, barh };
	if (n_ws == 1)
		wss[0]->r.y += (barh + GAP) / 2;
}

/* mwm's thin border: a raised bevel and one line of the face inside it. */
static void draw_frame(void)
{
	uint32_t face = focused ? px_sel : px_bg;
	fill(face, 0, 0, W, FRAME);
	fill(face, 0, H - FRAME, W, FRAME);
	fill(face, 0, 0, FRAME, H);
	fill(face, W - FRAME, 0, FRAME, H);
	bevel_in((struct rect){ 0, 0, W, H }, mix(face, 0xffffff, 0.55f),
		mix(face, 0x000000, 0.5f));
}

static int in(struct rect r, int x, int y)
{
	return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

static void paint(void)
{
	layout();
	fill(px_bg, 0, 0, W, H);
	draw_frame();
	if (!n_ws) {
		struct rect all = { FRAME, FRAME, W - 2 * FRAME, H - 2 * FRAME };
		text_centred(have_list ? xc_err : xc_dim, all, 0, placeholder());
		return;
	}
	for (int i = 0; i < n_ws; i++) {
		struct ws *w = wss[i];
		int active = w->state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE;
		int urgent = w->state & EXT_WORKSPACE_HANDLE_V1_STATE_URGENT;
		/* Pushed in while it is current, and while the mouse holds it. */
		int down = active || (w == armed && in(w->r, ptr_x, ptr_y));
		uint32_t face = active ? px_sel : urgent ? px_urgent : px_bg;
		fill(face, w->r.x + BEV, w->r.y + BEV, w->r.w - 2 * BEV, w->r.h - 2 * BEV);
		bevel(w->r, !down);
		uint32_t ink = active ? xc_sel_fg : urgent ? xc_urgent_fg : xc_fg;
		if (!can_activate(w) && !active)
			ink = urgent ? xc_urgent_dim : xc_dim; /* can't be switched to */
		text_centred(ink, w->r, down ? 1 : 0, w->name);
	}
}

/* 0 if the compositor holds both buffers: try again after a release. */
static int draw(void)
{
	layout();
	struct shmbuf *b = get_buffer(W * scale, H * scale);
	if (!b)
		return 0;
	cr = b->cr;
	cairo_identity_matrix(cr);
	cairo_scale(cr, scale, scale);
	paint();
	cairo_surface_flush(b->cs);
	if (frame)
		wl_callback_destroy(frame);
	frame = wl_surface_frame(surface);
	wl_callback_add_listener(frame, &frame_listener, NULL);
	wl_surface_set_buffer_scale(surface, scale);
	wl_surface_attach(surface, b->wl, 0, 0);
	wl_surface_damage_buffer(surface, 0, 0, b->w, b->h);
	wl_surface_commit(surface);
	b->busy = 1;
	return 1;
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
		activate(next_switchable(-1, 1));
		break;
	case XKB_KEY_End: case XKB_KEY_KP_End:
		activate(next_switchable(n_ws, -1));
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
	/* Bounded by its size, which may or may not count a closing NUL. */
	struct xkb_keymap *map = xkb_keymap_new_from_buffer(xkb, s, strnlen(s, size),
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
	focused = 1;
	dirty = 1;
}

static void kb_leave(void *data, struct wl_keyboard *k, uint32_t serial,
	struct wl_surface *s)
{
	focused = 0;
	dirty = 1;
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
	ptr_serial = serial;
	ptr_x = wl_fixed_to_int(x);
	ptr_y = wl_fixed_to_int(y);
	cursor_load();
	cursor_show();
}

static void unpress(void)
{
	if (armed) {
		armed = NULL;
		dirty = 1;
	}
}

/* The pointer is over the desktop the press went down on. */
static int on_armed(int x, int y)
{
	int at = button_at(x, y);
	return armed && at >= 0 && wss[at] == armed;
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
	int was = on_armed(ptr_x, ptr_y);
	ptr_x = wl_fixed_to_int(x);
	ptr_y = wl_fixed_to_int(y);
	if (on_armed(ptr_x, ptr_y) != was)
		dirty = 1;
}

/* A click counts on the release, over the button it went down on. */
static void ptr_button(void *data, struct wl_pointer *p, uint32_t serial,
	uint32_t time, uint32_t code, uint32_t state)
{
	if (code != BTN_LEFT || !ptr_surface)
		return;
	int at = button_at(ptr_x, ptr_y);
	if (state == WL_POINTER_BUTTON_STATE_PRESSED) {
		/* Not pushed in where the release can't switch. */
		armed = switchable(at) ? wss[at] : NULL;
		dirty = 1;
		return;
	}
	/* A pointer, not an index: the list may have been re-sorted meanwhile. */
	int take = on_armed(ptr_x, ptr_y);
	unpress();
	if (take)
		activate(at);
}

static void ptr_frame(void *data, struct wl_pointer *p);

static void ptr_axis(void *data, struct wl_pointer *p, uint32_t time,
	uint32_t axis, wl_fixed_t value)
{
	if (axis != WL_POINTER_AXIS_VERTICAL_SCROLL)
		return;
	axis_acc += wl_fixed_to_double(value);
	/* Before wl_seat 5 no frame ends the event: each one counts by itself. */
	if (wl_pointer_get_version(p) < WL_POINTER_FRAME_SINCE_VERSION)
		ptr_frame(data, p);
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
	if (steps && !armed)
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
		/* release exists from wl_seat 3 on; an older seat only knows destroy. */
		if (wl_pointer_get_version(pointer) >= WL_POINTER_RELEASE_SINCE_VERSION)
			wl_pointer_release(pointer);
		else
			wl_pointer_destroy(pointer);
		pointer = NULL;
		ptr_surface = NULL;
	}
	if (want_kb && !keyboard) {
		keyboard = wl_seat_get_keyboard(s);
		wl_keyboard_add_listener(keyboard, &keyboard_listener, NULL);
	} else if (!want_kb && keyboard) {
		if (wl_keyboard_get_version(keyboard) >= WL_KEYBOARD_RELEASE_SINCE_VERSION)
			wl_keyboard_release(keyboard);
		else
			wl_keyboard_destroy(keyboard);
		keyboard = NULL;
		/* Gone without a leave, perhaps: the frame goes inactive all the same. */
		focused = 0;
		dirty = 1;
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

/* A seat announced; 0 if the list is full and it is not kept. */
static int seats_add(struct seats *l, uint32_t name, uint32_t ver)
{
	if (l->n == MAX_SEATS)
		return 0;
	l->name[l->n] = name;
	l->ver[l->n] = ver;
	l->n++;
	return 1;
}

/* A global removed: dropped if it is a seat in the list. */
static void seats_remove(struct seats *l, uint32_t name)
{
	for (int i = 0; i < l->n; i++)
		if (l->name[i] == name) {
			for (l->n--; i < l->n; i++) {
				l->name[i] = l->name[i + 1];
				l->ver[i] = l->ver[i + 1];
			}
			return;
		}
}

/* The seat to take, the oldest left; 0 if there is none. */
static int seats_next(const struct seats *l, uint32_t *name, uint32_t *ver)
{
	if (!l->n)
		return 0;
	*name = l->name[0];
	*ver = l->ver[0];
	return 1;
}

static void seat_bind(struct wl_registry *r, uint32_t name, uint32_t ver)
{
	seat = wl_registry_bind(r, name, &wl_seat_interface, MIN(ver, 7));
	seat_global = name;
	wl_seat_add_listener(seat, &seat_listener, NULL);
}

static void registry_global(void *data, struct wl_registry *r, uint32_t name,
	const char *iface, uint32_t ver)
{
	/* wl_surface.damage_buffer is from version 4: an older one is not taken. */
	if (!strcmp(iface, wl_compositor_interface.name) && ver >= 4) {
		compositor = wl_registry_bind(r, name, &wl_compositor_interface,
			MIN(ver, 6));
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, wl_seat_interface.name)) {
		if (seats_add(&seats, name, ver) && !seat)
			seat_bind(r, name, ver);
	} else if (!strcmp(iface, wp_viewporter_interface.name) && !viewporter) {
		viewporter = wl_registry_bind(r, name, &wp_viewporter_interface, 1);
	} else if (!strcmp(iface, xdg_wm_base_interface.name)) {
		wm_base = wl_registry_bind(r, name, &xdg_wm_base_interface,
			MIN(ver, 2));
		xdg_wm_base_add_listener(wm_base, &wm_base_listener, NULL);
	} else if (!strcmp(iface, ext_workspace_manager_v1_interface.name) && !ws_mgr) {
		ws_mgr = wl_registry_bind(r, name, &ext_workspace_manager_v1_interface, 1);
		ext_workspace_manager_v1_add_listener(ws_mgr, &mgr_listener, NULL);
	}
}

/* A seat that goes is let go of, and the oldest one left taken; with none, the
 * next one announced. */
static void registry_remove(void *data, struct wl_registry *r, uint32_t name)
{
	seats_remove(&seats, name);
	if (!seat || name != seat_global)
		return;
	seat_capabilities(NULL, seat, 0);
	if (wl_seat_get_version(seat) >= WL_SEAT_RELEASE_SINCE_VERSION)
		wl_seat_release(seat);
	else
		wl_seat_destroy(seat);
	seat = NULL;
	uint32_t next, ver;
	if (seats_next(&seats, &next, &ver))
		seat_bind(r, next, ver);
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

/* The theme's cursor at the window's scale, loaded again when that changes. */
/* XCURSOR_SIZE, 24 if unset or bad, at most 256: times the scale it must not
 * overflow. */
static int cursor_size(void)
{
	const char *s = getenv("XCURSOR_SIZE");
	char *end;
	long v = s ? strtol(s, &end, 10) : 0;
	if (!s || end == s || *end || v <= 0)
		return 24;
	return v > 256 ? 256 : (int)v;
}

static void cursor_load(void)
{
	if (cursor_scale == scale)
		return;
	/* The new theme first: if it fails, the old one and its cursor stay, and
	 * the next enter tries again. */
	struct wl_cursor_theme *theme = wl_cursor_theme_load(getenv("XCURSOR_THEME"),
		cursor_size() * scale, shm);
	struct wl_cursor *c = NULL;
	if (theme) {
		c = wl_cursor_theme_get_cursor(theme, "default");
		if (!c)
			c = wl_cursor_theme_get_cursor(theme, "left_ptr");
	}
	if (!c) {
		if (theme)
			wl_cursor_theme_destroy(theme);
		return;
	}
	struct wl_cursor_theme *old = cursor_theme;
	cursor_theme = theme;
	cursor_img = c->images[0];
	cursor_scale = scale;
	if (!cursor_surf)
		cursor_surf = wl_compositor_create_surface(compositor);
	if (viewporter && !cursor_vp)
		cursor_vp = wp_viewporter_get_viewport(viewporter, cursor_surf);
	/* A theme has only some sizes: Adwaita gives 64x64 for 72. A buffer scale
	 * that doesn't divide the size is a protocol error. With a viewport the
	 * buffer is shown at its size over the scale, as on mwc's own surfaces;
	 * without, the largest scale up to the window's that divides it, and the
	 * cursor is that much larger. */
	int w = cursor_img->width, h = cursor_img->height;
	cursor_buf_scale = 1;
	cursor_vw = cursor_vh = -1;
	if (w % scale == 0 && h % scale == 0) {
		cursor_buf_scale = scale;
	} else if (cursor_vp) {
		cursor_vw = MAX((w + scale / 2) / scale, 1);
		cursor_vh = MAX((h + scale / 2) / scale, 1);
	} else {
		for (int d = scale - 1; d > 1; d--)
			if (w % d == 0 && h % d == 0) {
				cursor_buf_scale = d;
				break;
			}
	}
	if (old) {
		/* The old theme's buffers go with it, so the surface must not show one
		 * of them: the new cursor replaces it, or, off the window, nothing. As
		 * digitalClock: it never flickers empty under the pointer. */
		if (!cursor_show()) {
			wl_surface_attach(cursor_surf, NULL, 0, 0);
			wl_surface_commit(cursor_surf);
		}
		wl_cursor_theme_destroy(old);
	}
}

/* The cursor on the window, while the pointer is over it: 1 if it was set. */
static int cursor_show(void)
{
	if (!pointer || ptr_surface != surface || !cursor_img)
		return 0;
	int d = cursor_buf_scale;
	int hx = cursor_img->hotspot_x / d, hy = cursor_img->hotspot_y / d;
	if (cursor_vw > 0) {
		/* Rounded as the viewport's size is. */
		hx = (cursor_img->hotspot_x * cursor_vw + cursor_img->width / 2) /
			cursor_img->width;
		hy = (cursor_img->hotspot_y * cursor_vh + cursor_img->height / 2) /
			cursor_img->height;
	}
	wl_pointer_set_cursor(pointer, ptr_serial, cursor_surf, hx, hy);
	if (cursor_vp)
		wp_viewport_set_destination(cursor_vp, cursor_vw, cursor_vh);
	wl_surface_set_buffer_scale(cursor_surf, d);
	wl_surface_attach(cursor_surf, wl_cursor_image_get_buffer(cursor_img), 0, 0);
	wl_surface_damage_buffer(cursor_surf, 0, 0, cursor_img->width, cursor_img->height);
	wl_surface_commit(cursor_surf);
	return 1;
}

/* Sharp on a scaled output: wl_compositor 6 says which scale the output it is on
 * has. An older compositor never does, and it stays at 1. */
static void surf_enter(void *data, struct wl_surface *s, struct wl_output *o)
{
}

static void surf_leave(void *data, struct wl_surface *s, struct wl_output *o)
{
}

/* The buffer scale for the compositor's preferred factor: at most 16, as in
 * pulseControl, so that the window's and the cursor's sizes times it cannot
 * overflow; 0 for a factor below 1, which is to be ignored. */
static int buffer_scale(int32_t factor)
{
	if (factor < 1)
		return 0;
	return factor > 16 ? 16 : factor;       /* no output is denser */
}

static void surf_preferred_scale(void *data, struct wl_surface *s, int32_t factor)
{
	factor = buffer_scale(factor);
	if (!factor || factor == scale)
		return;
	scale = factor;
	dirty = 1;
	/* Not only at the next enter: the pointer may stay where it is. */
	cursor_load();
	cursor_show();
}

static void surf_preferred_transform(void *data, struct wl_surface *s,
	uint32_t transform)
{
}

static const struct wl_surface_listener surf_listener = {
	.enter = surf_enter,
	.leave = surf_leave,
	.preferred_buffer_scale = surf_preferred_scale,
	.preferred_buffer_transform = surf_preferred_transform,
};

static void setup_window(void)
{
	surface = wl_compositor_create_surface(compositor);
	wl_surface_add_listener(surface, &surf_listener, NULL);
	main_xdg = xdg_wm_base_get_xdg_surface(wm_base, surface);
	xdg_surface_add_listener(main_xdg, &main_xdg_listener, NULL);
	toplevel = xdg_surface_get_toplevel(main_xdg);
	xdg_toplevel_add_listener(toplevel, &toplevel_listener, NULL);
	xdg_toplevel_set_app_id(toplevel, "deskControl");
	xdg_toplevel_set_title(toplevel, "deskControl");
	sized_w = sized_h = 0;
	layout();
	/* No xdg-decoration: a compositor that offers it would add a title bar. */
}

/* ---- main loop ---------------------------------------------------------- */

/* err is the cause, or 0 for the one libwayland recorded: a failed poll() leaves
 * none there. */
static void lost_connection_err(int err)
{
	fprintf(stderr, "deskControl: lost the connection to the compositor: %s\n",
		strerror(err ? err : wl_display_get_error(dpy)));
	exit(1);
}

static void lost_connection(void)
{
	lost_connection_err(0);
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
	if (wl_display_roundtrip(dpy) < 0)
		lost_connection();
	if (!compositor || !shm || !wm_base) {
		fprintf(stderr, "deskControl: the compositor lacks wl_compositor 4, "
			"wl_shm or xdg_wm_base\n");
		return 1;
	}
	if (!ws_mgr) {
		fprintf(stderr, "deskControl: the compositor offers no desktops to "
			"switch (ext-workspace-v1)\n");
		return 1;
	}
	/* The workspaces first, so the window opens at its size. */
	if (wl_display_roundtrip(dpy) < 0)
		lost_connection();
	cursor_load();
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
			/* EPIPE: the compositor has closed, but may have said why first.
			 * Reading that error makes libwayland report it. */
			if (errno != EAGAIN && errno != EPIPE) {
				wl_display_cancel_read(dpy);
				lost_connection();
			}
			if (errno == EAGAIN)
				pfd.events |= POLLOUT;
		}
		if (poll(&pfd, 1, -1) < 0 && errno != EINTR) {
			int err = errno;
			wl_display_cancel_read(dpy);
			lost_connection_err(err);
		}
		if (pfd.revents & (POLLIN | POLLERR | POLLHUP)) {
			if (wl_display_read_events(dpy) < 0)
				lost_connection();
		} else {
			wl_display_cancel_read(dpy);
		}
	}
}
