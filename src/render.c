#include "render.h"
#include "output.h"
#include "server.h"
#include "blur.h"
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <strings.h>
#include <pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/pass.h>
#include <wlr/render/gles2.h>
#include <wlr/render/egl.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/util/box.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <drm_fourcc.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/dmabuf.h>
#include <wlr/types/wlr_buffer.h>

#define MAX_BLUR_RECTS 64
#define BLUR_MODE_ALL 0
#define BLUR_MODE_SKIP 1
#define BLUR_MODE_ONLY 2

static BlurContext g_blur = {0};
static bool g_blur_tried = false;

static struct wlr_buffer *g_backdrop = NULL;
static int g_backdrop_w = 0;
static int g_backdrop_h = 0;

static struct wlr_buffer *g_blurred = NULL;
static int g_blurred_w = 0;
static int g_blurred_h = 0;

static struct wlr_scene_rect *g_blur_borders[MAX_BLUR_RECTS * 4];
static int g_blur_border_count = 0;

typedef struct GLScope {
	EGLDisplay old_dpy;
	EGLContext old_ctx;
	EGLSurface old_draw;
	EGLSurface old_read;
	bool restore;
} GLScope;

static bool gl_scope_begin(Server *server, GLScope *scope) {
	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(server->renderer);
	if (!egl) {
		return false;
	}

	EGLDisplay egl_dpy = wlr_egl_get_display(egl);
	EGLContext egl_ctx = wlr_egl_get_context(egl);
	if (egl_dpy == EGL_NO_DISPLAY || egl_ctx == EGL_NO_CONTEXT) {
		return false;
	}

	scope->old_dpy = eglGetCurrentDisplay();
	scope->old_ctx = eglGetCurrentContext();
	scope->old_draw = eglGetCurrentSurface(EGL_DRAW);
	scope->old_read = eglGetCurrentSurface(EGL_READ);
	scope->restore = scope->old_dpy != EGL_NO_DISPLAY && scope->old_ctx != EGL_NO_CONTEXT;

	if (!eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx)) {
		scope->restore = false;
		return false;
	}

	return true;
}

static void gl_scope_end(Server *server, GLScope *scope) {
	if (scope->restore) {
		eglMakeCurrent(scope->old_dpy, scope->old_draw, scope->old_read, scope->old_ctx);
		return;
	}

	struct wlr_egl *egl = wlr_gles2_renderer_get_egl(server->renderer);
	if (egl) {
		eglMakeCurrent(
			wlr_egl_get_display(egl),
			EGL_NO_SURFACE,
			EGL_NO_SURFACE,
			wlr_egl_get_context(egl)
		);
	}
}

typedef struct FrameTextureList {
	struct wlr_texture **items;
	size_t count;
	size_t capacity;
} FrameTextureList;

typedef struct FrameSurfaceList {
	struct wlr_surface **items;
	size_t count;
	size_t capacity;
} FrameSurfaceList;

static void ftl_push(FrameTextureList *list, struct wlr_texture *texture) {
	if (!list || !texture) {
		return;
	}

	if (list->count == list->capacity) {
		size_t cap = list->capacity ? list->capacity * 2 : 16;
		struct wlr_texture **items = realloc(list->items, cap * sizeof(*items));
		if (!items) {
			return;
		}
		list->items = items;
		list->capacity = cap;
	}

	list->items[list->count++] = texture;
}

static void ftl_finish(FrameTextureList *list) {
	if (!list) {
		return;
	}

	for (size_t i = 0; i < list->count; i++) {
		if (list->items[i]) {
			wlr_texture_destroy(list->items[i]);
		}
	}

	free(list->items);
	list->items = NULL;
	list->count = 0;
	list->capacity = 0;
}

static void fsl_push(FrameSurfaceList *list, struct wlr_surface *surface) {
	if (!list || !surface) {
		return;
	}

	if (list->count == list->capacity) {
		size_t cap = list->capacity ? list->capacity * 2 : 32;
		struct wlr_surface **items = realloc(list->items, cap * sizeof(*items));
		if (!items) {
			return;
		}
		list->items = items;
		list->capacity = cap;
	}

	list->items[list->count++] = surface;
}

