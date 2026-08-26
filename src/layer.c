#include "layer.h"
#include "server.h"
#include "output.h"
#include <stdlib.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/box.h>

static inline void listener_remove(struct wl_listener *listener) {
    if (!wl_list_empty(&listener->link)) {
        wl_list_remove(&listener->link);
        wl_list_init(&listener->link);
    }
}

static void handle_layer_map(struct wl_listener *listener, void *data) {
	LayerSurface *layer = wl_container_of(listener, layer, map);
	(void)data;
	layer->mapped = true;
	if (layer->scene) {
		wlr_scene_node_set_enabled(&layer->scene->tree->node, true);
	}
	if (layer->server && !layer->server->shutting_down && layer->layer_surface && layer->layer_surface->current.keyboard_interactive) {
		server_focus_surface(layer->server, layer->layer_surface->surface);
	}
	server_arrange(layer->server);
}

static void handle_layer_unmap(struct wl_listener *listener, void *data) {
LayerSurface *layer = wl_container_of(listener, layer, unmap);
(void)data;
layer->mapped = false;
if (layer->scene) {
wlr_scene_node_set_enabled(&layer->scene->tree->node, false);
}
if (layer->server && layer->layer_surface &&
    layer->server->focused_surface == layer->layer_surface->surface) {
server_focus_surface(layer->server, NULL);
}
server_arrange(layer->server);
}

static void handle_layer_commit(struct wl_listener *listener, void *data) {
    LayerSurface *layer = wl_container_of(listener, layer, commit);
    (void)data;
    server_arrange(layer->server);
}

static void handle_layer_destroy(struct wl_listener *listener, void *data) {
LayerSurface *layer = wl_container_of(listener, layer, destroy);
(void)data;
if (layer->server && layer->layer_surface &&
    layer->server->focused_surface == layer->layer_surface->surface) {
server_focus_surface(layer->server, NULL);
}
listener_remove(&layer->map);
listener_remove(&layer->unmap);
listener_remove(&layer->destroy);
listener_remove(&layer->commit);
listener_remove(&layer->new_popup);
if (layer->layer_surface) {
layer->layer_surface->data = NULL;
layer->layer_surface = NULL;
}
wl_list_remove(&layer->link);
free(layer);
}

static void handle_new_popup(struct wl_listener *listener, void *data) {
LayerSurface *layer = wl_container_of(listener, layer, new_popup);
struct wlr_xdg_popup *popup = data;
if (!popup || !popup->base || popup->base->data) return;
struct wlr_scene_tree *parent = layer->scene ? layer->scene->tree : layer->server->scene_tree;
struct wlr_scene_tree *popup_tree = wlr_scene_xdg_surface_create(parent, popup->base);
if (popup_tree) {
popup->base->data = popup_tree;
}
}

static void handle_new_layer_surface(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, new_layer_surface);
    struct wlr_layer_surface_v1 *layer_surface = data;

    LayerSurface *layer = calloc(1, sizeof(LayerSurface));
    if (!layer) return;

    layer->server = server;
    layer->layer_surface = layer_surface;
    layer->mapped = false;

    if (!layer_surface->output && server->active_output) {
        layer_surface->output = server->active_output->wlr_output;
    }

    struct wlr_scene_tree *parent = server->layer_tree ? server->layer_tree : server->scene_tree;

    layer->scene = wlr_scene_layer_surface_v1_create(parent, layer_surface);
    if (!layer->scene) {
        free(layer);
        return;
    }

    wlr_scene_node_set_enabled(&layer->scene->tree->node, false);

    layer_surface->data = layer;

    wl_list_init(&layer->link);
    wl_list_init(&layer->map.link);
    wl_list_init(&layer->unmap.link);
    wl_list_init(&layer->destroy.link);
    wl_list_init(&layer->commit.link);
    wl_list_init(&layer->new_popup.link);

    layer->map.notify = handle_layer_map;
    wl_signal_add(&layer_surface->surface->events.map, &layer->map);

    layer->unmap.notify = handle_layer_unmap;
    wl_signal_add(&layer_surface->surface->events.unmap, &layer->unmap);

    layer->destroy.notify = handle_layer_destroy;
    wl_signal_add(&layer_surface->events.destroy, &layer->destroy);

    layer->commit.notify = handle_layer_commit;
    wl_signal_add(&layer_surface->surface->events.commit, &layer->commit);

    layer->new_popup.notify = handle_new_popup;
    wl_signal_add(&layer_surface->events.new_popup, &layer->new_popup);

    wl_list_insert(&server->layer_surfaces, &layer->link);
    server_arrange(server);
}

void layer_shell_init(Server *server) {
    wl_list_init(&server->layer_surfaces);

    server->layer_shell = wlr_layer_shell_v1_create(server->display, 5);
    if (!server->layer_shell) return;

    server->new_layer_surface.notify = handle_new_layer_surface;
    wl_signal_add(&server->layer_shell->events.new_surface, &server->new_layer_surface);
}

void layer_shell_destroy(Server *server) {
    listener_remove(&server->new_layer_surface);

    LayerSurface *layer;
    LayerSurface *tmp;

    wl_list_for_each_safe(layer, tmp, &server->layer_surfaces, link) {
        listener_remove(&layer->map);
        listener_remove(&layer->unmap);
        listener_remove(&layer->destroy);
        listener_remove(&layer->commit);
        listener_remove(&layer->new_popup);

        if (layer->layer_surface) {
            layer->layer_surface->data = NULL;
            layer->layer_surface = NULL;
        }

        wl_list_remove(&layer->link);
        free(layer);
    }

    wl_list_init(&server->layer_surfaces);
}

void layer_shell_arrange(Server *server) {
	if (!server) return;
	Output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_box full_area = {
			.x = output->x,
			.y = output->y,
			.width = output->width,
			.height = output->height
		};
		struct wlr_box usable_area = full_area;
		for (uint32_t layer = 0; layer <= 3; layer++) {
			LayerSurface *ls;
			wl_list_for_each(ls, &server->layer_surfaces, link) {
				if (!ls->scene || !ls->layer_surface) continue;
				if (ls->layer_surface->current.layer != layer) continue;
				struct wlr_output *surface_output = ls->layer_surface->output;
				if (!surface_output) {
					Output *chosen = server->active_output ? server->active_output : output;
					if (chosen != output) continue;
					ls->layer_surface->output = chosen->wlr_output;
					surface_output = chosen->wlr_output;
				}
				if (surface_output != output->wlr_output) continue;
				if (!ls->layer_surface->initialized) continue;
				if (ls->mapped) {
					wlr_scene_layer_surface_v1_configure(ls->scene, &full_area, &usable_area);
					wlr_scene_node_raise_to_top(&ls->scene->tree->node);
				} else {
					struct wlr_box dummy = usable_area;
					wlr_scene_layer_surface_v1_configure(ls->scene, &full_area, &dummy);
				}
			}
		}
		if (usable_area.width < 0) usable_area.width = 0;
		if (usable_area.height < 0) usable_area.height = 0;
		output->usable_x = usable_area.x - output->x;
		output->usable_y = usable_area.y - output->y;
		output->usable_width = usable_area.width;
		output->usable_height = usable_area.height;
	}
}
