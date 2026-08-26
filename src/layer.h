#ifndef LAYER_H
#define LAYER_H

#include <stdbool.h>
#include <wayland-server-core.h>
#include <wlr/types/wlr_scene.h>

struct Server;
struct wlr_layer_surface_v1;
struct wlr_scene_layer_surface_v1;

typedef struct LayerSurface {
struct Server *server;
struct wlr_layer_surface_v1 *layer_surface;
struct wlr_scene_layer_surface_v1 *scene;
struct wl_listener map;
struct wl_listener unmap;
struct wl_listener destroy;
struct wl_listener commit;
struct wl_listener new_popup;
struct wl_list link;
bool mapped;
bool initial_configure_sent;
} LayerSurface;

void layer_shell_init(struct Server *server);
void layer_shell_destroy(struct Server *server);
void layer_shell_arrange(struct Server *server);

#endif
