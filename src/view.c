#include "view.h"
#include "server.h"
#include <stdlib.h>
#include <stdio.h>
#include <dlfcn.h>
#include <math.h>
#include <wlr/util/box.h>
#include <wlr/util/edges.h>
#include <ctype.h>
#include <string.h>

static void handle_map(struct wl_listener *listener, void *data);
static void handle_unmap(struct wl_listener *listener, void *data);
static void view_play_open_animation(View *view);
static void handle_destroy(struct wl_listener *listener, void *data);
static void handle_commit(struct wl_listener *listener, void *data);
static void handle_toplevel_destroy(struct wl_listener *listener, void *data);
static void handle_request_fullscreen(struct wl_listener *listener, void *data);
static void handle_request_move(struct wl_listener *listener, void *data);
static void handle_request_resize(struct wl_listener *listener, void *data);
static void handle_new_popup(struct wl_listener *listener, void *data);
static void handle_scene_node_destroy(struct wl_listener *listener, void *data);
static void check_sticky(View *view);

typedef void (*scene_node_corner_radius_fn)(struct wlr_scene_node *node, float radius);
typedef void (*scene_buffer_corner_radius_fn)(struct wlr_scene_buffer *buffer, float radius);

static scene_node_corner_radius_fn resolve_node_corner_radius(void) {
    static scene_node_corner_radius_fn fn = NULL;
    static bool resolved = false;

    if (!resolved) {
        resolved = true;
        fn = (scene_node_corner_radius_fn)dlsym(RTLD_DEFAULT, "wlr_scene_node_set_corner_radius");
    }

    return fn;
}

static scene_buffer_corner_radius_fn resolve_buffer_corner_radius(void) {
    static scene_buffer_corner_radius_fn fn = NULL;
    static bool resolved = false;

    if (!resolved) {
        resolved = true;
        fn = (scene_buffer_corner_radius_fn)dlsym(RTLD_DEFAULT, "wlr_scene_buffer_set_corner_radius");
    }

    return fn;
}

static bool corner_radius_supported(void) {
    return resolve_node_corner_radius() != NULL || resolve_buffer_corner_radius() != NULL;
}

static void apply_corner_radius_buffer(struct wlr_scene_buffer *buffer, int x, int y, void *data) {
    (void)x;
    (void)y;

    float radius = *(const float *)data;
    scene_buffer_corner_radius_fn fn = resolve_buffer_corner_radius();

    if (fn) {
        fn(buffer, radius);
    }
}

static void view_apply_corner_radius(View *view, double radius) {
    if (!view || radius < 0.0) return;

    if (view->scene_tree) {
        scene_node_corner_radius_fn node_fn = resolve_node_corner_radius();

        if (node_fn) {
            node_fn(&view->scene_tree->node, (float)radius);
            return;
        }

        scene_buffer_corner_radius_fn buffer_fn = resolve_buffer_corner_radius();

        if (buffer_fn) {
            float r = (float)radius;
            wlr_scene_node_for_each_buffer(&view->scene_tree->node, apply_corner_radius_buffer, &r);
        }
    }
}

static inline void listener_remove(struct wl_listener *listener) {
    if (!wl_list_empty(&listener->link)) {
        wl_list_remove(&listener->link);
        wl_list_init(&listener->link);
    }
}
#if defined(WYO_HAS_FOREIGN_TOPLEVEL)
static void handle_foreign_activate(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, foreign_activate);
    (void)data;
    if (view->server && view->mapped) {
        server_focus_view(view->server, view);
    }
}
static void handle_foreign_close(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, foreign_close);
    (void)data;
    view_close(view);
}
static void handle_foreign_destroy(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, foreign_destroy);
    (void)data;
    view->foreign_handle = NULL;
    view->foreign_output = NULL;
    listener_remove(&view->foreign_activate);
    listener_remove(&view->foreign_close);
    listener_remove(&view->foreign_set_fullscreen);
    listener_remove(&view->foreign_set_maximized);
    listener_remove(&view->foreign_destroy);
}
static void view_foreign_sync_meta(View *view) {
	if (!view->foreign_handle || !view->xdg.toplevel) return;

	char safe_title[2048];
	char safe_app_id[2048];

	snprintf(safe_title, sizeof(safe_title), "%s",
		view->xdg.toplevel->title ? view->xdg.toplevel->title : "");
	snprintf(safe_app_id, sizeof(safe_app_id), "%s",
		view->xdg.toplevel->app_id ? view->xdg.toplevel->app_id : "");

	wlr_foreign_toplevel_handle_v1_set_title(view->foreign_handle, safe_title);
	wlr_foreign_toplevel_handle_v1_set_app_id(view->foreign_handle, safe_app_id);
}
static void view_foreign_create(View *view) {
    Server *server = view->server;
    if (!server || view->foreign_handle) return;
    if (!server->foreign_toplevel_manager) return;
    struct wlr_foreign_toplevel_handle_v1 *handle =
        wlr_foreign_toplevel_handle_v1_create(server->foreign_toplevel_manager);
    if (!handle) return;
    view->foreign_handle = handle;
    view->foreign_output = NULL;
    view->foreign_activated = false;
    view->foreign_maximized = false;
    view->foreign_fullscreen = false;
    wl_list_init(&view->foreign_activate.link);
    wl_list_init(&view->foreign_close.link);
    wl_list_init(&view->foreign_set_fullscreen.link);
    wl_list_init(&view->foreign_set_maximized.link);
    wl_list_init(&view->foreign_destroy.link);
    view->foreign_activate.notify = handle_foreign_activate;
    wl_signal_add(&handle->events.request_activate, &view->foreign_activate);
    view->foreign_close.notify = handle_foreign_close;
    wl_signal_add(&handle->events.request_close, &view->foreign_close);
    view->foreign_destroy.notify = handle_foreign_destroy;
    wl_signal_add(&handle->events.destroy, &view->foreign_destroy);
    view_foreign_sync_meta(view);
}
static void view_foreign_destroy(View *view) {
    if (!view->foreign_handle) return;
    struct wlr_foreign_toplevel_handle_v1 *handle = view->foreign_handle;
    listener_remove(&view->foreign_activate);
    listener_remove(&view->foreign_close);
    listener_remove(&view->foreign_set_fullscreen);
    listener_remove(&view->foreign_set_maximized);
    listener_remove(&view->foreign_destroy);
    view->foreign_handle = NULL;
    view->foreign_output = NULL;
    wlr_foreign_toplevel_handle_v1_destroy(handle);
}
#else
static void view_foreign_sync_meta(View *view) {
    (void)view;
}
static void view_foreign_create(View *view) {
    (void)view;
}
static void view_foreign_destroy(View *view) {
    (void)view;
}
#endif
void view_foreign_sync(View *view) {
#if defined(WYO_HAS_FOREIGN_TOPLEVEL)
if (!view || !view->foreign_handle) return;
struct wlr_output *out = view->output ? view->output->wlr_output : NULL;
if (out != view->foreign_output) {
    if (view->foreign_output) {
        wlr_foreign_toplevel_handle_v1_output_leave(view->foreign_handle, view->foreign_output);
    }
    if (out) {
        wlr_foreign_toplevel_handle_v1_output_enter(view->foreign_handle, out);
    }
    view->foreign_output = out;
}
bool activated = view->server && view->server->focused_view == view;
if (activated != view->foreign_activated) {
    wlr_foreign_toplevel_handle_v1_set_activated(view->foreign_handle, activated);
    view->foreign_activated = activated;
}
if (view->tiled != view->foreign_maximized) {
    wlr_foreign_toplevel_handle_v1_set_maximized(view->foreign_handle, view->tiled);
    view->foreign_maximized = view->tiled;
}
if (view->fullscreen != view->foreign_fullscreen) {
    wlr_foreign_toplevel_handle_v1_set_fullscreen(view->foreign_handle, view->fullscreen);
    view->foreign_fullscreen = view->fullscreen;
}
#else
    (void)view;
#endif
}
static void handle_set_title(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, set_title);
    (void)data;
    view_foreign_sync_meta(view);
}