static void fsl_finish(FrameSurfaceList *list) {
	if (!list) {
		return;
	}

	free(list->items);
	list->items = NULL;
	list->count = 0;
	list->capacity = 0;
}

static View *view_from_surface(Server *server, struct wlr_surface *surface) {
	if (!server || !surface) {
		return NULL;
	}

	struct wlr_surface *root = wlr_surface_get_root_surface(surface);
	if (!root) {
		root = surface;
	}

	View *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->type == VIEW_TYPE_XDG &&
			view->xdg.xdg_surface &&
			view->xdg.xdg_surface->surface == root) {
			return view;
		}
#if defined(WYO_HAS_XWAYLAND)
		if (view->type == VIEW_TYPE_XWAYLAND &&
			view->xwayland.xsurface &&
			view->xwayland.xsurface->surface == root) {
			return view;
		}
#endif
	}

	return NULL;
}

static bool blur_match_string(const char *pattern, const char *value) {
	if (!pattern || !value || !*pattern || !*value) {
		return false;
	}

	if (strcmp(pattern, "*") == 0) {
		return true;
	}

	return strcasecmp(pattern, value) == 0;
}

static const ConfigBlurRule *surface_blur_rule(Server *server, struct wlr_surface *surface) {
	if (!server || !surface || server->config.blur_rule_count <= 0) {
		return NULL;
	}

	struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(surface);
	if (xdg && xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && xdg->toplevel) {
		for (int i = 0; i < server->config.blur_rule_count; i++) {
			const ConfigBlurRule *rule = &server->config.blur_rules[i];

			if (rule->type == BLUR_MATCH_APP_ID) {
				if (blur_match_string(rule->pattern, xdg->toplevel->app_id)) {
					return rule;
				}
			} else if (rule->type == BLUR_MATCH_TITLE) {
				if (blur_match_string(rule->pattern, xdg->toplevel->title)) {
					return rule;
				}
			}
		}
	}

	struct wlr_layer_surface_v1 *ls = wlr_layer_surface_v1_try_from_wlr_surface(surface);
	if (ls) {
		for (int i = 0; i < server->config.blur_rule_count; i++) {
			const ConfigBlurRule *rule = &server->config.blur_rules[i];

			if (rule->type == BLUR_MATCH_NAMESPACE) {
				if (blur_match_string(rule->pattern, ls->namespace)) {
					return rule;
				}
			}
		}
	}

#if defined(WYO_HAS_XWAYLAND)
	View *view = view_from_surface(server, surface);
	if (view && view->type == VIEW_TYPE_XWAYLAND && view->xwayland.xsurface) {
		struct wlr_xwayland_surface *xs = view->xwayland.xsurface;

		for (int i = 0; i < server->config.blur_rule_count; i++) {
			const ConfigBlurRule *rule = &server->config.blur_rules[i];

			if (rule->type == BLUR_MATCH_CLASS) {
				if (blur_match_string(rule->pattern, xs->class)) {
					return rule;
				}
			} else if (rule->type == BLUR_MATCH_INSTANCE) {
				if (blur_match_string(rule->pattern, xs->instance)) {
					return rule;
				}
			} else if (rule->type == BLUR_MATCH_TITLE) {
				if (blur_match_string(rule->pattern, xs->title)) {
					return rule;
				}
			}
		}
	}
#endif

	return NULL;
}

static void blur_add_border(struct wlr_scene_rect *rect) {
	if (!rect) {
		return;
	}

	if (g_blur_border_count >= (int)(sizeof(g_blur_borders) / sizeof(g_blur_borders[0]))) {
		return;
	}

	for (int i = 0; i < g_blur_border_count; i++) {
		if (g_blur_borders[i] == rect) {
			return;
		}
	}

	g_blur_borders[g_blur_border_count++] = rect;
}

static bool blur_rect_is_border(struct wlr_scene_rect *rect) {
	if (!rect) {
		return false;
	}

	for (int i = 0; i < g_blur_border_count; i++) {
		if (g_blur_borders[i] == rect) {
			return true;
		}
	}

	return false;
}

