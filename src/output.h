#ifndef OUTPUT_H
#define OUTPUT_H

#include <wayland-server.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include "layout.h"

struct Server;
struct wlr_buffer;
struct Workspace;

typedef struct Output {
    struct Server *server;
    struct wlr_output *wlr_output;
    struct wlr_output_layout_output *layout_output;
    struct wlr_scene_output *scene_output;
    struct wl_list link;

    struct wl_listener frame;
    struct wl_listener destroy;

    int x;
    int y;
    int width;
    int height;
    int usable_x;
    int usable_y;
    int usable_width;
    int usable_height;

    DwindleLayout layout;
    struct Workspace *active_workspace;
    View *focused_view;

    struct wlr_scene_buffer *wallpaper;
    struct wlr_buffer *wallpaper_buffer;
} Output;

Output *output_create(struct Server *server, struct wlr_output *wlr_output);
void output_destroy(Output *output);
void output_update_geometry(Output *output);
Output *output_at(struct Server *server, double lx, double ly);

#endif