static void handle_set_app_id(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, set_app_id);
    (void)data;
    view_foreign_sync_meta(view);
    check_sticky(view);
}

static void handle_scene_node_destroy(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, scene_node_destroy);
    (void)data;

    listener_remove(&view->scene_node_destroy);
    view->scene_tree = NULL;
}

static void handle_destroy(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, xdg.destroy);
    (void)data;

    view_destroy(view);
}
static void handle_request_fullscreen(struct wl_listener *listener, void *data) {
	View *view = wl_container_of(listener, view, xdg.request_fullscreen);
	(void)data;

	if (!view || !view->server || view->server->shutting_down) {
		return;
	}

	if (!view->mapped || !view->xdg.toplevel || !view->xdg.xdg_surface || !view->xdg.xdg_surface->initialized) {
		return;
	}

	bool requested = view->xdg.toplevel->requested.fullscreen;

	if (requested && !view->fullscreen) {
		view_toggle_fullscreen(view);
	} else if (!requested && view->fullscreen) {
		view_toggle_fullscreen(view);
	}
}

static void handle_request_move(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, xdg.request_move);
    (void)data;

    if (!view || !view->server || view->server->shutting_down) return;
}

static void handle_request_resize(struct wl_listener *listener, void *data) {
View *view = wl_container_of(listener, view, xdg.request_resize);
(void)data;
if (!view || !view->server || view->server->shutting_down) return;
}

typedef struct PopupConstrain {
    View *view;
    struct wlr_xdg_popup *popup;
    struct wl_listener commit;
    struct wl_listener destroy;
} PopupConstrain;
static void popup_constrain_finish(PopupConstrain *pc) {
    listener_remove(&pc->commit);
    listener_remove(&pc->destroy);
    free(pc);
}
static void popup_constrain_destroy(struct wl_listener *listener, void *data) {
    PopupConstrain *pc = wl_container_of(listener, pc, destroy);
    (void)data;
    popup_constrain_finish(pc);
}
static void popup_constrain_commit(struct wl_listener *listener, void *data) {
    PopupConstrain *pc = wl_container_of(listener, pc, commit);
    (void)data;

    struct wlr_xdg_popup *popup = pc->popup;
    if (!popup->base || !popup->base->initialized) return;

    View *view = pc->view;
    Output *out = view ? view->output : NULL;
    if (!out) {
        popup_constrain_finish(pc);
        return;
    }

    if (!popup->parent ||
        !view->xdg.xdg_surface ||
        popup->parent != view->xdg.xdg_surface->surface) {
        popup_constrain_finish(pc);
        return;
    }

    int origin_x = view->x;
    int origin_y = view->y;

    if (view->root_tree) {
        origin_x = view->root_tree->node.x;
        origin_y = view->root_tree->node.y;
    }

    if (view->scene_tree) {
        origin_x += view->scene_tree->node.x;
        origin_y += view->scene_tree->node.y;
    }

    struct wlr_box box;
    box.x = out->x + out->usable_x - origin_x;
    box.y = out->y + out->usable_y - origin_y;
    box.width = out->usable_width;
    box.height = out->usable_height;

    wlr_xdg_popup_unconstrain_from_box(popup, &box);

    if (popup->base->data) {
        struct wlr_scene_tree *tree = popup->base->data;
        wlr_scene_node_raise_to_top(&tree->node);
    }

    popup_constrain_finish(pc);
}

static void popup_surface_destroy_notify(struct wl_listener *listener, void *data) {
    PopupSurface *popup_surface = wl_container_of(listener, popup_surface, destroy);
    (void)data;

    if (!wl_list_empty(&popup_surface->destroy.link)) {
        wl_list_remove(&popup_surface->destroy.link);
        wl_list_init(&popup_surface->destroy.link);
    }

    if (!wl_list_empty(&popup_surface->link)) {
        wl_list_remove(&popup_surface->link);
        wl_list_init(&popup_surface->link);
    }

    free(popup_surface);
}

static void handle_new_popup(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, xdg.new_popup);
    struct wlr_xdg_popup *popup = data;

    if (!view || !popup || !popup->base || popup->base->data) return;

    struct wlr_scene_tree *parent = view->scene_tree ? view->scene_tree : view->root_tree;
    if (!parent) return;

    if (popup->parent &&
        view->xdg.xdg_surface &&
        popup->parent != view->xdg.xdg_surface->surface &&
        popup->parent->data) {
        parent = popup->parent->data;
    }

    struct wlr_scene_tree *popup_tree = wlr_scene_xdg_surface_create(parent, popup->base);
    if (!popup_tree) return;

    popup->base->data = popup_tree;

    if (popup->base->surface) {
        popup->base->surface->data = popup_tree;
    }

    if (view->server) {
        PopupSurface *popup_surface = calloc(1, sizeof(PopupSurface));

        if (popup_surface) {
            popup_surface->surface = popup->base->surface;

            wl_list_init(&popup_surface->link);
            wl_list_init(&popup_surface->destroy.link);

            popup_surface->destroy.notify = popup_surface_destroy_notify;
            wl_signal_add(&popup->base->surface->events.destroy, &popup_surface->destroy);

            wl_list_insert(&view->server->popup_surfaces, &popup_surface->link);
        }
    }

    wlr_scene_node_set_enabled(&popup_tree->node, true);
    wlr_scene_node_raise_to_top(&popup_tree->node);

    PopupConstrain *pc = calloc(1, sizeof(PopupConstrain));
    if (!pc) return;

    pc->view = view;
    pc->popup = popup;

    wl_list_init(&pc->commit.link);
    wl_list_init(&pc->destroy.link);

    pc->commit.notify = popup_constrain_commit;
    wl_signal_add(&popup->base->surface->events.commit, &pc->commit);

    pc->destroy.notify = popup_constrain_destroy;
    wl_signal_add(&popup->base->events.destroy, &pc->destroy);
}

static void apply_buffer_opacity(struct wlr_scene_buffer *buffer, int x, int y, void *data) {
    (void)x;
    (void)y;

    float opacity = *(const float *)data;
    wlr_scene_buffer_set_opacity(buffer, opacity);
}

void view_apply_opacity(View *view) {
    if (!view || !view->scene_tree) return;

    float opacity = (float)view->opacity.current;
    wlr_scene_node_for_each_buffer(&view->scene_tree->node, apply_buffer_opacity, &opacity);
}

static void view_schedule_frame(View *view) {
    if (!view || !view->server || view->server->shutting_down) return;

    Output *out = view->output ? view->output : view->server->active_output;
    if (out && out->wlr_output && out->wlr_output->enabled) {
        wlr_output_schedule_frame(out->wlr_output);
    }
}

static const float fallback_border_focused[4] = { 0.4f, 0.6f, 1.0f, 1.0f };
static const float fallback_border_unfocused[4] = { 0.3f, 0.3f, 0.35f, 1.0f };

