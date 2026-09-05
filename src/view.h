#ifndef VIEW_H
#define VIEW_H

#include <stdbool.h>
#include <wayland-server.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_scene.h>
#if __has_include(<wlr/types/wlr_foreign_toplevel_management_v1.h>)
#include <wlr/types/wlr_foreign_toplevel_management_v1.h>
#define WYO_HAS_FOREIGN_TOPLEVEL 1
#endif
#include "animation.h"

struct Server;
struct Workspace;

typedef enum {
    VIEW_TYPE_XDG,
    VIEW_TYPE_XWAYLAND
} ViewType;

typedef struct View {
    struct Server *server;
    struct Output *output;
    struct Workspace *workspace;
    ViewType type;
    struct wlr_scene_tree *root_tree;
    struct wlr_scene_tree *scene_tree;
    struct wlr_scene_rect *border[4];
    struct wl_listener scene_node_destroy;
    struct wl_list link;

    int x;
    int y;
    int width;
    int height;

    int target_x;
    int target_y;
    int target_width;
    int target_height;

    int saved_x;
    int saved_y;
    int saved_width;
    int saved_height;
    bool saved_tiled;

    bool mapped;
    bool tiled;
    bool floating;
    bool fullscreen;
    bool urgent;
    bool dragging;
    bool sticky;

    AnimatedValue opacity;
    AnimatedValue anim_x;
    AnimatedValue anim_y;

    union {
        struct {
            struct wlr_xdg_surface *xdg_surface;
            struct wlr_xdg_toplevel *toplevel;
            struct wl_listener map;
            struct wl_listener unmap;
            struct wl_listener destroy;
            struct wl_listener commit;
         	struct wl_listener toplevel_destroy;
			struct wl_listener request_fullscreen;
			struct wl_listener request_move;
            struct wl_listener request_resize;
            struct wl_listener new_popup;
            bool listeners_initialized;
        } xdg;
    };

    struct wlr_xdg_toplevel_decoration_v1 *decoration;
    bool decoration_mode_set;
    struct wl_listener decoration_destroy;
    AnimatedValue border_blend;
#if defined(WYO_HAS_FOREIGN_TOPLEVEL)
    struct wlr_foreign_toplevel_handle_v1 *foreign_handle;
    struct wlr_output *foreign_output;
    bool foreign_activated;
    bool foreign_maximized;
    bool foreign_fullscreen;
    struct wl_listener foreign_activate;
    struct wl_listener foreign_close;
    struct wl_listener foreign_set_fullscreen;
    struct wl_listener foreign_set_maximized;
    struct wl_listener foreign_destroy;
#endif
    struct wl_listener set_title;
    struct wl_listener set_app_id;
} View;

View *view_create_xdg(struct Server *server, struct wlr_xdg_surface *xdg_surface, struct wlr_xdg_toplevel *toplevel);
void view_destroy(View *view);
void view_set_geometry(View *view, int x, int y, int width, int height);
void view_set_opacity(View *view, double opacity);
void view_apply_opacity(View *view);
void view_focus(View *view);
void view_unfocus(View *view);
void view_close(View *view);
void view_cleanup_for_shutdown(View *view);
void view_toggle_fullscreen(View *view);
void view_center(View *view);
void view_move_by(View *view, int dx, int dy);
void view_refresh_decorations(View *view);
bool view_frame_update(View *view, int64_t now_ms);
void view_foreign_sync(View *view);
#endif