static bool blur_surface_on_output(Server *server, struct wlr_surface *surface, struct Output *output) {
	if (!server || !surface || !output) {
		return false;
	}

	struct wlr_surface *root = wlr_surface_get_root_surface(surface);
	if (!root) {
		root = surface;
	}

	View *view = view_from_surface(server, root);
	if (view) {
		if (view->floating || view->dragging || view->fullscreen) {
			int vx0 = view->x;
			int vy0 = view->y;
			int vx1 = view->x + view->width;
			int vy1 = view->y + view->height;
			int ox0 = output->x;
			int oy0 = output->y;
			int ox1 = output->x + output->width;
			int oy1 = output->y + output->height;
			return vx0 < ox1 && vx1 > ox0 && vy0 < oy1 && vy1 > oy0;
		}

		return view->output == output;
	}

	struct wlr_layer_surface_v1 *ls = wlr_layer_surface_v1_try_from_wlr_surface(root);
	if (ls) {
		if (ls->output == output->wlr_output) {
			return true;
		}
		if (ls->output == NULL) {
			return true;
		}
		return false;
	}

	return true;
}

static int collect_blur_rects(
	Server *server,
	struct Output *output,
	struct wlr_scene_tree *tree,
	int offset_x,
	int offset_y,
	BlurRect *rects,
	int max_rects,
	int count,
	int *max_radius
) {
	if (!server || !output || !tree || !tree->node.enabled || count >= max_rects) {
		return count;
	}

	struct wlr_scene_node *child;
	wl_list_for_each(child, &tree->children, link) {
		if (!child->enabled) {
			continue;
		}

		int cx = offset_x + child->x;
		int cy = offset_y + child->y;

		if (child->type == WLR_SCENE_NODE_TREE) {
			struct wlr_scene_tree *sub = wlr_scene_tree_from_node(child);
			count = collect_blur_rects(server, output, sub, cx, cy, rects, max_rects, count, max_radius);
			continue;
		}

		if (child->type != WLR_SCENE_NODE_BUFFER) {
			continue;
		}

		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(child);
		if (!sb->buffer) {
			continue;
		}

		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
		if (!ss || !ss->surface) {
			continue;
		}

		struct wlr_surface *root = wlr_surface_get_root_surface(ss->surface);
		if (!root) {
			root = ss->surface;
		}

		const ConfigBlurRule *rule = surface_blur_rule(server, ss->surface);
		if (!rule && root != ss->surface) {
			rule = surface_blur_rule(server, root);
		}
		if (!rule) {
			continue;
		}

		if (!blur_surface_on_output(server, root, output)) {
			continue;
		}

		float strength = (float)rule->strength;
		if (strength <= 0.0f) {
			continue;
		}
		if (strength > 1.0f) {
			strength = 1.0f;
		}

		if (rule->radius > *max_radius) {
			*max_radius = rule->radius;
		}

		int w = sb->dst_width > 0 ? sb->dst_width : (int)sb->buffer->width;
		int h = sb->dst_height > 0 ? sb->dst_height : (int)sb->buffer->height;
		if (w <= 0 || h <= 0) {
			continue;
		}

		View *view = view_from_surface(server, root);
		if (view && view->mapped && view->root_tree && view->width > 0 && view->height > 0) {
			cx = view->root_tree->node.x - output->x;
			cy = view->root_tree->node.y - output->y;

			w = (int)lround(view->anim_w.current * view->anim_scale.current);
			h = (int)lround(view->anim_h.current * view->anim_scale.current);

			if (w <= 0) w = view->width;
			if (h <= 0) h = view->height;

			for (int i = 0; i < 4; i++) {
				blur_add_border(view->border[i]);
			}
		}

		int rx = cx;
		int ry = cy;
		int rw = w;
		int rh = h;

		if (rx < 0) {
			rw += rx;
			rx = 0;
		}
		if (ry < 0) {
			rh += ry;
			ry = 0;
		}
		if (rx + rw > output->width) {
			rw = output->width - rx;
		}
		if (ry + rh > output->height) {
			rh = output->height - ry;
		}

		if (rw <= 0 || rh <= 0) {
			continue;
		}

		if (count < max_rects) {
			rects[count].x = rx;
			rects[count].y = ry;
			rects[count].w = rw;
			rects[count].h = rh;
			rects[count].strength = strength;
			count++;
		}
	}

	return count;
}