static struct wlr_scene_buffer *view_find_surface_buffer(View *view) {
	if (!view || !view->scene_tree) return NULL;

	struct wlr_surface *root = NULL;
	if (view->type == VIEW_TYPE_XDG && view->xdg.xdg_surface) {
		root = view->xdg.xdg_surface->surface;
	}
#if defined(WYO_HAS_XWAYLAND)
	else if (view->type == VIEW_TYPE_XWAYLAND && view->xwayland.xsurface) {
		root = view->xwayland.xsurface->surface;
	}
#endif
	if (!root) return NULL;

	struct wlr_scene_node *child;
	wl_list_for_each(child, &view->scene_tree->children, link) {
		if (child->type != WLR_SCENE_NODE_BUFFER) continue;
		struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(child);
		struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
		if (ss && ss->surface == root) return sb;
	}

	return NULL;
}

static void view_visual_size(View *view, int *w_out, int *h_out) {
	double s = view->anim_scale.current;
	if (s < 0.01) s = 0.01;
	double bw = view->anim_w.current;
	double bh = view->anim_h.current;
	if (bw <= 0.0) bw = (double)view->width;
	if (bh <= 0.0) bh = (double)view->height;
	*w_out = (int)lround(bw * s);
	*h_out = (int)lround(bh * s);
	if (*w_out < 1) *w_out = 1;
	if (*h_out < 1) *h_out = 1;
}

static void view_apply_geometry(View *view) {
	if (!view || !view->root_tree) return;

	int vw = 0;
	int vh = 0;
	view_visual_size(view, &vw, &vh);

	int cx = (int)lround(view->anim_x.current);
	int cy = (int)lround(view->anim_y.current);

	int x = cx - (int)lround((double)vw * 0.5);
	int y = cy - (int)lround((double)vh * 0.5);

	int old_x = view->root_tree->node.x;
	int old_y = view->root_tree->node.y;

	if (x != old_x || y != old_y) {
		wlr_scene_node_set_position(&view->root_tree->node, x, y);
	}

	struct wlr_scene_buffer *sb = view_find_surface_buffer(view);
	if (sb && sb->buffer) {
		wlr_scene_buffer_set_dest_size(sb, vw, vh);
	}
}

static void view_update_xdg_tiled_state(View *view) {
	if (!view || view->type != VIEW_TYPE_XDG || !view->xdg.toplevel) return;
	if (!view->mapped) return;
	if (!view->xdg.xdg_surface || !view->xdg.xdg_surface->initialized) return;
	enum wlr_edges edges = WLR_EDGE_NONE;
	if (view->tiled && !view->fullscreen) {
		edges = WLR_EDGE_TOP | WLR_EDGE_BOTTOM | WLR_EDGE_LEFT | WLR_EDGE_RIGHT;
	}
	wlr_xdg_toplevel_set_tiled(view->xdg.toplevel, edges);
}

static void view_update_border(View *view) {
	if (!view) return;
	if (view->mapped) {
		view_update_xdg_tiled_state(view);
	}
	int border_width = 2;
	const float *focused_color = fallback_border_focused;
	const float *unfocused_color = fallback_border_unfocused;
	if (view->server) {
		border_width = view->server->config.border_width;
		focused_color = view->server->config.border_color_focused;
		unfocused_color = view->server->config.border_color_unfocused;
	}
	bool enabled = border_width > 0 &&
		view->mapped &&
		!view->fullscreen &&
		view->width > 0 &&
		view->height > 0;
	bool enabled_changed = false;
	for (int i = 0; i < 4; i++) {
		if (view->border[i] && view->border[i]->node.enabled != enabled) {
			wlr_scene_node_set_enabled(&view->border[i]->node, enabled);
			enabled_changed = true;
		}
	}
	if (!enabled) return;
	int w = 0;
	int h = 0;
	view_visual_size(view, &w, &h);
	int radius = 0;
	if (view->server && corner_radius_supported()) {
		radius = (int)lround(view->server->config.corner_radius);
	}
	if (radius < 0) radius = 0;
	if (radius > w / 2) radius = w / 2;
	if (radius > h / 2) radius = h / 2;
	int horizontal_width = w + border_width * 2 - radius * 2;
	int vertical_height = h - radius * 2;
	if (horizontal_width < 0) horizontal_width = 0;
	if (vertical_height < 0) vertical_height = 0;
	if (view->border[0]) {
		wlr_scene_rect_set_size(view->border[0], horizontal_width, border_width);
		wlr_scene_node_set_position(&view->border[0]->node, -border_width + radius, -border_width);
	}
	if (view->border[1]) {
		wlr_scene_rect_set_size(view->border[1], horizontal_width, border_width);
		wlr_scene_node_set_position(&view->border[1]->node, -border_width + radius, h);
	}
	if (view->border[2]) {
		wlr_scene_rect_set_size(view->border[2], border_width, vertical_height);
		wlr_scene_node_set_position(&view->border[2]->node, -border_width, radius);
	}
	if (view->border[3]) {
		wlr_scene_rect_set_size(view->border[3], border_width, vertical_height);
		wlr_scene_node_set_position(&view->border[3]->node, w, radius);
	}
	float t = (float)clamp01(view->border_blend.current);
	float color[4];
	for (int c = 0; c < 4; c++) {
		color[c] = unfocused_color[c] + (focused_color[c] - unfocused_color[c]) * t;
	}
	for (int i = 0; i < 4; i++) {
		if (view->border[i]) {
			wlr_scene_rect_set_color(view->border[i], color);
		}
	}
	if (enabled_changed) {
		view_schedule_frame(view);
	}
}

void view_refresh_decorations(View *view) {
    if (!view) return;

    bool focused = view->server && view->server->focused_view == view;
    double target = focused ? 1.0 : 0.0;
    if (view->border_blend.target != target && !view->border_blend.active) {
        animation_set_target(&view->border_blend, target, 0, EASING_LINEAR, NULL);
    }

    view_update_border(view);
    view_apply_corner_radius(view, view->server ? view->server->config.corner_radius : 0.0);
}

#if defined(WYO_HAS_XWAYLAND)
static void handle_xwayland_associate(struct wl_listener *listener, void *data);
static void handle_xwayland_dissociate(struct wl_listener *listener, void *data);
static void handle_xwayland_map(struct wl_listener *listener, void *data);
static void handle_xwayland_unmap(struct wl_listener *listener, void *data);
static void handle_xwayland_request_configure(struct wl_listener *listener, void *data);
static void handle_xwayland_request_activate(struct wl_listener *listener, void *data);
static void handle_xwayland_request_close(struct wl_listener *listener, void *data);
static void handle_xwayland_destroy(struct wl_listener *listener, void *data);

static void handle_xwayland_associate(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, xwayland.associate);
    (void)data;
    struct wlr_xwayland_surface *xsurface = view->xwayland.xsurface;
    if (!xsurface || !xsurface->surface) return;
    view->scene_tree = wlr_scene_subsurface_tree_create(view->root_tree, xsurface->surface);
    if (view->scene_tree) {
        wl_list_init(&view->scene_node_destroy.link);
        view->scene_node_destroy.notify = handle_scene_node_destroy;
        wl_signal_add(&view->scene_tree->node.events.destroy, &view->scene_node_destroy);
    }
    wl_list_init(&view->xwayland.map.link);
    wl_list_init(&view->xwayland.unmap.link);
    view->xwayland.map.notify = handle_xwayland_map;
    wl_signal_add(&xsurface->surface->events.map, &view->xwayland.map);
    view->xwayland.unmap.notify = handle_xwayland_unmap;
    wl_signal_add(&xsurface->surface->events.unmap, &view->xwayland.unmap);
}

