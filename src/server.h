#ifndef SERVER_H
#define SERVER_H

#include <stdbool.h>
#include <wayland-server.h>
#include <wlr/backend.h>
#include <wlr/backend/session.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>
#if __has_include(<wlr/types/wlr_xdg_output_v1.h>)
#include <wlr/types/wlr_xdg_output_v1.h>
#elif __has_include(<wlr/types/wlr_xdg_output_manager_v1.h>)
#include <wlr/types/wlr_xdg_output_manager_v1.h>
#endif

#if __has_include(<wlr/types/wlr_foreign_toplevel_management_v1.h>)
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#define WYO_HAS_FOREIGN_TOPLEVEL 1
#endif
#include "arena.h"
#include "ipc.h"
#include "config.h"
#include "view.h"
#include "output.h"
#include "layout.h"

struct Server;
struct View;
struct wlr_surface;
struct wlr_layer_shell_v1;
struct wlr_buffer;

typedef struct DragState {
    bool active;
    bool move;
    bool resize;
    bool restore_tiled;
    bool tiled_resize;
    struct View *view;
    struct Output *output;
    double grab_x;
    double grab_y;
    double last_x;
    double last_y;
    int orig_x;
    int orig_y;
    int orig_width;
    int orig_height;
} DragState;

typedef struct Server {
    struct wl_display *display;
    struct wl_event_loop *loop;

    struct wlr_backend *backend;
    struct wlr_session *session;
    struct wlr_renderer *renderer;
    struct wlr_allocator *allocator;

    struct wlr_compositor *compositor;
    struct wlr_subcompositor *subcompositor;
    struct wlr_data_device_manager *data_device_manager;

    struct wlr_output_layout *output_layout;
    struct wlr_scene *scene;
    struct wlr_scene_tree *scene_tree;
    struct wlr_scene_tree *view_tree;
    struct wlr_scene_tree *layer_tree;
    struct wlr_scene_output_layout *scene_layout;
    struct wlr_scene_rect *background;

    struct wlr_xdg_shell *xdg_shell;
    struct wlr_seat *seat;

    struct wlr_cursor *cursor;
    struct wlr_xcursor_manager *xcursor_manager;
    struct wlr_keyboard *active_keyboard;

    struct wl_list outputs;
    struct wl_list views;
    struct wl_list keyboards;
    struct wl_list layer_surfaces;

    View *focused_view;
    struct wlr_surface *focused_surface;
    struct wl_listener focused_surface_destroy;
    Output *active_output;

    DragState drag;

    bool shutting_down;

    struct wl_listener new_output;
    struct wl_listener new_toplevel;
    struct wl_listener new_input;
    struct wl_listener new_layer_surface;

    struct wl_listener cursor_motion;
    struct wl_listener cursor_motion_absolute;
    struct wl_listener cursor_button;
    struct wl_listener cursor_axis;

    Arena arena;
    IPCServer ipc;
    struct wlr_layer_shell_v1 *layer_shell;
    void *xdg_output_manager;
    #if defined(WYO_HAS_FOREIGN_TOPLEVEL)
    struct wlr_foreign_toplevel_manager_v1 *foreign_toplevel_manager;
    #endif

    Config config;
    struct wl_event_source *config_signal;

    struct wlr_buffer *wallpaper_buffer;
    int wallpaper_width;
    int wallpaper_height;
} Server;

bool server_init(Server *server);
void server_run(Server *server);
void server_destroy(Server *server);

void server_view_mapped(Server *server, View *view);
void server_view_unmapped(Server *server, View *view);
void server_view_destroyed(Server *server, View *view);
void server_focus_view(struct Server *server, View *view);
void server_focus_surface(struct Server *server, struct wlr_surface *surface);
void server_arrange(struct Server *server);

#endif