static bool buffer_visible_on_output(struct wlr_scene_buffer *sb, struct Output *output) {
	if (!sb || !sb->buffer || !output || !output->server) {
		return true;
	}

	struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
	if (!ss || !ss->surface) {
		return true;
	}

	struct wlr_surface *root = wlr_surface_get_root_surface(ss->surface);
	if (!root) {
		return true;
	}

	Server *server = output->server;
	View *view;
	wl_list_for_each(view, &server->views, link) {
		bool match = false;

		if (view->type == VIEW_TYPE_XDG &&
			view->xdg.xdg_surface &&
			view->xdg.xdg_surface->surface == root) {
			match = true;
		}
#if defined(WYO_HAS_XWAYLAND)
		else if (view->type == VIEW_TYPE_XWAYLAND &&
				 view->xwayland.xsurface &&
				 view->xwayland.xsurface->surface == root) {
			match = true;
		}
#endif

		if (!match) {
			continue;
		}

		if (view->floating || view->fullscreen) {
			int vx0 = view->x;
			int vy0 = view->y;
			int vx1 = view->x + view->width;
			int vy1 = view->y + view->height;
			int ox0 = output->x;
			int oy0 = output->y;
			int ox1 = output->x + output->width;
			int oy1 = output->y + output->height;
			return vx0 < ox1 && vx1 > ox0 && vy0 < oy1 && vy1 > oy0;
		}

		return view->output == output;
	}

	return true;
}

static void render_scene_tree(
	struct wlr_render_pass *pass,
	struct wlr_renderer *renderer,
	struct wlr_scene_tree *tree,
	int offset_x,
	int offset_y,
	float scale,
	FrameTextureList *owned,
	FrameSurfaceList *surfaces,
	int blur_mode,
	struct Output *output
) {
	if (!tree || !tree->node.enabled) {
		return;
	}

	struct wlr_scene_node *child;
	wl_list_for_each(child, &tree->children, link) {
		if (!child->enabled) {
			continue;
		}

		float base_x = (float)(offset_x + child->x) * scale;
		float base_y = (float)(offset_y + child->y) * scale;

		if (child->type == WLR_SCENE_NODE_TREE) {
			struct wlr_scene_tree *sub = wlr_scene_tree_from_node(child);
			render_scene_tree(
				pass,
				renderer,
				sub,
				offset_x + child->x,
				offset_y + child->y,
				scale,
				owned,
				surfaces,
				blur_mode,
				output
			);
			continue;
		}

		if (child->type == WLR_SCENE_NODE_RECT) {
			struct wlr_scene_rect *rect = wlr_scene_rect_from_node(child);

			if (blur_mode == BLUR_MODE_ONLY && !blur_rect_is_border(rect)) {
				continue;
			}

			if (rect->width <= 0 || rect->height <= 0) {
				continue;
			}
			if (rect->color[3] <= 0.0f) {
				continue;
			}

			struct wlr_render_rect_options opts = {0};
			opts.box.x = (int)lround(base_x);
			opts.box.y = (int)lround(base_y);
			opts.box.width = (int)lround((float)rect->width * scale);
			opts.box.height = (int)lround((float)rect->height * scale);
			opts.color.r = rect->color[0] * rect->color[3];
			opts.color.g = rect->color[1] * rect->color[3];
			opts.color.b = rect->color[2] * rect->color[3];
			opts.color.a = rect->color[3];

			wlr_render_pass_add_rect(pass, &opts);
			continue;
		}

		if (child->type != WLR_SCENE_NODE_BUFFER) {
			continue;
		}

		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(child);
		if (!sb->buffer) {
			continue;
		}
		if (sb->opacity <= 0.0f) {
			continue;
		}
		if (!buffer_visible_on_output(sb, output)) {
			continue;
		}

		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
		bool needs_blur = false;

		if (ss && ss->surface && output && output->server) {
			const ConfigBlurRule *rule = surface_blur_rule(output->server, ss->surface);
			if (!rule) {
				struct wlr_surface *root = wlr_surface_get_root_surface(ss->surface);
				if (root && root != ss->surface) {
					rule = surface_blur_rule(output->server, root);
				}
			}
			needs_blur = rule != NULL;
		}

		if (blur_mode == BLUR_MODE_SKIP && needs_blur) {
			continue;
		}
		if (blur_mode == BLUR_MODE_ONLY && !needs_blur) {
			continue;
		}

		struct wlr_texture *texture = NULL;
		if (ss && ss->surface) {
			texture = wlr_surface_get_texture(ss->surface);
		}

		if (!texture) {
			texture = wlr_texture_from_buffer(renderer, sb->buffer);
			if (!texture) {
				continue;
			}
			ftl_push(owned, texture);
		}

		int dst_w = sb->dst_width;
		int dst_h = sb->dst_height;

		if (dst_w <= 0) {
			dst_w = sb->src_box.width > 0.0
				? (int)sb->src_box.width
				: (int)texture->width;
		}
		if (dst_h <= 0) {
			dst_h = sb->src_box.height > 0.0
				? (int)sb->src_box.height
				: (int)texture->height;
		}
		if (dst_w <= 0 || dst_h <= 0) {
			continue;
		}

		struct wlr_render_texture_options opts = {0};
		opts.texture = texture;
		opts.src_box = sb->src_box;
		opts.dst_box.x = (int)lround(base_x);
		opts.dst_box.y = (int)lround(base_y);
		opts.dst_box.width = (int)lround((float)dst_w * scale);
		opts.dst_box.height = (int)lround((float)dst_h * scale);
		opts.transform = sb->transform;
		opts.filter_mode = sb->filter_mode;

		if (sb->opacity < 1.0f) {
			opts.alpha = &sb->opacity;
		}

		wlr_render_pass_add_texture(pass, &opts);

		if (ss && ss->surface) {
			fsl_push(surfaces, ss->surface);
		}
	}
}