static void handle_xwayland_dissociate(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, xwayland.dissociate);
    (void)data;
    listener_remove(&view->xwayland.map);
    listener_remove(&view->xwayland.unmap);
    if (view->scene_tree) {
        wlr_scene_node_destroy(&view->scene_tree->node);
        view->scene_tree = NULL;
    }
}

static void handle_xwayland_map(struct wl_listener *listener, void *data) {
	View *view = wl_container_of(listener, view, xwayland.map);
	(void)data;
	if (!view || !view->server || view->server->shutting_down) return;

	view->mapped = true;

	animation_set_target(&view->opacity, 0.0, 0, EASING_LINEAR, NULL);
	view_apply_opacity(view);

	if (view->scene_tree) {
		wlr_scene_node_set_enabled(&view->scene_tree->node, true);
	}

	server_view_mapped(view->server, view);
	view_play_open_animation(view);
}

static void handle_xwayland_unmap(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, xwayland.unmap);
    (void)data;
    if (!view || !view->server) return;
    if (view->server->shutting_down) return;
    if (!view->mapped) return;
    view->mapped = false;
    server_view_unmapped(view->server, view);
    if (view->scene_tree) {
        wlr_scene_node_set_enabled(&view->scene_tree->node, false);
    }
}

static void handle_xwayland_request_configure(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, xwayland.request_configure);
    struct wlr_xwayland_surface_configure_event *event = data;
    if (!view || !view->xwayland.xsurface || !event) return;
    if (!view->xwayland.xsurface->surface) return;
    if (view->floating || !view->tiled) {
        wlr_xwayland_surface_configure(event->surface, event->x, event->y, event->width, event->height);
        int rel_x = event->x;
        int rel_y = event->y;
        if (view->output) {
            rel_x -= view->output->x;
            rel_y -= view->output->y;
        }
        view_set_geometry(view, rel_x, rel_y, event->width, event->height);
    } else {
        wlr_xwayland_surface_configure(event->surface, view->x, view->y, view->width, view->height);
    }
}

static void handle_xwayland_request_activate(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, xwayland.request_activate);
    (void)data;
    if (view && view->mapped && view->server) {
        server_focus_view(view->server, view);
    }
}

static void handle_xwayland_request_close(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, xwayland.request_close);
    (void)data;
    if (view) {
        view_close(view);
    }
}

static void handle_xwayland_destroy(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, xwayland.destroy);
    (void)data;
    if (view) {
        view_destroy(view);
    }
}

static void handle_xwayland_request_fullscreen(struct wl_listener *listener, void *data) {
    (void)data;
    View *view = wl_container_of(listener, view, xwayland.request_fullscreen);
    if (!view || !view->server || view->server->shutting_down) return;
    if (!view->mapped || !view->xwayland.xsurface) return;
    bool requested = view->xwayland.xsurface->fullscreen;
    if (requested && !view->fullscreen) {
        view_toggle_fullscreen(view);
    } else if (!requested && view->fullscreen) {
        view_toggle_fullscreen(view);
    }
}

View *view_create_xwayland(Server *server, struct wlr_xwayland_surface *xsurface) {
    View *view = calloc(1, sizeof(View));
    if (!view) return NULL;
    view->server = server;
    view->type = VIEW_TYPE_XWAYLAND;
    view->xwayland.xsurface = xsurface;
    view->width = xsurface->width > 0 ? xsurface->width : 800;
    view->height = xsurface->height > 0 ? xsurface->height : 600;
    struct wlr_scene_tree *view_parent = server->tiled_tree;
    if (!view_parent) {
	view_parent = server->view_tree;
    }
    if (!view_parent) {
	view_parent = server->scene_tree;
    }
    view->root_tree = wlr_scene_tree_create(view_parent);
    if (!view->root_tree) { free(view); return NULL; }
    for (int i = 0; i < 4; i++) {
        view->border[i] = wlr_scene_rect_create(view->root_tree, 0, 0, server->config.border_color_unfocused);
        if (!view->border[i]) { wlr_scene_node_destroy(&view->root_tree->node); free(view); return NULL; }
        wlr_scene_node_set_enabled(&view->border[i]->node, false);
    }
    animation_init(&view->opacity, 0.0);
    animation_init(&view->anim_x, 0.0);
    animation_init(&view->anim_y, 0.0);
    animation_init(&view->anim_w, 0.0);
    animation_init(&view->anim_h, 0.0);
    animation_init(&view->anim_scale, 1.0);
    animation_init(&view->border_blend, 0.0);
    xsurface->data = view;
	wl_list_init(&view->link);
	wl_list_init(&view->scene_node_destroy.link);
	wl_list_init(&view->decoration_destroy.link);
	wl_list_init(&view->decoration_request_mode.link);
	wl_list_init(&view->set_title.link);
	wl_list_init(&view->set_app_id.link);
	wl_list_init(&view->xwayland.associate.link);
    wl_list_init(&view->xwayland.dissociate.link);
    wl_list_init(&view->xwayland.request_configure.link);
    wl_list_init(&view->xwayland.request_activate.link);
    wl_list_init(&view->xwayland.request_close.link);
    wl_list_init(&view->xwayland.request_fullscreen.link);
    wl_list_init(&view->xwayland.destroy.link);
    wl_list_init(&view->xwayland.map.link);
    wl_list_init(&view->xwayland.unmap.link);

    view->xwayland.associate.notify = handle_xwayland_associate;
    wl_signal_add(&xsurface->events.associate, &view->xwayland.associate);

    view->xwayland.dissociate.notify = handle_xwayland_dissociate;
    wl_signal_add(&xsurface->events.dissociate, &view->xwayland.dissociate);

    view->xwayland.request_configure.notify = handle_xwayland_request_configure;
    wl_signal_add(&xsurface->events.request_configure, &view->xwayland.request_configure);

    view->xwayland.request_activate.notify = handle_xwayland_request_activate;
    wl_signal_add(&xsurface->events.request_activate, &view->xwayland.request_activate);

    view->xwayland.request_close.notify = handle_xwayland_request_close;
    wl_signal_add(&xsurface->events.request_close, &view->xwayland.request_close);

    view->xwayland.request_fullscreen.notify = handle_xwayland_request_fullscreen;
    wl_signal_add(&xsurface->events.request_fullscreen, &view->xwayland.request_fullscreen);

    view->xwayland.destroy.notify = handle_xwayland_destroy;
    wl_signal_add(&xsurface->events.destroy, &view->xwayland.destroy);
    wl_list_insert(&server->views, &view->link);
    return view;
}

#endif

View *view_create_xdg(Server *server, struct wlr_xdg_surface *xdg_surface, struct wlr_xdg_toplevel *toplevel) {
    View *view = calloc(1, sizeof(View));
    if (!view) return NULL;

    view->server = server;
    view->type = VIEW_TYPE_XDG;
    view->xdg.xdg_surface = xdg_surface;
    view->xdg.toplevel = toplevel;
    view->xdg.listeners_initialized = false;

    struct wlr_scene_tree *view_parent = server->tiled_tree;
    if (!view_parent) {
	view_parent = server->view_tree;
    }
    if (!view_parent) {
	view_parent = server->scene_tree;
    }
    view->root_tree = wlr_scene_tree_create(view_parent);
    if (!view->root_tree) {
        free(view);
        return NULL;
    }

    for (int i = 0; i < 4; i++) {
        view->border[i] = wlr_scene_rect_create(view->root_tree, 0, 0, server->config.border_color_unfocused);
        if (!view->border[i]) {
            wlr_scene_node_destroy(&view->root_tree->node);
            free(view);
            return NULL;
        }

        wlr_scene_node_set_enabled(&view->border[i]->node, false);
    }

    view->scene_tree = wlr_scene_xdg_surface_create(view->root_tree, xdg_surface);
    if (!view->scene_tree) {
        wlr_scene_node_destroy(&view->root_tree->node);
        free(view);
        return NULL;
    }

    animation_init(&view->opacity, 0.0);
    animation_init(&view->anim_x, 0.0);
    animation_init(&view->anim_y, 0.0);
    animation_init(&view->anim_w, 0.0);
    animation_init(&view->anim_h, 0.0);
    animation_init(&view->anim_scale, 1.0);
    animation_init(&view->border_blend, 0.0);
    xdg_surface->data = view;
    wl_list_init(&view->link);
    wl_list_init(&view->scene_node_destroy.link);
    wl_list_init(&view->decoration_destroy.link);
    wl_list_init(&view->decoration_request_mode.link);
    wl_list_init(&view->xdg.map.link);
    wl_list_init(&view->xdg.unmap.link);
    wl_list_init(&view->xdg.destroy.link);
    wl_list_init(&view->xdg.commit.link);
    wl_list_init(&view->xdg.toplevel_destroy.link);
	wl_list_init(&view->xdg.request_fullscreen.link);
	wl_list_init(&view->xdg.request_move.link);
    wl_list_init(&view->xdg.request_resize.link);
    wl_list_init(&view->xdg.new_popup.link);
    wl_list_init(&view->set_title.link);
    wl_list_init(&view->set_app_id.link);

    view->scene_node_destroy.notify = handle_scene_node_destroy;
    wl_signal_add(&view->scene_tree->node.events.destroy, &view->scene_node_destroy);

    view->xdg.map.notify = handle_map;
    wl_signal_add(&xdg_surface->surface->events.map, &view->xdg.map);

    view->xdg.unmap.notify = handle_unmap;
    wl_signal_add(&xdg_surface->surface->events.unmap, &view->xdg.unmap);

    view->xdg.destroy.notify = handle_destroy;
    wl_signal_add(&xdg_surface->events.destroy, &view->xdg.destroy);

    view->xdg.commit.notify = handle_commit;
    wl_signal_add(&xdg_surface->surface->events.commit, &view->xdg.commit);

    view->xdg.toplevel_destroy.notify = handle_toplevel_destroy;
	wl_signal_add(&toplevel->events.destroy, &view->xdg.toplevel_destroy);
	view->xdg.request_fullscreen.notify = handle_request_fullscreen;
	wl_signal_add(&toplevel->events.request_fullscreen, &view->xdg.request_fullscreen);
	view->xdg.request_move.notify = handle_request_move;

    view->xdg.request_move.notify = handle_request_move;
    wl_signal_add(&toplevel->events.request_move, &view->xdg.request_move);

    view->xdg.request_resize.notify = handle_request_resize;
    wl_signal_add(&toplevel->events.request_resize, &view->xdg.request_resize);
    view->xdg.new_popup.notify = handle_new_popup;
    wl_signal_add(&xdg_surface->events.new_popup, &view->xdg.new_popup);
    view->set_title.notify = handle_set_title;
    wl_signal_add(&toplevel->events.set_title, &view->set_title);
    view->set_app_id.notify = handle_set_app_id;
    wl_signal_add(&toplevel->events.set_app_id, &view->set_app_id);
    view->xdg.listeners_initialized = true;

    wl_list_insert(&server->views, &view->link);

   	bool transient = toplevel->parent != NULL;
    if (!transient && xdg_surface->initialized) {
        struct wlr_box geo = xdg_surface->current.geometry;
        if (geo.width <= 0 || geo.height <= 0) {
            wlr_xdg_toplevel_set_size(toplevel, 800, 600);
        }
    }
	return view;
}

void view_destroy(View *view) {
    if (!view) return;

    Server *server = view->server;

    if (server && server->drag.view == view) {
        server->drag.active = false;
        server->drag.move = false;
        server->drag.resize = false;
        server->drag.view = NULL;
    }

    if (server && !server->shutting_down) {
        server_view_destroyed(server, view);
    }

   	if (view->type == VIEW_TYPE_XDG && view->xdg.listeners_initialized) {
		listener_remove(&view->decoration_request_mode);
		listener_remove(&view->xdg.map);
		listener_remove(&view->xdg.unmap);
		listener_remove(&view->xdg.destroy);
		listener_remove(&view->xdg.commit);
		listener_remove(&view->xdg.toplevel_destroy);
		listener_remove(&view->xdg.request_fullscreen);
		listener_remove(&view->xdg.request_move);
		listener_remove(&view->xdg.request_resize);
		listener_remove(&view->xdg.new_popup);
		listener_remove(&view->set_title);
		listener_remove(&view->set_app_id);
		view->xdg.listeners_initialized = false;
	}
   #if defined(WYO_HAS_XWAYLAND)
	if (view->type == VIEW_TYPE_XWAYLAND) {
		listener_remove(&view->xwayland.associate);
		listener_remove(&view->xwayland.dissociate);
		listener_remove(&view->xwayland.request_configure);
		listener_remove(&view->xwayland.request_activate);
		listener_remove(&view->xwayland.request_close);
		listener_remove(&view->xwayland.request_fullscreen);
		listener_remove(&view->xwayland.destroy);
		listener_remove(&view->xwayland.map);
		listener_remove(&view->xwayland.unmap);
	}
   #endif
    view_foreign_destroy(view);

    if (view->scene_tree) {
        listener_remove(&view->scene_node_destroy);
        view->scene_tree = NULL;
    } else {
        listener_remove(&view->scene_node_destroy);
    }

    listener_remove(&view->decoration_destroy);
    view->decoration = NULL;
    view->decoration_mode_set = false;

    if (view->root_tree) {
        wlr_scene_node_destroy(&view->root_tree->node);
        view->root_tree = NULL;

        for (int i = 0; i < 4; i++) {
            view->border[i] = NULL;
        }
    }

    if (view->type == VIEW_TYPE_XDG && view->xdg.xdg_surface) {
        view->xdg.xdg_surface->data = NULL;
        view->xdg.xdg_surface = NULL;
    }

    view->xdg.toplevel = NULL;

    if (!wl_list_empty(&view->link)) {
        wl_list_remove(&view->link);
        wl_list_init(&view->link);
    }

    free(view);
}