static struct wlr_buffer *ensure_buffer(
	Server *server,
	struct wlr_buffer **buf,
	int *bw,
	int *bh,
	int w,
	int h
) {
	if (*buf && *bw == w && *bh == h) {
		return *buf;
	}

	if (*buf) {
		wlr_buffer_unlock(*buf);
		*buf = NULL;
	}

	const struct wlr_drm_format *fmt = NULL;
	const struct wlr_drm_format_set *formats =
		wlr_renderer_get_texture_formats(server->renderer, WLR_BUFFER_CAP_DMABUF);

	if (formats) {
		fmt = wlr_drm_format_set_get(formats, DRM_FORMAT_ARGB8888);
		if (!fmt) {
			fmt = wlr_drm_format_set_get(formats, DRM_FORMAT_XRGB8888);
		}
	}

	if (!fmt) {
		formats = wlr_renderer_get_texture_formats(server->renderer, 0);
		if (formats) {
			fmt = wlr_drm_format_set_get(formats, DRM_FORMAT_ARGB8888);
		}
	}

	if (!fmt) {
		formats = wlr_renderer_get_texture_formats(server->renderer, WLR_BUFFER_CAP_DATA_PTR);
		if (formats) {
			fmt = wlr_drm_format_set_get(formats, DRM_FORMAT_ARGB8888);
		}
	}

	if (!fmt) {
		return NULL;
	}

	struct wlr_buffer *new_buf = wlr_allocator_create_buffer(server->allocator, w, h, fmt);
	if (!new_buf) {
		return NULL;
	}

	wlr_buffer_lock(new_buf);
	*buf = new_buf;
	*bw = w;
	*bh = h;
	return new_buf;
}

static bool render_direct(struct Output *output) {
	Server *server = output->server;
	struct wlr_output *wlr_output = output->wlr_output;

	struct wlr_output_state state;
	wlr_output_state_init(&state);

	struct wlr_render_pass *pass = wlr_output_begin_render_pass(wlr_output, &state, NULL);
	if (!pass) {
		wlr_output_state_finish(&state);
		return false;
	}

	FrameTextureList owned = {0};
	FrameSurfaceList surfaces = {0};
	float scale = wlr_output->scale > 0.0f ? wlr_output->scale : 1.0f;

	render_scene_tree(
		pass,
		server->renderer,
		server->scene_tree,
		-output->x,
		-output->y,
		scale,
		&owned,
		&surfaces,
		BLUR_MODE_ALL,
		output
	);

	int dw = wlr_output->width > 0 ? wlr_output->width : 1;
	int dh = wlr_output->height > 0 ? wlr_output->height : 1;

	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, dw, dh);
	wlr_output_state_set_damage(&state, &damage);
	wlr_output_add_software_cursors_to_render_pass(wlr_output, pass, &damage);

	bool ok = wlr_render_pass_submit(pass);
	if (ok) {
		ok = wlr_output_commit_state(wlr_output, &state);
	}

	if (ok) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		for (size_t i = 0; i < surfaces.count; i++) {
			if (surfaces.items[i]) {
				wlr_surface_send_frame_done(surfaces.items[i], &now);
			}
		}
	}

	pixman_region32_fini(&damage);
	ftl_finish(&owned);
	fsl_finish(&surfaces);
	wlr_output_state_finish(&state);
	return ok;
}

static bool render_with_blur(struct Output *output, BlurRect *rects, int num_rects, int blur_radius) {
	Server *server = output->server;
	struct wlr_output *wlr_output = output->wlr_output;

	int ow = wlr_output->width;
	int oh = wlr_output->height;

	if (ow <= 0 || oh <= 0) {
		return render_direct(output);
	}

	struct wlr_buffer *backdrop = ensure_buffer(server, &g_backdrop, &g_backdrop_w, &g_backdrop_h, ow, oh);
	struct wlr_buffer *blurred = ensure_buffer(server, &g_blurred, &g_blurred_w, &g_blurred_h, ow, oh);

	if (!backdrop || !blurred) {
		return render_direct(output);
	}

	struct wlr_buffer_pass_options bpass_opts = {0};
	struct wlr_render_pass *bpass = wlr_renderer_begin_buffer_pass(server->renderer, backdrop, &bpass_opts);
	if (!bpass) {
		return render_direct(output);
	}

	FrameTextureList owned = {0};
	FrameSurfaceList surfaces = {0};
	float scale = wlr_output->scale > 0.0f ? wlr_output->scale : 1.0f;

	render_scene_tree(
		bpass,
		server->renderer,
		server->scene_tree,
		-output->x,
		-output->y,
		scale,
		&owned,
		&surfaces,
		BLUR_MODE_SKIP,
		output
	);

	if (!wlr_render_pass_submit(bpass)) {
		ftl_finish(&owned);
		fsl_finish(&surfaces);
		return render_direct(output);
	}

	GLuint src_fbo = wlr_gles2_renderer_get_buffer_fbo(server->renderer, backdrop);
	GLuint dst_fbo = wlr_gles2_renderer_get_buffer_fbo(server->renderer, blurred);

	bool blurred_ok = false;
	if (src_fbo && dst_fbo && g_blur.initialized && blur_radius > 0) {
	GLScope scope;
        if (gl_scope_begin(server, &scope)) {
            blurred_ok = blur_apply_full(&g_blur, src_fbo, dst_fbo, ow, oh, blur_radius);
            gl_scope_end(server, &scope);
        }
	}

	if (!blurred_ok) {
		ftl_finish(&owned);
		fsl_finish(&surfaces);
		return render_direct(output);
	}

	struct wlr_texture *sharp_tex = wlr_texture_from_buffer(server->renderer, backdrop);
	struct wlr_texture *blur_tex = wlr_texture_from_buffer(server->renderer, blurred);

	if (!sharp_tex || !blur_tex) {
		if (sharp_tex) {
			wlr_texture_destroy(sharp_tex);
		}
		if (blur_tex) {
			wlr_texture_destroy(blur_tex);
		}
		ftl_finish(&owned);
		fsl_finish(&surfaces);
		return render_direct(output);
	}

	ftl_push(&owned, sharp_tex);
	ftl_push(&owned, blur_tex);

	struct wlr_output_state state;
	wlr_output_state_init(&state);

	struct wlr_render_pass *pass = wlr_output_begin_render_pass(wlr_output, &state, NULL);
	if (!pass) {
		wlr_output_state_finish(&state);
		ftl_finish(&owned);
		fsl_finish(&surfaces);
		return false;
	}

	struct wlr_render_texture_options full = {0};
	full.texture = sharp_tex;
	full.dst_box.x = 0;
	full.dst_box.y = 0;
	full.dst_box.width = ow;
	full.dst_box.height = oh;
	wlr_render_pass_add_texture(pass, &full);

	for (int i = 0; i < num_rects; i++) {
		int rx = (int)lround((float)rects[i].x * scale);
		int ry = (int)lround((float)rects[i].y * scale);
		int rw = (int)lround((float)rects[i].w * scale);
		int rh = (int)lround((float)rects[i].h * scale);

		if (rw <= 0 || rh <= 0) {
			continue;
		}

		float alpha = rects[i].strength;
		if (alpha <= 0.0f) {
			continue;
		}
		if (alpha > 1.0f) {
			alpha = 1.0f;
		}

		struct wlr_render_texture_options opts = {0};
		opts.texture = blur_tex;
		opts.src_box.x = (double)rx;
		opts.src_box.y = (double)ry;
		opts.src_box.width = (double)rw;
		opts.src_box.height = (double)rh;
		opts.dst_box.x = rx;
		opts.dst_box.y = ry;
		opts.dst_box.width = rw;
		opts.dst_box.height = rh;
		opts.alpha = &alpha;

		wlr_render_pass_add_texture(pass, &opts);
	}

	render_scene_tree(
		pass,
		server->renderer,
		server->scene_tree,
		-output->x,
		-output->y,
		scale,
		&owned,
		&surfaces,
		BLUR_MODE_ONLY,
		output
	);

	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, ow, oh);
	wlr_output_state_set_damage(&state, &damage);
	wlr_output_add_software_cursors_to_render_pass(wlr_output, pass, &damage);

	bool ok = wlr_render_pass_submit(pass);
	if (ok) {
		ok = wlr_output_commit_state(wlr_output, &state);
	}

	if (ok) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		for (size_t i = 0; i < surfaces.count; i++) {
			if (surfaces.items[i]) {
				wlr_surface_send_frame_done(surfaces.items[i], &now);
			}
		}
	}

	pixman_region32_fini(&damage);
	ftl_finish(&owned);
	fsl_finish(&surfaces);
	wlr_output_state_finish(&state);
	return ok;
}