void view_cleanup_for_shutdown(View *view) {
    if (!view) return;

    Server *server = view->server;

    if (server && server->drag.view == view) {
        server->drag.active = false;
        server->drag.move = false;
        server->drag.resize = false;
        server->drag.view = NULL;
    }

   	if (view->type == VIEW_TYPE_XDG && view->xdg.listeners_initialized) {
		listener_remove(&view->decoration_request_mode);
		listener_remove(&view->xdg.map);
		listener_remove(&view->xdg.unmap);
		listener_remove(&view->xdg.destroy);
		listener_remove(&view->xdg.commit);
		listener_remove(&view->xdg.toplevel_destroy);
		listener_remove(&view->xdg.request_fullscreen);
		listener_remove(&view->xdg.request_move);
		listener_remove(&view->xdg.request_resize);
		listener_remove(&view->xdg.new_popup);
		listener_remove(&view->set_title);
		listener_remove(&view->set_app_id);
		view->xdg.listeners_initialized = false;
	}
   #if defined(WYO_HAS_XWAYLAND)
	if (view->type == VIEW_TYPE_XWAYLAND) {
		listener_remove(&view->xwayland.associate);
		listener_remove(&view->xwayland.dissociate);
		listener_remove(&view->xwayland.request_configure);
		listener_remove(&view->xwayland.request_activate);
		listener_remove(&view->xwayland.request_close);
		listener_remove(&view->xwayland.request_fullscreen);
		listener_remove(&view->xwayland.destroy);
		listener_remove(&view->xwayland.map);
		listener_remove(&view->xwayland.unmap);
	}
   #endif
    view_foreign_destroy(view);

    if (view->scene_tree) {
        listener_remove(&view->scene_node_destroy);
        view->scene_tree = NULL;
    } else {
        listener_remove(&view->scene_node_destroy);
    }

    listener_remove(&view->decoration_destroy);
    view->decoration = NULL;
    view->decoration_mode_set = false;

    if (view->root_tree) {
        wlr_scene_node_destroy(&view->root_tree->node);
        view->root_tree = NULL;

        for (int i = 0; i < 4; i++) {
            view->border[i] = NULL;
        }
    }

    if (view->type == VIEW_TYPE_XDG && view->xdg.xdg_surface) {
        view->xdg.xdg_surface->data = NULL;
    }

    if (!wl_list_empty(&view->link)) {
        wl_list_remove(&view->link);
        wl_list_init(&view->link);
    }

    free(view);
}

void view_update_scene_parent(View *view) {
	if (!view || !view->root_tree || !view->server) return;

	struct wlr_scene_tree *desired = NULL;

	if (view->fullscreen) {
		desired = view->server->fullscreen_tree;
	} else if (view->floating) {
		desired = view->server->floating_tree;
	} else {
		desired = view->server->tiled_tree;
	}

	if (!desired) {
		desired = view->server->view_tree;
	}

	if (!desired) {
		desired = view->server->scene_tree;
	}

	if (!desired) return;

	if (view->root_tree->node.parent != desired) {
		wlr_scene_node_reparent(&view->root_tree->node, desired);
	}
}

void view_set_geometry(View *view, int x, int y, int width, int height) {
	if (!view) return;

	int offset_x = 0;
	int offset_y = 0;

	if (view->output) {
		offset_x = view->output->x;
		offset_y = view->output->y;
	}

	int new_x = x + offset_x;
	int new_y = y + offset_y;

	int old_x = view->x;
	int old_y = view->y;
	int old_width = view->width;
	int old_height = view->height;

	bool size_changed = old_width != width || old_height != height;
	bool target_pos_changed = old_x != new_x || old_y != new_y;
	bool first = view->target_width <= 0 || view->target_height <= 0;

	double new_cx = (double)new_x + (double)width * 0.5;
	double new_cy = (double)new_y + (double)height * 0.5;

	int target_cx = (int)lround(new_cx);
	int target_cy = (int)lround(new_cy);

	int visual_cx = (int)lround(view->anim_x.current);
	int visual_cy = (int)lround(view->anim_y.current);
	int visual_w = (int)lround(view->anim_w.current);
	int visual_h = (int)lround(view->anim_h.current);

	bool visual_center_changed = visual_cx != target_cx || visual_cy != target_cy;
	bool visual_changed = visual_center_changed || visual_w != width || visual_h != height;

	bool anim_running = view->anim_x.active ||
		view->anim_y.active ||
		view->anim_w.active ||
		view->anim_h.active;

	view->x = new_x;
	view->y = new_y;
	view->width = width;
	view->height = height;
	view->target_x = new_x;
	view->target_y = new_y;
	view->target_width = width;
	view->target_height = height;

	int duration = 160;
	EasingCurve curve = EASING_EASE_OUT;
	const BezierCurve *bezier = NULL;

	if (view->server) {
		for (int i = 0; i < view->server->config.anim_rule_count; i++) {
			if (strcmp(view->server->config.anim_rules[i].type, "windows") == 0 &&
				view->server->config.anim_rules[i].enabled) {
				duration = view->server->config.anim_rules[i].duration_ms;

				for (int j = 0; j < view->server->config.bezier_count; j++) {
					if (strcmp(view->server->config.beziers[j].name,
							   view->server->config.anim_rules[i].curve_name) == 0) {
						bezier = &view->server->config.beziers[j].curve;
						curve = EASING_BEZIER;
						break;
					}
				}

				if (!bezier) {
					curve = (EasingCurve)view->server->config.anim_window_curve;
				}

				break;
			}
		}
	}

	if (duration < 0) {
		duration = 0;
	}

	bool animate = false;

	if (duration > 0 && view->mapped && !first && !view->dragging) {
		if (target_pos_changed || size_changed || visual_changed) {
			animate = true;
		}

		if (view->server &&
			(view->server->shutting_down || view->server->drag.view == view)) {
			animate = false;
		}
	}

	if (animate) {
		animation_set_target(&view->anim_x, new_cx, duration, curve, bezier);
		animation_set_target(&view->anim_y, new_cy, duration, curve, bezier);
		animation_set_target(&view->anim_w, (double)width, duration, curve, bezier);
		animation_set_target(&view->anim_h, (double)height, duration, curve, bezier);

		if (view->output) {
			wlr_output_schedule_frame(view->output->wlr_output);
		}
	} else if (visual_changed || !anim_running) {
		animation_set_target(&view->anim_x, new_cx, 0, EASING_LINEAR, NULL);
		animation_set_target(&view->anim_y, new_cy, 0, EASING_LINEAR, NULL);
		animation_set_target(&view->anim_w, (double)width, 0, EASING_LINEAR, NULL);
		animation_set_target(&view->anim_h, (double)height, 0, EASING_LINEAR, NULL);
	}

	if (view->type == VIEW_TYPE_XDG &&
		view->xdg.toplevel &&
		view->xdg.xdg_surface &&
		view->xdg.xdg_surface->initialized &&
		size_changed) {
		wlr_xdg_toplevel_set_size(view->xdg.toplevel, width, height);
	}
#if defined(WYO_HAS_XWAYLAND)
	else if (view->type == VIEW_TYPE_XWAYLAND &&
			 view->xwayland.xsurface &&
			 view->xwayland.xsurface->surface &&
			 size_changed) {
		wlr_xwayland_surface_configure(view->xwayland.xsurface, view->x, view->y, width, height);
	}
#endif

	view_apply_geometry(view);
	view_update_border(view);
	view_foreign_sync(view);
	view_update_scene_parent(view);
}

void view_set_opacity(View *view, double opacity) {
    if (!view) return;

    animation_set_target(&view->opacity, opacity, 0, EASING_LINEAR, NULL);
    view_apply_opacity(view);
}