static bool find_single_scene_surface(
	struct wlr_scene_tree *tree,
	struct wlr_surface **surface_out,
	struct wlr_buffer **buffer_out,
	int *count
) {
	if (!tree) {
		return false;
	}

	struct wlr_scene_node *child;
	wl_list_for_each(child, &tree->children, link) {
		if (!child->enabled) {
			continue;
		}

		if (child->type == WLR_SCENE_NODE_TREE) {
			struct wlr_scene_tree *sub = wlr_scene_tree_from_node(child);
			if (!find_single_scene_surface(sub, surface_out, buffer_out, count)) {
				return false;
			}
		} else if (child->type == WLR_SCENE_NODE_BUFFER) {
			struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(child);
			struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);

			if (!ss || !ss->surface || !sb->buffer) {
				continue;
			}

			(*count)++;

			if (*count > 1) {
				return false;
			}

			*surface_out = ss->surface;
			*buffer_out = sb->buffer;
		} else {
			return false;
		}
	}

	return true;
}

static bool surface_opaque_covers_buffer(
	struct wlr_surface *surface,
	struct wlr_buffer *buffer
) {
	pixman_box32_t rect = {
		.x1 = 0,
		.y1 = 0,
		.x2 = buffer->width,
		.y2 = buffer->height
	};

	return pixman_region32_contains_rectangle(
		&surface->current.opaque,
		&rect
	);
}

static bool buffer_get_dmabuf_format(struct wlr_buffer *buffer, uint32_t *format_out) {
	struct wlr_dmabuf_attributes attrs = {0};

	if (!wlr_buffer_get_dmabuf(buffer, &attrs)) {
		return false;
	}

	if (format_out) {
		*format_out = attrs.format;
	}

	wlr_dmabuf_attributes_finish(&attrs);
	return true;
}