void view_focus(View *view) {
    if (!view) return;

    if (view->root_tree) {
        wlr_scene_node_raise_to_top(&view->root_tree->node);
    } else if (view->scene_tree) {
        wlr_scene_node_raise_to_top(&view->scene_tree->node);
    }

    if (view->type == VIEW_TYPE_XDG && view->xdg.toplevel && view->xdg.xdg_surface && view->xdg.xdg_surface->initialized) {
        wlr_xdg_toplevel_set_activated(view->xdg.toplevel, true);
    }

    #if defined(WYO_HAS_XWAYLAND)
        else if (view->type == VIEW_TYPE_XWAYLAND && view->xwayland.xsurface) {
            wlr_xwayland_surface_activate(view->xwayland.xsurface, true);
        }
    #endif

   	int duration = 160;
	EasingCurve curve = EASING_EASE_OUT;
	const BezierCurve *bezier = NULL;
	if (view->server && !view->server->shutting_down) {
		for (int i = 0; i < view->server->config.anim_rule_count; i++) {
			if (strcmp(view->server->config.anim_rules[i].type, "fade") == 0 && view->server->config.anim_rules[i].enabled) {
				duration = view->server->config.anim_rules[i].duration_ms;
				for (int j = 0; j < view->server->config.bezier_count; j++) {
					if (strcmp(view->server->config.beziers[j].name, view->server->config.anim_rules[i].curve_name) == 0) {
						bezier = &view->server->config.beziers[j].curve;
						curve = EASING_BEZIER;
						break;
					}
				}
				if (!bezier) curve = (EasingCurve)view->server->config.anim_opacity_curve;
				break;
			}
		}
	} else {
		duration = 0;
	}
	animation_set_target(&view->border_blend, 1.0, duration, curve, bezier);
    view_update_border(view);
    view_foreign_sync(view);

    if (view->output) {
        wlr_output_schedule_frame(view->output->wlr_output);
    }
}

void view_unfocus(View *view) {
    if (!view) return;

    if (view->type == VIEW_TYPE_XDG && view->xdg.toplevel && view->xdg.xdg_surface && view->xdg.xdg_surface->initialized) {
        wlr_xdg_toplevel_set_activated(view->xdg.toplevel, false);
    }

    #if defined(WYO_HAS_XWAYLAND)
        else if (view->type == VIEW_TYPE_XWAYLAND && view->xwayland.xsurface) {
            wlr_xwayland_surface_activate(view->xwayland.xsurface, false);
        }
    #endif

   	int duration = 160;
	EasingCurve curve = EASING_EASE_OUT;
	const BezierCurve *bezier = NULL;
	if (view->server && !view->server->shutting_down) {
		for (int i = 0; i < view->server->config.anim_rule_count; i++) {
			if (strcmp(view->server->config.anim_rules[i].type, "fade") == 0 && view->server->config.anim_rules[i].enabled) {
				duration = view->server->config.anim_rules[i].duration_ms;
				for (int j = 0; j < view->server->config.bezier_count; j++) {
					if (strcmp(view->server->config.beziers[j].name, view->server->config.anim_rules[i].curve_name) == 0) {
						bezier = &view->server->config.beziers[j].curve;
						curve = EASING_BEZIER;
						break;
					}
				}
				if (!bezier) curve = (EasingCurve)view->server->config.anim_opacity_curve;
				break;
			}
		}
	} else {
		duration = 0;
	}
	animation_set_target(&view->border_blend, 0.0, duration, curve, bezier);
    view_update_border(view);
    view_foreign_sync(view);

    if (view->output) {
        wlr_output_schedule_frame(view->output->wlr_output);
    }
}

static void handle_toplevel_destroy(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, xdg.toplevel_destroy);
    (void)data;
    if (!view->xdg.listeners_initialized) return;
    listener_remove(&view->xdg.request_fullscreen);
	listener_remove(&view->xdg.request_move);
	listener_remove(&view->xdg.request_resize);
	listener_remove(&view->xdg.toplevel_destroy);
    listener_remove(&view->set_title);
    listener_remove(&view->set_app_id);
    view->xdg.toplevel = NULL;
}

static bool app_id_in_sticky_list(const char *app_id, const char *sticky_apps) {
    if (!app_id || !*app_id || !sticky_apps || !*sticky_apps) return false;
    char *copy = strdup(sticky_apps);
    if (!copy) return false;
    bool found = false;
    char *saveptr = NULL;
    for (char *tok = strtok_r(copy, ",", &saveptr); tok; tok = strtok_r(NULL, ",", &saveptr)) {
        while (*tok && isspace((unsigned char)*tok)) tok++;
        char *end = tok + strlen(tok) - 1;
        while (end > tok && isspace((unsigned char)*end)) *end-- = '\0';
        if (*tok && strcasecmp(tok, app_id) == 0) {
            found = true;
            break;
        }
    }
    free(copy);
    return found;
}

static void check_sticky(View *view) {
	if (!view || !view->server || view->sticky || view->server->shutting_down) return;
	if (view->type != VIEW_TYPE_XDG || !view->xdg.toplevel) return;
	const char *app_id = view->xdg.toplevel->app_id;
	if (!app_id_in_sticky_list(app_id, view->server->config.sticky_apps)) return;

	view->sticky = true;
	view->floating = true;

	if (view->workspace) {
		workspace_remove_view(view->workspace, view);
		view->workspace = NULL;
	}
}

static void view_play_open_animation(View *view) {
	if (!view || !view->server || view->server->shutting_down) return;

	double target_opacity = view->server->config.active_opacity;
	if (view->server->focused_view != view) {
		target_opacity = view->server->config.inactive_opacity;
	}
	if (target_opacity < 0.0) target_opacity = 0.0;
	if (target_opacity > 1.0) target_opacity = 1.0;

	int fade_ms = 250;
	int win_ms = 208;
	EasingCurve fade_curve = EASING_EASE_OUT;
	EasingCurve win_curve = EASING_EASE_OUT;
	const BezierCurve *fade_bez = NULL;
	const BezierCurve *win_bez = NULL;

	for (int i = 0; i < view->server->config.anim_rule_count; i++) {
		const ConfigAnimRule *rule = &view->server->config.anim_rules[i];
		if (!rule->enabled) continue;

		if (strcmp(rule->type, "fade") == 0) {
			fade_ms = rule->duration_ms;
			for (int j = 0; j < view->server->config.bezier_count; j++) {
				if (strcmp(view->server->config.beziers[j].name, rule->curve_name) == 0) {
					fade_bez = &view->server->config.beziers[j].curve;
					fade_curve = EASING_BEZIER;
					break;
				}
			}
		} else if (strcmp(rule->type, "windows") == 0) {
			win_ms = rule->duration_ms;
			for (int j = 0; j < view->server->config.bezier_count; j++) {
				if (strcmp(view->server->config.beziers[j].name, rule->curve_name) == 0) {
					win_bez = &view->server->config.beziers[j].curve;
					win_curve = EASING_BEZIER;
					break;
				}
			}
		}
	}

	if (fade_ms < 0) fade_ms = 0;
	if (win_ms < 0) win_ms = 0;

	animation_set_target(&view->opacity, 0.0, 0, EASING_LINEAR, NULL);
	view_apply_opacity(view);

	animation_set_target(&view->anim_scale, 0.92, 0, EASING_LINEAR, NULL);
	view_apply_geometry(view);

	animation_set_target(&view->opacity, target_opacity, fade_ms, fade_curve, fade_bez);
	animation_set_target(&view->anim_scale, 1.0, win_ms, win_curve, win_bez);

	if (view->output) {
		wlr_output_schedule_frame(view->output->wlr_output);
	}
}