static bool buffer_format_supported(struct wlr_output *wlr_output, uint32_t format) {
	const struct wlr_drm_format_set *formats =
		wlr_output_get_primary_formats(wlr_output, WLR_BUFFER_CAP_DMABUF);

	if (!formats) {
		return true;
	}

	return wlr_drm_format_set_get(formats, format) != NULL;
}

bool output_try_direct_scanout(Output *output) {
	if (!output || !output->server || output->server->shutting_down || !output->wlr_output) {
		return false;
	}

	Server *server = output->server;

	if (server->drag.active || server->dnd_active) {
		return false;
	}

	if (!wl_list_empty(&server->popup_surfaces)) {
		return false;
	}

	if (output->wallpaper_fade || output->wallpaper_fade_anim.active) {
		return false;
	}

	View *view = output_fullscreen_view(output);
	if (!view || !view->root_tree) {
		return false;
	}

	if (view->anim_x.active ||
		view->anim_y.active ||
		view->anim_w.active ||
		view->anim_h.active ||
		view->anim_scale.active) {
		return false;
	}

	struct wlr_surface *surface = NULL;
	struct wlr_buffer *buffer = NULL;
	int count = 0;

	if (!find_single_scene_surface(view->root_tree, &surface, &buffer, &count)) {
		return false;
	}

	if (count != 1 || !surface || !buffer) {
		return false;
	}

	if (buffer->width != output->wlr_output->width ||
		buffer->height != output->wlr_output->height) {
		return false;
	}

	if (surface->current.transform != output->wlr_output->transform) {
		return false;
	}

	if (surface->current.scale != 1 &&
		(float)surface->current.scale != output->wlr_output->scale) {
		return false;
	}

	if (!surface_opaque_covers_buffer(surface, buffer)) {
		return false;
	}

	uint32_t buffer_format = 0;

	if (!buffer_get_dmabuf_format(buffer, &buffer_format)) {
		return false;
	}

	if (!buffer_format_supported(output->wlr_output, buffer_format)) {
		return false;
	}

	if (!wlr_output_is_direct_scanout_allowed(output->wlr_output)) {
		return false;
	}

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_buffer(&state, buffer);

	pixman_region32_t damage;
	pixman_region32_init_rect(&damage, 0, 0, buffer->width, buffer->height);
	wlr_output_state_set_damage(&state, &damage);
	pixman_region32_fini(&damage);

	bool ok = wlr_output_commit_state(output->wlr_output, &state);
	wlr_output_state_finish(&state);

	if (ok) {
		output->render_pending = false;

		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		wlr_surface_send_frame_done(surface, &now);
	}

	return ok;
}

bool output_has_fullscreen_view(Output *output) {
	if (!output || !output->server) return false;

	View *view;
	wl_list_for_each(view, &output->server->views, link) {
		if (view->output == output &&
			view->mapped &&
			view->fullscreen &&
			view->root_tree &&
			view->root_tree->node.enabled) {
			return true;
		}
	}

	return false;
}

bool output_render_custom(struct Output *output) {
	output->render_pending = false;
	Server *server = output->server;

	if (!g_blur_tried) {
		g_blur_tried = true;
		if (wlr_renderer_is_gles2(server->renderer)) {
			GLScope scope;
			if (gl_scope_begin(server, &scope)) {
				blur_init(&g_blur, BLUR_MAX_TAPS);
				gl_scope_end(server, &scope);
			}
		}
	}

	BlurRect rects[MAX_BLUR_RECTS];
	int num_rects = 0;
	int blur_radius = 0;
	g_blur_border_count = 0;

	if (g_blur.initialized && !output_has_fullscreen_view(output)) {
		num_rects = collect_blur_rects(
			server,
			output,
			server->scene_tree,
			-output->x,
			-output->y,
			rects,
			MAX_BLUR_RECTS,
			0,
			&blur_radius
		);
	}

	if (num_rects > 0 && blur_radius > 0) {
		return render_with_blur(output, rects, num_rects, blur_radius);
	}

	return render_direct(output);
}