static void handle_map(struct wl_listener *listener, void *data) {
	View *view = wl_container_of(listener, view, xdg.map);
	(void)data;
	if (!view || !view->server || view->server->shutting_down) return;

	view->mapped = true;

	animation_set_target(&view->opacity, 0.0, 0, EASING_LINEAR, NULL);
	view_apply_opacity(view);

	if (view->scene_tree) {
		wlr_scene_node_set_enabled(&view->scene_tree->node, true);
	}

	server_view_mapped(view->server, view);

	if (view->xdg.toplevel && view->xdg.toplevel->requested.fullscreen && !view->fullscreen) {
		view_toggle_fullscreen(view);
	}

	view_foreign_create(view);
	view_foreign_sync(view);
	view_refresh_decorations(view);
	check_sticky(view);
	view_play_open_animation(view);
}

static void handle_unmap(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, xdg.unmap);
    (void)data;

    if (!view || !view->server) return;
    if (view->server->shutting_down) return;
    if (!view->mapped) return;

    view->mapped = false;

    if (view->fullscreen) {
        view->fullscreen = false;
        view->saved_tiled = false;
    }

    server_view_unmapped(view->server, view);
    view_foreign_destroy(view);
    if (view->scene_tree) {
        wlr_scene_node_set_enabled(&view->scene_tree->node, false);
    }

    view_refresh_decorations(view);
}

static void handle_commit(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, xdg.commit);
    (void)data;
    if (!view || !view->server || view->server->shutting_down) return;

    bool transient_commit = view->xdg.toplevel && view->xdg.toplevel->parent != NULL;
    if (!transient_commit &&
        !view->mapped &&
        view->xdg.xdg_surface &&
        view->xdg.xdg_surface->initialized &&
        view->xdg.toplevel &&
        view->width == 0 &&
        view->height == 0) {
        struct wlr_box geo = view->xdg.xdg_surface->current.geometry;
        if (geo.width <= 0 || geo.height <= 0) {
            wlr_xdg_toplevel_set_size(view->xdg.toplevel, 800, 600);
        }
    }

    if (view->decoration && !view->decoration_mode_set && view->xdg.xdg_surface && view->xdg.xdg_surface->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(view->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        view->decoration_mode_set = true;
    }

    if (view->mapped) {
        if (view->floating && !view->fullscreen && view->xdg.xdg_surface) {
            struct wlr_box geo = view->xdg.xdg_surface->current.geometry;
            if (geo.width > 0 && geo.height > 0 &&
                (geo.width != view->width || geo.height != view->height)) {
                view->width = geo.width;
                view->height = geo.height;
                view->target_width = geo.width;
                view->target_height = geo.height;
            }
        }
        view_foreign_sync(view);
        view_apply_geometry(view);
        view_refresh_decorations(view);
    }
}

void view_close(View *view) {
    if (!view || !view->mapped) return;
    if (view->type == VIEW_TYPE_XDG) {
        if (!view->xdg.xdg_surface || !view->xdg.toplevel) return;
        wlr_xdg_toplevel_send_close(view->xdg.toplevel);
    } else if (view->type == VIEW_TYPE_XWAYLAND) {
#if defined(WYO_HAS_XWAYLAND)
        if (!view->xwayland.xsurface) return;
        wlr_xwayland_surface_close(view->xwayland.xsurface);
#endif
    }
}

void view_toggle_fullscreen(View *view) {
	if (!view || !view->server || view->server->shutting_down) return;
	if (!view->output) return;

	Output *output = view->output;
	Server *server = view->server;

	if (view->fullscreen) {
		view->fullscreen = false;
		if (view->type == VIEW_TYPE_XDG && view->xdg.toplevel && view->xdg.xdg_surface && view->xdg.xdg_surface->initialized) {
			wlr_xdg_toplevel_set_fullscreen(view->xdg.toplevel, false);
		}

		if (view->saved_tiled) {
			view->saved_tiled = false;
			if (!view->tiled && view->workspace) {
				dwindle_add_view(&view->workspace->layout, view);
				view->tiled = true;
				server_arrange(server);
			}
		} else {
			int width = view->saved_width > 0 ? view->saved_width : view->width;
			int height = view->saved_height > 0 ? view->saved_height : view->height;
			if (width <= 0) width = 800;
			if (height <= 0) height = 600;
			view_set_geometry(view, view->saved_x, view->saved_y, width, height);
		}

		view_update_scene_parent(view);
		if (view->root_tree) {
			wlr_scene_node_raise_to_top(&view->root_tree->node);
		}
		server_arrange(server);
		return;
	}

	if (!view->mapped) return;

	view->saved_x = view->x - output->x;
	view->saved_y = view->y - output->y;
	view->saved_width = view->width;
	view->saved_height = view->height;
	view->saved_tiled = view->tiled;

	if (view->tiled && view->workspace) {
		dwindle_remove_view(&view->workspace->layout, view);
		view->tiled = false;
	}

	view->fullscreen = true;
	if (view->type == VIEW_TYPE_XDG && view->xdg.toplevel && view->xdg.xdg_surface && view->xdg.xdg_surface->initialized) {
		wlr_xdg_toplevel_set_fullscreen(view->xdg.toplevel, true);
	}

	view_set_geometry(view, 0, 0, output->width, output->height);

	if (view->root_tree && server->fullscreen_tree) {
		wlr_scene_node_reparent(&view->root_tree->node, server->fullscreen_tree);
		wlr_scene_node_raise_to_top(&view->root_tree->node);
	}

	server_arrange(server);
}

void view_center(View *view) {
	if (!view || !view->server || view->server->shutting_down) return;
	if (!view->output || view->fullscreen) return;

	Output *output = view->output;

	if (view->tiled && view->workspace) {
		dwindle_remove_view(&view->workspace->layout, view);
		view->tiled = false;
		view->floating = true;
	}

	int width = view->width > 0 ? view->width : 800;
	int height = view->height > 0 ? view->height : 600;

	int x = (output->width - width) / 2;
	int y = (output->height - height) / 2;

	if (x < 0) x = 0;
	if (y < 0) y = 0;

	view_set_geometry(view, x, y, width, height);
}

void view_move_by(View *view, int dx, int dy) {
    if (!view || view->fullscreen) return;
    if (view->width <= 0 || view->height <= 0) return;

    int offset_x = 0;
    int offset_y = 0;

    if (view->output) {
        offset_x = view->output->x;
        offset_y = view->output->y;
    }

    int rel_x = view->x - offset_x + dx;
    int rel_y = view->y - offset_y + dy;

    view_set_geometry(view, rel_x, rel_y, view->width, view->height);
}

bool view_frame_update(View *view, int64_t now_ms) {
    if (!view) return false;

    bool active = false;

   	if (view->anim_x.active || view->anim_y.active || view->anim_w.active || view->anim_h.active || view->anim_scale.active) {
		animation_update(&view->anim_x, now_ms);
		animation_update(&view->anim_y, now_ms);
		animation_update(&view->anim_w, now_ms);
		animation_update(&view->anim_h, now_ms);
		animation_update(&view->anim_scale, now_ms);
		view_apply_geometry(view);
		view_update_border(view);
		active = true;
	}

    if (view->opacity.active) {
        animation_update(&view->opacity, now_ms);
        view_apply_opacity(view);
        active = true;
    }

    if (view->border_blend.active) {
        animation_update(&view->border_blend, now_ms);
        view_update_border(view);
        active = true;
    }

    return active;
}
