#include "server.h"
#include "input.h"
#include "wallpaper.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include "layer.h"
#include <dlfcn.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/stat.h>
#ifdef __NetBSD__
#include "server_netbsd.h"
#else
#include <sys/signalfd.h>
#endif
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_cursor_shape_v1.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_idle_inhibit_v1.h>
#include <wlr/types/wlr_gamma_control_v1.h>
#include <wlr/types/wlr_buffer.h>
#include <wlr/types/wlr_output_management_v1.h>
#include <wlr/render/drm_format_set.h>
#include <drm_fourcc.h>
#include <wlr/types/wlr_tearing_control_v1.h>

#ifndef __NetBSD__
#ifndef SFD_NONBLOCK
#define SFD_NONBLOCK O_NONBLOCK
#endif

#ifndef SFD_CLOEXEC
#define SFD_CLOEXEC O_CLOEXEC
#endif
#endif

static inline void listener_remove(struct wl_listener *listener) {
    if (!wl_list_empty(&listener->link)) {
        wl_list_remove(&listener->link);
        wl_list_init(&listener->link);
    }
}

static void handle_output_manager_destroy(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, output_manager_destroy);
    (void)data;
    listener_remove(&server->output_manager_apply);
    listener_remove(&server->output_manager_test);
    listener_remove(&server->output_manager_destroy);
    server->output_manager = NULL;
}

static bool server_apply_output_config(Server *server, struct wlr_output_configuration_v1 *config) {
    size_t states_len = 0;
    struct wlr_backend_output_state *states =
        wlr_output_configuration_v1_build_state(config, &states_len);
    if (!states) {
        return false;
    }

    bool ok = wlr_backend_test(server->backend, states, states_len);
    if (ok) {
        ok = wlr_backend_commit(server->backend, states, states_len);
    }

    if (ok) {
        struct wlr_output_configuration_head_v1 *head;
        wl_list_for_each(head, &config->heads, link) {
            struct wlr_output *wlr_output = head->state.output;
            if (!head->state.enabled) {
                wlr_output_layout_remove(server->output_layout, wlr_output);
            } else {
                wlr_output_layout_add(
                    server->output_layout, wlr_output,
                    head->state.x, head->state.y);
            }
        }
    }

    free(states);
    return ok;
}

static void handle_output_manager_apply(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, output_manager_apply);
    struct wlr_output_configuration_v1 *config = data;

    bool ok = server_apply_output_config(server, config);

    if (ok) {
        wlr_output_configuration_v1_send_succeeded(config);
    } else {
        wlr_output_configuration_v1_send_failed(config);
    }

    wlr_output_configuration_v1_destroy(config);

    server_arrange(server);
    server_update_output_manager_config(server);
}

static void handle_output_manager_test(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, output_manager_test);
    struct wlr_output_configuration_v1 *config = data;

    size_t states_len = 0;
    struct wlr_backend_output_state *states =
        wlr_output_configuration_v1_build_state(config, &states_len);
    if (!states) {
        wlr_output_configuration_v1_send_failed(config);
        wlr_output_configuration_v1_destroy(config);
        return;
    }

    bool ok = wlr_backend_test(server->backend, states, states_len);
    free(states);

    if (ok) {
        wlr_output_configuration_v1_send_succeeded(config);
    } else {
        wlr_output_configuration_v1_send_failed(config);
    }

    wlr_output_configuration_v1_destroy(config);
}

void server_update_output_manager_config(Server *server) {
    if (!server || !server->output_manager || server->shutting_down) return;

    struct wlr_output_configuration_v1 *config = wlr_output_configuration_v1_create();
    if (!config) return;

    Output *output;
    wl_list_for_each(output, &server->outputs, link) {
        struct wlr_output *wlr_output = output->wlr_output;
        struct wlr_output_configuration_head_v1 *head =
            wlr_output_configuration_head_v1_create(config, wlr_output);
        if (!head) continue;

        struct wlr_box box = {0};
        wlr_output_layout_get_box(server->output_layout, wlr_output, &box);
        head->state.x = box.x;
        head->state.y = box.y;
        head->state.enabled = wlr_output->enabled;
        head->state.mode = wlr_output->current_mode;
        head->state.scale = wlr_output->scale;
        head->state.transform = wlr_output->transform;
    }

    wlr_output_manager_v1_set_configuration(server->output_manager, config);
}

void server_apply_output_rules(Server *server) {
    if (!server || server->shutting_down) return;

    for (int i = 0; i < server->config.output_rule_count; i++) {
        const ConfigOutputRule *rule = &server->config.output_rules[i];
        Output *output = output_find_by_name(server, rule->name);
        if (!output || !output->wlr_output) continue;

        struct wlr_output_state state;
        wlr_output_state_init(&state);

        if (!rule->enabled) {
            wlr_output_state_set_enabled(&state, false);
            wlr_output_commit_state(output->wlr_output, &state);
            wlr_output_state_finish(&state);
            wlr_output_layout_remove(server->output_layout, output->wlr_output);
            continue;
        }

        wlr_output_state_set_enabled(&state, true);

        if (!rule->use_preferred) {
            struct wlr_output_mode *mode;
            wl_list_for_each(mode, &output->wlr_output->modes, link) {
                if (mode->width == rule->mode_width &&
                    mode->height == rule->mode_height) {
                    if (rule->mode_refresh == 0 ||
                        mode->refresh == rule->mode_refresh) {
                        wlr_output_state_set_mode(&state, mode);
                        break;
                    }
                }
            }
        }

        bool ok = wlr_output_commit_state(output->wlr_output, &state);
        wlr_output_state_finish(&state);

        if (ok) {
            wlr_output_layout_add(
                server->output_layout, output->wlr_output,
                rule->x, rule->y);
        }
    }

    server_arrange(server);
    server_update_output_manager_config(server);
}

static void reload_config(Server *server) {
    if (!server || server->shutting_down) return;

    Config new_config;
    config_init_defaults(&new_config);

    char path[1024];
    if (config_default_path(path, sizeof(path))) {
        config_load_file(&new_config, path);
    }

    if (!new_config.has_keybinds) {
        config_load_default_keybinds(&new_config);
    }
    config_load_workspace_keybinds(&new_config);

    config_destroy(&server->config);
    server->config = new_config;

    wl_list_init(&server->config.keybinds);
    wl_list_init(&server->config.exec_once);
    wl_list_insert_list(&server->config.keybinds, &new_config.keybinds);
    wl_list_insert_list(&server->config.exec_once, &new_config.exec_once);

    Output *output;
    wl_list_for_each(output, &server->outputs, link) {
        if (output->wallpaper) {
            wlr_scene_node_destroy(&output->wallpaper->node);
            output->wallpaper = NULL;
            output->wallpaper_buffer = NULL;
        }
    }

    struct wlr_buffer *old_wallpaper = server->wallpaper_buffer;
    server->wallpaper_buffer = NULL;
    server->wallpaper_width = 0;
    server->wallpaper_height = 0;

    if (server->config.wallpaper_path[0]) {
        wallpaper_load_file(
            server->renderer,
            server->allocator,
            server->config.wallpaper_path,
            &server->wallpaper_buffer,
            &server->wallpaper_width,
            &server->wallpaper_height
        );
    }

    bool do_fade = old_wallpaper && server->wallpaper_buffer &&
                   old_wallpaper != server->wallpaper_buffer;

    if (do_fade) {
        int fade_ms = server->config.animation_duration_ms;
        if (fade_ms < 0) fade_ms = 0;
        wl_list_for_each(output, &server->outputs, link) {
            if (output->wallpaper_fade) {
                wlr_scene_node_destroy(&output->wallpaper_fade->node);
                output->wallpaper_fade = NULL;
            }
            output->wallpaper_fade = wlr_scene_buffer_create(server->scene_tree, old_wallpaper);
            if (output->wallpaper_fade) {
                wlr_scene_node_lower_to_bottom(&output->wallpaper_fade->node);
                wlr_scene_node_set_position(&output->wallpaper_fade->node, output->x, output->y);
                wlr_scene_buffer_set_dest_size(output->wallpaper_fade, output->width, output->height);
                animation_init(&output->wallpaper_fade_anim, 1.0);
                animation_set_target(&output->wallpaper_fade_anim, 0.0, fade_ms, EASING_EASE_OUT, NULL);
                wlr_scene_buffer_set_opacity(output->wallpaper_fade, 1.0f);
                if (output->wlr_output) {
                    wlr_output_schedule_frame(output->wlr_output);
                }
            }
        }
    }

    if (old_wallpaper) {
        wlr_buffer_unlock(old_wallpaper);
    }

    View *view;
    wl_list_for_each(view, &server->views, link) {
        view_refresh_decorations(view);
    }

    input_reload_keymaps(server);
    input_reload_cursor(server);

    server_apply_output_rules(server);

    server_arrange(server);
}

void server_reload_config(Server *server) {
    reload_config(server);
}

#ifndef __NetBSD__
static int handle_reload_fd(int fd, uint32_t mask, void *data) {
    Server *server = data;
    if (mask & WL_EVENT_READABLE) {
        struct signalfd_siginfo si;
        bool got = false;
        for (;;) {
            ssize_t n = read(fd, &si, sizeof(si));
            if (n != (ssize_t)sizeof(si)) {
                break;
            }
            got = true;
        }
        if (got) {
            reload_config(server);
        }
    }
    return 0;
}
#endif

static void handle_decoration_destroy(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, decoration_destroy);
    (void)data;
    listener_remove(&view->decoration_request_mode);
    view->decoration = NULL;
    view->decoration_mode_set = false;
    listener_remove(&view->decoration_destroy);
}

static void handle_decoration_request_mode(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, decoration_request_mode);
    (void)data;
    if (!view || !view->decoration) return;
    if (!view->xdg.xdg_surface || !view->xdg.xdg_surface->initialized) return;
    wlr_xdg_toplevel_decoration_v1_set_mode(view->decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
    view->decoration_mode_set = true;
}

static void handle_xdg_activation_request_activate(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, xdg_activation_request_activate);
    struct wlr_xdg_activation_v1_request_activate_event *event = data;
    if (!event || !event->surface || server->shutting_down) return;
    struct wlr_surface *root = wlr_surface_get_root_surface(event->surface);
    if (!root) return;
    View *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->type == VIEW_TYPE_XDG &&
            view->mapped &&
            view->xdg.xdg_surface &&
            view->xdg.xdg_surface->surface == root) {
            View *fs = view->output ? output_fullscreen_view(view->output) : NULL;
            if (fs && fs != view) return;
            server_focus_view(server, view);
            return;
        }
    }
}

static void handle_request_set_selection(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, request_set_selection);
    struct wlr_seat_request_set_selection_event *event = data;
    wlr_seat_set_selection(server->seat, event->source, event->serial);
}

static void handle_new_xdg_decoration(struct wl_listener *listener, void *data) {
    (void)listener;
    struct wlr_xdg_toplevel_decoration_v1 *decoration = data;
    if (!decoration->toplevel || !decoration->toplevel->base) {
        return;
    }
    struct wlr_xdg_surface *xdg_surface = decoration->toplevel->base;
    View *view = xdg_surface->data;
    if (!view) {
        if (xdg_surface->initialized) {
            wlr_xdg_toplevel_decoration_v1_set_mode(decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        }
        return;
    }
    listener_remove(&view->decoration_destroy);
    view->decoration = decoration;
    view->decoration_mode_set = false;
    view->decoration_destroy.notify = handle_decoration_destroy;
    wl_signal_add(&decoration->events.destroy, &view->decoration_destroy);
    if (xdg_surface->initialized) {
        wlr_xdg_toplevel_decoration_v1_set_mode(decoration, WLR_XDG_TOPLEVEL_DECORATION_V1_MODE_SERVER_SIDE);
        view->decoration_mode_set = true;
    }
    listener_remove(&view->decoration_request_mode);
    view->decoration_request_mode.notify = handle_decoration_request_mode;
    wl_signal_add(&decoration->events.request_mode, &view->decoration_request_mode);
}

#if defined(WYO_HAS_XWAYLAND)
static struct wlr_buffer *xcursor_image_to_buffer(Server *server, struct wlr_xcursor_image *image) {
    if (!server || !server->renderer || !server->allocator || !image) return NULL;
    const struct wlr_drm_format_set *formats = wlr_renderer_get_texture_formats(server->renderer, WLR_BUFFER_CAP_DATA_PTR);
    if (!formats) return NULL;
    const struct wlr_drm_format *fmt = wlr_drm_format_set_get(formats, DRM_FORMAT_ARGB8888);
    if (!fmt) fmt = wlr_drm_format_set_get(formats, DRM_FORMAT_XRGB8888);
    if (!fmt) return NULL;
    struct wlr_buffer *buf = wlr_allocator_create_buffer(server->allocator, image->width, image->height, fmt);
    if (!buf) return NULL;
    void *data = NULL;
    uint32_t out_fmt = 0;
    size_t stride = 0;
    if (!wlr_buffer_begin_data_ptr_access(buf, WLR_BUFFER_DATA_PTR_ACCESS_WRITE, &data, &out_fmt, &stride)) {
        wlr_buffer_drop(buf);
        return NULL;
    }
    uint8_t *dst = (uint8_t *)data;
    const uint8_t *src = image->buffer;
    size_t row = (size_t)image->width * 4;
    for (uint32_t y = 0; y < image->height; y++) {
        memcpy(dst + (size_t)y * stride, src + (size_t)y * row, row);
    }
    wlr_buffer_end_data_ptr_access(buf);
    return buf;
}

static void handle_xwayland_ready(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, xwayland_ready);
    (void)data;
    if (!server->xwayland) return;
    setenv("DISPLAY", server->xwayland->display_name, 1);
    wlr_xwayland_set_seat(server->xwayland, server->seat);
    if (server->xcursor_manager) {
        struct wlr_xcursor *xcursor = wlr_xcursor_manager_get_xcursor(server->xcursor_manager, "left_ptr", 1);
        if (xcursor && xcursor->image_count > 0) {
            struct wlr_xcursor_image *image = xcursor->images[0];
            struct wlr_buffer *buf = xcursor_image_to_buffer(server, image);
            if (buf) {
                wlr_xwayland_set_cursor(server->xwayland, buf, image->hotspot_x, image->hotspot_y);
                wlr_buffer_drop(buf);
            }
        }
    }
}

static void handle_xwayland_new_surface(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, xwayland_new_surface);
    struct wlr_xwayland_surface *xsurface = data;
    view_create_xwayland(server, xsurface);
}

static void handle_xwayland_destroy(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, xwayland_destroy);
    (void)data;
    server->xwayland = NULL;
}
#endif

static const char *cursor_shape_fallback(const char *name) {
    if (!name || !*name) return "left_ptr";
    if (strcmp(name, "crosshair") == 0) return "crosshair";
    if (strcmp(name, "sb_v_double_arrow") == 0) return "ns-resize";
    if (strcmp(name, "sb_h_double_arrow") == 0) return "ew-resize";
    if (strcmp(name, "fleur") == 0) return "move";
    if (strcmp(name, "hand1") == 0 || strcmp(name, "hand2") == 0) return "pointer";
    if (strcmp(name, "xterm") == 0 || strcmp(name, "ibeam") == 0) return "text";
    if (strcmp(name, "watch") == 0 || strcmp(name, "wait") == 0) return "wait";
    if (strcmp(name, "pirate") == 0) return "not-allowed";
    return name;
}

static void handle_request_set_cursor_shape(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, request_set_cursor_shape);
    struct wlr_cursor_shape_manager_v1_request_set_shape_event *event = data;
    if (!server || !server->seat || !server->cursor || !event) {
        return;
    }
    if (server->drag.active || server->dnd_active) {
        return;
    }
    if (event->seat_client != server->seat->pointer_state.focused_client) {
        return;
    }
    if (server->xcursor_manager) {
        const char *name = wlr_cursor_shape_v1_name(event->shape);
        const char *fallback_name = cursor_shape_fallback(name);
        if (fallback_name && fallback_name[0]) {
            wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, fallback_name);
        }
    }
}

static void handle_new_output(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, new_output);
    struct wlr_output *wlr_output = data;
    output_create(server, wlr_output);
}

static void handle_new_toplevel(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, new_toplevel);
    struct wlr_xdg_toplevel *toplevel = data;
    struct wlr_xdg_surface *xdg_surface = toplevel->base;
    view_create_xdg(server, xdg_surface, toplevel);
}

void server_view_mapped(Server *server, View *view) {
    if (!server || server->shutting_down || !view) return;

    if (view->sticky) {
        view->floating = true;
        view->workspace = NULL;
        if (!view->output) {
            if (server->active_output) {
                view->output = server->active_output;
            }
            if (!view->output && server->cursor) {
                view->output = output_at(server, server->cursor->x, server->cursor->y);
            }
            if (!view->output && !wl_list_empty(&server->outputs)) {
                Output *first = wl_container_of(server->outputs.next, first, link);
                view->output = first;
            }
        }
        if (view->output && view->width > 0 && view->height > 0) {
            int rel_x = view->x - view->output->x;
            int rel_y = view->y - view->output->y;
            if (rel_x < 0 || rel_y < 0 || rel_x >= view->output->width || rel_y >= view->output->height) {
                int x = (view->output->width - view->width) / 2;
                int y = (view->output->height - view->height) / 2;
                if (x < 0) x = 0;
                if (y < 0) y = 0;
                view_set_geometry(view, x, y, view->width, view->height);
            }
        }
        server_focus_view(server, view);
        server_arrange(server);
        return;
    }

    if (!view->workspace) {
        if (!view->output) {
            if (server->active_output) {
                view->output = server->active_output;
            }
            if (!view->output && server->cursor) {
                view->output = output_at(server, server->cursor->x, server->cursor->y);
            }
            if (!view->output && !wl_list_empty(&server->outputs)) {
                Output *first = wl_container_of(server->outputs.next, first, link);
                view->output = first;
            }
        }
        if (view->output && view->output->active_workspace) {
            view->workspace = view->output->active_workspace;
        }
    }
    if (view->workspace && view->workspace->output) {
        view->output = view->workspace->output;
    }

    if (!view->workspace || !view->output) return;

    if (!view->tiled && !view->floating && !view->fullscreen && !view->dragging) {
        bool transient = false;
        if (view->type == VIEW_TYPE_XDG &&
            view->xdg.toplevel &&
            view->xdg.toplevel->parent != NULL) {
            transient = true;
        }
#if defined(WYO_HAS_XWAYLAND)
        if (view->type == VIEW_TYPE_XWAYLAND && view->xwayland.xsurface) {
            if (view->xwayland.xsurface->parent != NULL ||
                view->xwayland.xsurface->override_redirect) {
                transient = true;
            }
        }
#endif
        if (transient) {
            view->floating = true;
            Output *out = view->output;
            if (out) {
                int width = view->width > 0 ? view->width : 640;
                int height = view->height > 0 ? view->height : 480;
                int x = (out->width - width) / 2;
                int y = (out->height - height) / 2;
                if (x < 0) x = 0;
                if (y < 0) y = 0;
                view_set_geometry(view, x, y, width, height);
            }
        } else {
            workspace_add_view(view->workspace, view);
            view->tiled = true;
        }
    }

    view_apply_window_rules(view);

    if (view->output) {
        server_focus_view(server, view);
    }
    server_arrange(server);

    if (view->type == VIEW_TYPE_XDG &&
        view->xdg.toplevel &&
        view->xdg.xdg_surface &&
        view->xdg.xdg_surface->initialized &&
        view->width > 0 &&
        view->height > 0) {
        wlr_xdg_toplevel_set_size(view->xdg.toplevel, view->width, view->height);
    }

    ipc_notify_workspaces(server);
    ext_workspace_sync(server);
}

static void server_release_workspace_if_empty(Server *server, Workspace *ws) {
    if (!server || server->shutting_down || !ws || !ws->output) return;
    if (!workspace_empty(server, ws)) return;

    Output *out = ws->output;
    bool was_active = (out->active_workspace == ws);

    workspace_hide(ws);
    ws->last_output = NULL;

    if (!was_active) return;

    Workspace *replacement = NULL;
    Workspace *w;
    wl_list_for_each(w, &server->workspaces, link) {
        if (w != ws && !w->output && workspace_empty(server, w)) {
            replacement = w;
            break;
        }
    }
    if (!replacement) {
        wl_list_for_each(w, &server->workspaces, link) {
            if (w != ws && !w->output) {
                replacement = w;
                break;
            }
        }
    }

    out->active_workspace = replacement;
    if (replacement) {
        workspace_show(replacement, out);
    }
}

void server_view_unmapped(Server *server, View *view) {
    if (!view || !server || server->shutting_down) return;

    if (server->drag.view == view) {
        server->drag.active = false;
        server->drag.move = false;
        server->drag.resize = false;
        server->drag.view = NULL;
    }

    if (view->workspace) {
        workspace_remove_view(view->workspace, view);
    }
    view->output = NULL;
    view->tiled = false;

    if (server->focused_view == view) {
        View *next = NULL;
        if (view->workspace && view->workspace->output) {
            next = workspace_focused_view(view->workspace);
            if (!next) {
                next = workspace_first_view(view->workspace);
            }
        }
        if (!next && server->active_output && server->active_output->active_workspace) {
            next = workspace_focused_view(server->active_output->active_workspace);
            if (!next) {
                next = workspace_first_view(server->active_output->active_workspace);
            }
        }
        server_focus_view(server, next);
    }

    server_arrange(server);
    ipc_notify_workspaces(server);
    ext_workspace_sync(server);
}

void server_view_destroyed(Server *server, View *view) {
    if (!view || !server || server->shutting_down) return;

    if (server->drag.view == view) {
        server->drag.active = false;
        server->drag.move = false;
        server->drag.resize = false;
        server->drag.view = NULL;
    }

    Workspace *ws = view->workspace;
    if (ws) {
        workspace_remove_view(ws, view);
        view->workspace = NULL;
    }
    view->output = NULL;
    view->tiled = false;

    if (server->focused_view == view) {
        View *next = NULL;
        if (ws) {
            next = workspace_focused_view(ws);
            if (!next) {
                next = workspace_first_view(ws);
            }
        }
        if (ws) {
            server_release_workspace_if_empty(server, ws);
        }
        if (!next && server->active_output && server->active_output->active_workspace) {
            next = workspace_focused_view(server->active_output->active_workspace);
            if (!next) {
                next = workspace_first_view(server->active_output->active_workspace);
            }
        }
        server_focus_view(server, next);
    } else {
        if (ws) {
            server_release_workspace_if_empty(server, ws);
        }
    }

    server_arrange(server);
    ipc_notify_workspaces(server);
    ext_workspace_sync(server);
}

static void server_raise_floating(Server *server) {
    if (!server || server->shutting_down) return;
    View *focused = server->focused_view;
    View *view;
    wl_list_for_each_reverse(view, &server->views, link) {
        if (view == focused) continue;
        if (!view->mapped || !view->floating || view->fullscreen || !view->output || !view->root_tree) {
            continue;
        }
        wlr_scene_node_raise_to_top(&view->root_tree->node);
    }
    if (focused && focused->mapped && focused->floating &&
        !focused->fullscreen && focused->output && focused->root_tree) {
        wlr_scene_node_raise_to_top(&focused->root_tree->node);
    }
}

View *output_fullscreen_view(Output *output) {
    if (!output || !output->server) return NULL;
    View *view;
    wl_list_for_each(view, &output->server->views, link) {
        if (view->output == output &&
            view->mapped &&
            view->fullscreen &&
            view->root_tree &&
            view->root_tree->node.enabled) {
            return view;
        }
    }
    return NULL;
}

void server_focus_view(Server *server, View *view) {
    if (!server || server->shutting_down) return;

    if (view && (!view->mapped || !view->output)) {
        view = NULL;
    }

    if (view && view->output) {
        View *fs = output_fullscreen_view(view->output);
        if (fs && fs != view) {
            view = fs;
        }
    }

    if (view && view->output) {
        server->active_output = view->output;
    }

    if (server->focused_view == view && !server->focused_surface) {
        if (view && view->workspace) {
            dwindle_focus(&view->workspace->layout, view);
        }
        return;
    }

    listener_remove(&server->focused_surface_destroy);
    server->focused_surface = NULL;

    if (server->focused_view) {
        view_unfocus(server->focused_view);
    }

    server->focused_view = view;

    if (view) {
        view_focus(view);
        if (view->workspace) {
            dwindle_focus(&view->workspace->layout, view);
        }

        if (server->seat &&
            server->active_keyboard &&
            view->type == VIEW_TYPE_XDG &&
            view->xdg.xdg_surface &&
            view->xdg.xdg_surface->surface) {
            wlr_seat_set_keyboard(server->seat, server->active_keyboard);
            wlr_seat_keyboard_notify_enter(
                server->seat,
                view->xdg.xdg_surface->surface,
                server->active_keyboard->keycodes,
                server->active_keyboard->num_keycodes,
                &server->active_keyboard->modifiers
            );
#if defined(WYO_HAS_XWAYLAND)
        } else if (server->seat &&
                   server->active_keyboard &&
                   view->type == VIEW_TYPE_XWAYLAND &&
                   view->xwayland.xsurface &&
                   view->xwayland.xsurface->surface) {
            wlr_seat_set_keyboard(server->seat, server->active_keyboard);
            wlr_seat_keyboard_notify_enter(
                server->seat,
                view->xwayland.xsurface->surface,
                server->active_keyboard->keycodes,
                server->active_keyboard->num_keycodes,
                &server->active_keyboard->modifiers
            );
#endif
        }
    } else {
        if (server->seat) {
            wlr_seat_keyboard_notify_clear_focus(server->seat);
        }
    }

    server_raise_floating(server);
    ipc_notify_workspaces(server);
    ext_workspace_sync(server);
}

static void handle_focused_surface_destroy(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, focused_surface_destroy);
    (void)data;
    listener_remove(&server->focused_surface_destroy);
    server->focused_surface = NULL;

    if (server->shutting_down) return;

    View *next = NULL;
    if (server->active_output && server->active_output->active_workspace) {
        next = dwindle_focused_view(&server->active_output->active_workspace->layout);
        if (!next) next = dwindle_first_view(&server->active_output->active_workspace->layout);
    }
    server->focused_view = NULL;
    server_focus_view(server, next);
}

void server_focus_surface(Server *server, struct wlr_surface *surface) {
    if (!server || server->shutting_down) return;
    if (surface && server->focused_surface == surface) return;

    listener_remove(&server->focused_surface_destroy);

    if (server->focused_view) {
        view_unfocus(server->focused_view);
        server->focused_view = NULL;
    }

    server->focused_surface = surface;

    if (surface) {
        server->focused_surface_destroy.notify = handle_focused_surface_destroy;
        wl_signal_add(&surface->events.destroy, &server->focused_surface_destroy);
        if (server->seat && server->active_keyboard) {
            wlr_seat_set_keyboard(server->seat, server->active_keyboard);
            wlr_seat_keyboard_notify_enter(
                server->seat,
                surface,
                server->active_keyboard->keycodes,
                server->active_keyboard->num_keycodes,
                &server->active_keyboard->modifiers
            );
            return;
        }
    }

    if (server->seat) {
        wlr_seat_keyboard_notify_clear_focus(server->seat);
    }
}

static void update_background(Server *server) {
    if (!server->background) return;
    if (server->wallpaper_buffer) {
        wlr_scene_node_set_enabled(&server->background->node, false);
        return;
    }
    wlr_scene_node_set_enabled(&server->background->node, true);
    wlr_scene_rect_set_color(server->background, server->config.background_color);

    if (wl_list_empty(&server->outputs)) {
        wlr_scene_rect_set_size(server->background, 0, 0);
        return;
    }

    int min_x = 0;
    int min_y = 0;
    int max_x = 0;
    int max_y = 0;
    bool first = true;

    Output *output;
    wl_list_for_each(output, &server->outputs, link) {
        int right = output->x + output->width;
        int bottom = output->y + output->height;
        if (first) {
            min_x = output->x;
            min_y = output->y;
            max_x = right;
            max_y = bottom;
            first = false;
        } else {
            if (output->x < min_x) min_x = output->x;
            if (output->y < min_y) min_y = output->y;
            if (right > max_x) max_x = right;
            if (bottom > max_y) max_y = bottom;
        }
    }

    wlr_scene_node_set_position(&server->background->node, min_x, min_y);
    wlr_scene_rect_set_size(server->background, max_x - min_x, max_y - min_y);
    wlr_scene_node_lower_to_bottom(&server->background->node);
}

void server_arrange(Server *server) {
    if (!server || server->shutting_down) return;

    update_background(server);

    Output *output;
    wl_list_for_each(output, &server->outputs, link) {
        output_update_geometry(output);
        wallpaper_configure_output(output);
    }

    (void)layer_shell_arrange(server);

    View *view;
    wl_list_for_each(view, &server->views, link) {
        view_update_scene_parent(view);
    }

    wl_list_for_each(output, &server->outputs, link) {
        Workspace *ws = output->active_workspace;
        if (!ws) continue;

        int gaps_out = server->config.gaps_out;
        if (server->config.smart_gaps) {
            int tiled_count = 0;
            View *v;
            wl_list_for_each(v, &server->views, link) {
                if (v->workspace == ws && v->tiled && v->mapped) {
                    tiled_count++;
                }
            }
            if (tiled_count == 1) {
                gaps_out = server->config.border_width;
            }
        }

        dwindle_set_gaps(&ws->layout, server->config.gaps_in, gaps_out);
        ws->layout.origin_x = output->usable_x;
        ws->layout.origin_y = output->usable_y;
        int width = output->usable_width > 0 ? output->usable_width : output->width;
        int height = output->usable_height > 0 ? output->usable_height : output->height;
        dwindle_arrange(&ws->layout, width, height);
    }

    server_raise_floating(server);

    wl_list_for_each(output, &server->outputs, link) {
        if (output->wlr_output) {
            wlr_output_schedule_frame(output->wlr_output);
        }
    }
}

static void handle_constraint_destroy(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, constraint_destroy);
    struct wlr_pointer_constraint_v1 *constraint = data;
    if (server->active_constraint == constraint) {
        server->active_constraint = NULL;
        server->cursor_hidden = false;
        server->client_cursor_hidden = false;
        server->client_cursor_hidden_time = 0;
        if (server->cursor && server->xcursor_manager) {
            wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, "left_ptr");
        }
    }
    listener_remove(&server->constraint_destroy);
}

static void handle_new_constraint(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, new_constraint);
    struct wlr_pointer_constraint_v1 *constraint = data;
    if (!constraint || !server->cursor) {
        return;
    }
    if (server->active_constraint) {
        listener_remove(&server->constraint_destroy);
        wlr_pointer_constraint_v1_send_deactivated(server->active_constraint);
    }
    server->active_constraint = constraint;
    constraint->data = server;
    server->constraint_destroy.notify = handle_constraint_destroy;
    wl_signal_add(&constraint->events.destroy, &server->constraint_destroy);
    wlr_pointer_constraint_v1_send_activated(constraint);
    if (constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
        server->cursor_hidden = true;
        server->client_cursor_hidden = true;
        wlr_cursor_set_surface(server->cursor, NULL, 0, 0);
        wlr_cursor_unset_image(server->cursor);
    } else {
        server->cursor_hidden = false;
        server->client_cursor_hidden = false;
        server->client_cursor_hidden_time = 0;
        if (server->xcursor_manager) {
            wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, "left_ptr");
        }
    }
}

bool server_init(Server *server) {
    memset(server, 0, sizeof(Server));
    server->shutting_down = false;
    server->reload_fd = -1;

    wl_list_init(&server->new_output.link);
    wl_list_init(&server->new_toplevel.link);
    wl_list_init(&server->new_input.link);
    wl_list_init(&server->new_layer_surface.link);
    wl_list_init(&server->cursor_motion.link);
    wl_list_init(&server->cursor_motion_absolute.link);
    wl_list_init(&server->cursor_button.link);
    wl_list_init(&server->cursor_axis.link);
    wl_list_init(&server->new_xdg_decoration.link);
    wl_list_init(&server->request_set_selection.link);
    wl_list_init(&server->request_set_cursor.link);
    wl_list_init(&server->request_start_drag.link);
    wl_list_init(&server->start_drag.link);
    wl_list_init(&server->drag_focus.link);
    wl_list_init(&server->drag_motion.link);
    wl_list_init(&server->drag_drop.link);
    wl_list_init(&server->drag_destroy.link);
    wl_list_init(&server->request_set_cursor_shape.link);
    wl_list_init(&server->new_constraint.link);
    wl_list_init(&server->constraint_destroy.link);
    wl_list_init(&server->ext_workspace_commit.link);
    wl_list_init(&server->ext_workspace_destroy_listener.link);
    wl_list_init(&server->xdg_activation_request_activate.link);
    wl_list_init(&server->output_manager_apply.link);
    wl_list_init(&server->output_manager_test.link);
    wl_list_init(&server->output_manager_destroy.link);
#if defined(WYO_HAS_XWAYLAND)
    wl_list_init(&server->xwayland_ready.link);
    wl_list_init(&server->xwayland_new_surface.link);
    wl_list_init(&server->xwayland_destroy.link);
#endif

    server->display = wl_display_create();
    if (!server->display) return false;

    server->loop = wl_display_get_event_loop(server->display);
    server->backend = wlr_backend_autocreate(server->loop, &server->session);
    if (!server->backend) return false;

    server->renderer = wlr_renderer_autocreate(server->backend);
    if (!server->renderer) return false;
    wlr_renderer_init_wl_display(server->renderer, server->display);

    server->allocator = wlr_allocator_autocreate(server->backend, server->renderer);
    if (!server->allocator) return false;

    if (!wlr_linux_dmabuf_v1_create_with_renderer(server->display, 4, server->renderer)) {
    }

    wlr_tearing_control_manager_v1_create(server->display, 1);
    wlr_primary_selection_v1_device_manager_create(server->display);
    wlr_idle_inhibit_v1_create(server->display);
    wlr_gamma_control_manager_v1_create(server->display);

    server->compositor = wlr_compositor_create(server->display, 6, server->renderer);
    if (!server->compositor) return false;

    server->subcompositor = wlr_subcompositor_create(server->display);
    if (!server->subcompositor) return false;

    server->data_device_manager = wlr_data_device_manager_create(server->display);
    if (!server->data_device_manager) return false;

    server->xdg_decoration_manager = wlr_xdg_decoration_manager_v1_create(server->display);
    if (server->xdg_decoration_manager) {
        server->new_xdg_decoration.notify = handle_new_xdg_decoration;
        wl_signal_add(&server->xdg_decoration_manager->events.new_toplevel_decoration, &server->new_xdg_decoration);
    }

    wlr_viewporter_create(server->display);

    server->xdg_activation = wlr_xdg_activation_v1_create(server->display);
    if (server->xdg_activation) {
        server->xdg_activation_request_activate.notify = handle_xdg_activation_request_activate;
        wl_signal_add(&server->xdg_activation->events.request_activate,
                      &server->xdg_activation_request_activate);
    }

    server->relative_pointer_manager = wlr_relative_pointer_manager_v1_create(server->display);

    server->pointer_constraints = wlr_pointer_constraints_v1_create(server->display);
    if (server->pointer_constraints) {
        server->new_constraint.notify = handle_new_constraint;
        wl_signal_add(&server->pointer_constraints->events.new_constraint, &server->new_constraint);
    }

    wlr_text_input_manager_v3_create(server->display);
    wlr_pointer_gestures_v1_create(server->display);
    wlr_screencopy_manager_v1_create(server->display);

    server->cursor_shape_manager = wlr_cursor_shape_manager_v1_create(server->display, 1);
    if (server->cursor_shape_manager) {
        server->request_set_cursor_shape.notify = handle_request_set_cursor_shape;
        wl_signal_add(&server->cursor_shape_manager->events.request_set_shape, &server->request_set_cursor_shape);
    }

    typedef void *(*wyo_versioned_global_fn)(struct wl_display *display, uint32_t version);
    wyo_versioned_global_fn create_frac_scale = (wyo_versioned_global_fn)dlsym(RTLD_DEFAULT, "wlr_fractional_scale_manager_v1_create");
    if (create_frac_scale) create_frac_scale(server->display, 1);

    server->output_layout = wlr_output_layout_create(server->display);
    if (!server->output_layout) return false;

    server->scene = wlr_scene_create();
    if (!server->scene) return false;

    server->scene_tree = wlr_scene_tree_create(&server->scene->tree);
    if (!server->scene_tree) return false;

    server->background = wlr_scene_rect_create(server->scene_tree, 0, 0, server->config.background_color);
    if (!server->background) return false;
    wlr_scene_node_lower_to_bottom(&server->background->node);

    server->view_tree = wlr_scene_tree_create(server->scene_tree);
    server->tiled_tree = wlr_scene_tree_create(server->view_tree);
    server->floating_tree = wlr_scene_tree_create(server->view_tree);
    server->layer_tree = wlr_scene_tree_create(server->scene_tree);
    server->fullscreen_tree = wlr_scene_tree_create(server->scene_tree);

    if (!server->view_tree || !server->tiled_tree || !server->floating_tree ||
        !server->layer_tree || !server->fullscreen_tree) return false;

    wlr_scene_node_raise_to_top(&server->floating_tree->node);

    server->scene_layout = wlr_scene_attach_output_layout(server->scene, server->output_layout);
    if (!server->scene_layout) return false;

    (void)wlr_presentation_create(server->display, server->backend, 1);

    typedef void *(*wyo_xdg_output_fn)(struct wl_display *display, struct wlr_output_layout *layout);
    wyo_xdg_output_fn create_xdg_output = (wyo_xdg_output_fn)dlsym(RTLD_DEFAULT, "wlr_xdg_output_manager_create");
    if (!create_xdg_output) {
        create_xdg_output = (wyo_xdg_output_fn)dlsym(RTLD_DEFAULT, "wlr_xdg_output_manager_v1_create");
    }
    if (create_xdg_output) {
        server->xdg_output_manager = create_xdg_output(server->display, server->output_layout);
    }

#if defined(WYO_HAS_FOREIGN_TOPLEVEL)
    server->foreign_toplevel_manager = wlr_foreign_toplevel_manager_v1_create(server->display);
#endif

config_init_defaults(&server->config);
char config_path[1024];
if (config_default_path(config_path, sizeof(config_path))) {
    if (access(config_path, F_OK) != 0) {
        config_generate_default(config_path);
    }
    config_load_file(&server->config, config_path);
}

    if (!server->config.has_keybinds) {
        config_load_default_keybinds(&server->config);
    }
    config_load_workspace_keybinds(&server->config);

    wlr_scene_rect_set_color(server->background, server->config.background_color);

    if (server->config.wallpaper_path[0]) {
        wallpaper_load_file(
            server->renderer,
            server->allocator,
            server->config.wallpaper_path,
            &server->wallpaper_buffer,
            &server->wallpaper_width,
            &server->wallpaper_height
        );
    }

    if (server->wallpaper_buffer) {
        wlr_scene_node_set_enabled(&server->background->node, false);
    }

    server->xdg_shell = wlr_xdg_shell_create(server->display, 6);
    if (!server->xdg_shell) return false;

    server->seat = wlr_seat_create(server->display, "seat0");
    if (!server->seat) return false;

    wl_list_init(&server->outputs);
    wl_list_init(&server->views);
    wl_list_init(&server->workspaces);
    wl_list_init(&server->keyboards);
    wl_list_init(&server->popup_surfaces);
    wl_list_init(&server->layer_surfaces);

    wl_list_init(&server->new_output.link);
    wl_list_init(&server->new_toplevel.link);
    wl_list_init(&server->new_input.link);
    wl_list_init(&server->new_layer_surface.link);
    wl_list_init(&server->cursor_motion.link);
    wl_list_init(&server->cursor_motion_absolute.link);
    wl_list_init(&server->cursor_button.link);
    wl_list_init(&server->cursor_axis.link);
    wl_list_init(&server->request_set_selection.link);
    wl_list_init(&server->request_set_cursor.link);
    wl_list_init(&server->request_start_drag.link);
    wl_list_init(&server->start_drag.link);
    wl_list_init(&server->drag_focus.link);
    wl_list_init(&server->drag_motion.link);
    wl_list_init(&server->drag_drop.link);
    wl_list_init(&server->drag_destroy.link);
    wl_list_init(&server->new_xdg_decoration.link);
    wl_list_init(&server->request_set_cursor_shape.link);
    wl_list_init(&server->new_constraint.link);
    wl_list_init(&server->constraint_destroy.link);
    wl_list_init(&server->focused_surface_destroy.link);
    wl_list_init(&server->xdg_activation_request_activate.link);
    wl_list_init(&server->ext_workspace_commit.link);
    wl_list_init(&server->ext_workspace_destroy_listener.link);
    wl_list_init(&server->output_manager_apply.link);
    wl_list_init(&server->output_manager_test.link);
    wl_list_init(&server->output_manager_destroy.link);
#if defined(WYO_HAS_XWAYLAND)
    wl_list_init(&server->xwayland_ready.link);
    wl_list_init(&server->xwayland_new_surface.link);
    wl_list_init(&server->xwayland_destroy.link);
#endif

    workspaces_init(server);
    ext_workspace_init(server);

    server->request_set_selection.notify = handle_request_set_selection;
    wl_signal_add(&server->seat->events.request_set_selection, &server->request_set_selection);

    input_init(server);
    layer_shell_init(server);

    server->new_output.notify = handle_new_output;
    wl_signal_add(&server->backend->events.new_output, &server->new_output);

    server->new_toplevel.notify = handle_new_toplevel;
    wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_toplevel);

    #ifdef __NetBSD__
        if (!server_netbsd_reload_init(server)) return false;
    #else
        sigset_t reload_mask;
        (void)sigemptyset(&reload_mask);
        (void)sigaddset(&reload_mask, SIGUSR1);
        (void)sigprocmask(SIG_BLOCK, &reload_mask, NULL);

        server->reload_fd = signalfd(-1, &reload_mask, SFD_NONBLOCK | SFD_CLOEXEC);
        if (server->reload_fd < 0) return false;

        server->reload_source = wl_event_loop_add_fd(
            server->loop,
            server->reload_fd,
            WL_EVENT_READABLE,
            handle_reload_fd,
            server
        );
        if (!server->reload_source) {
            close(server->reload_fd);
            server->reload_fd = -1;
            return false;
        }
    #endif

    char ipc_path[1024];
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && *runtime_dir) {
        snprintf(ipc_path, sizeof(ipc_path), "%s/wyowm.sock", runtime_dir);
    } else {
        snprintf(ipc_path, sizeof(ipc_path), "/tmp/wyowm.sock");
    }
    ipc_init(&server->ipc, server, server->loop, ipc_path);

    server->output_manager = wlr_output_manager_v1_create(server->display);
    if (server->output_manager) {
        server->output_manager_apply.notify = handle_output_manager_apply;
        wl_signal_add(&server->output_manager->events.apply, &server->output_manager_apply);
        server->output_manager_test.notify = handle_output_manager_test;
        wl_signal_add(&server->output_manager->events.test, &server->output_manager_test);
        server->output_manager_destroy.notify = handle_output_manager_destroy;
        wl_signal_add(&server->output_manager->events.destroy, &server->output_manager_destroy);
    }

#if defined(WYO_HAS_XWAYLAND)
    server->xwayland = wlr_xwayland_create(server->display, server->compositor, false);
    if (server->xwayland) {
        if (server->xwayland->display_name) {
            setenv("DISPLAY", server->xwayland->display_name, 1);
        }
        server->xwayland_ready.notify = handle_xwayland_ready;
        wl_signal_add(&server->xwayland->events.ready, &server->xwayland_ready);
        server->xwayland_new_surface.notify = handle_xwayland_new_surface;
        wl_signal_add(&server->xwayland->events.new_surface, &server->xwayland_new_surface);
        server->xwayland_destroy.notify = handle_xwayland_destroy;
        wl_signal_add(&server->xwayland->events.destroy, &server->xwayland_destroy);
    }
#endif

    return true;
}

static bool find_binary(const char *name, char *out, size_t out_size) {
    if (!name || !*name || !out || out_size == 0) return false;
    const char *dirs[] = {
        "/usr/libexec",
        "/usr/lib",
        "/usr/local/libexec",
        "/usr/local/bin",
        "/usr/bin",
        "/bin"
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(out, out_size, "%s/%s", dirs[i], name);
        if (access(out, X_OK) == 0) {
            return true;
        }
    }
    const char *path_env = getenv("PATH");
    if (!path_env || !*path_env) return false;
    char *copy = strdup(path_env);
    if (!copy) return false;
    bool found = false;
    char *saveptr = NULL;
    for (char *tok = strtok_r(copy, ":", &saveptr); tok; tok = strtok_r(NULL, ":", &saveptr)) {
        snprintf(out, out_size, "%s/%s", tok, name);
        if (access(out, X_OK) == 0) {
            found = true;
            break;
        }
    }
    free(copy);
    return found;
}

static bool process_running(const char *name) {
    if (!name || !*name) return false;
    DIR *dir = opendir("/proc");
    if (!dir) return false;
    size_t name_len = strlen(name);
    bool found = false;
    struct dirent *ent;
    while (!found && (ent = readdir(dir)) != NULL) {
        if (ent->d_name[0] < '0' || ent->d_name[0] > '9') {
            continue;
        }
        char path[288];
        snprintf(path, sizeof(path), "/proc/%s/cmdline", ent->d_name);
        FILE *fp = fopen(path, "r");
        if (!fp) continue;
        char buf[512];
        size_t n = fread(buf, 1, sizeof(buf) - 1, fp);
        fclose(fp);
        if (n == 0) continue;
        buf[n] = '\0';
        const char *base = strrchr(buf, '/');
        const char *cmd = base ? base + 1 : buf;
        if (strlen(cmd) == name_len && strncmp(cmd, name, name_len) == 0) {
            found = true;
        }
    }
    closedir(dir);
    return found;
}

static void ensure_portal_config(void) {
    char base[1024];
    const char *config_home = getenv("XDG_CONFIG_HOME");
    if (config_home && *config_home) {
        snprintf(base, sizeof(base), "%s/xdg-desktop-portal", config_home);
    } else {
        const char *home = getenv("HOME");
        if (!home || !*home) return;
        snprintf(base, sizeof(base), "%s/.config/xdg-desktop-portal", home);
    }
    char path[1280];
    snprintf(path, sizeof(path), "%s/portals.conf", base);
    if (access(path, F_OK) == 0) return;
    mkdir(base, 0755);
    FILE *fp = fopen(path, "w");
    if (!fp) return;
    fputs("[preferred]\n", fp);
    fputs("org.freedesktop.impl.portal.ScreenCast=wlr\n", fp);
    fputs("org.freedesktop.impl.portal.Screenshot=wlr\n", fp);
    fclose(fp);
}

static void spawn_portal_if_present(const char *name) {
    char path[512];
    if (!find_binary(name, path, sizeof(path))) return;
    if (process_running(name)) return;
    input_spawn_command(path);
}

static void spawn_session_portals(void *data) {
    (void)data;
    if (!getenv("DBUS_SESSION_BUS_ADDRESS")) return;
    char path[512];
    if (find_binary("xdg-desktop-portal-wlr", path, sizeof(path))) {
        ensure_portal_config();
        spawn_portal_if_present("xdg-desktop-portal-wlr");
    }
    spawn_portal_if_present("xdg-desktop-portal-gtk");
    spawn_portal_if_present("xdg-desktop-portal-kde");
    spawn_portal_if_present("xdg-desktop-portal");
}

static void spawn_exec_once(void *data) {
    Server *server = data;
    ConfigExecOnce *exec;
    wl_list_for_each(exec, &server->config.exec_once, link) {
        if (exec->command) {
            input_spawn_command(exec->command);
        }
    }
}

void server_run(Server *server) {
    const char *socket = wl_display_add_socket_auto(server->display);
    if (!socket) exit(1);

    if (!wlr_backend_start(server->backend)) exit(1);

    setenv("WAYLAND_DISPLAY", socket, 1);

    wl_event_loop_add_idle(server->loop, spawn_exec_once, server);
    wl_event_loop_add_idle(server->loop, spawn_session_portals, server);

    wl_display_run(server->display);
}

void server_destroy(Server *server) {
    if (!server) return;
    server->shutting_down = true;

    listener_remove(&server->focused_surface_destroy);
    server->focused_surface = NULL;
    server->focused_view = NULL;

    ext_workspace_destroy(server);
    layer_shell_destroy(server);

#if defined(WYO_HAS_XWAYLAND)
    listener_remove(&server->xwayland_ready);
    listener_remove(&server->xwayland_new_surface);
    listener_remove(&server->xwayland_destroy);
#endif

    while (!wl_list_empty(&server->outputs)) {
        Output *output = wl_container_of(server->outputs.next, output, link);
        output_destroy(output);
    }

    while (!wl_list_empty(&server->views)) {
        View *view = wl_container_of(server->views.next, view, link);
        view_cleanup_for_shutdown(view);
    }

    PopupSurface *popup_surface;
    PopupSurface *popup_surface_tmp;
    wl_list_for_each_safe(popup_surface, popup_surface_tmp, &server->popup_surfaces, link) {
        listener_remove(&popup_surface->destroy);
        wl_list_remove(&popup_surface->link);
        wl_list_init(&popup_surface->link);
        free(popup_surface);
    }
    wl_list_init(&server->popup_surfaces);

    workspaces_destroy(server);

    listener_remove(&server->new_output);
    listener_remove(&server->new_toplevel);
    listener_remove(&server->request_set_selection);
    listener_remove(&server->xdg_activation_request_activate);
    listener_remove(&server->constraint_destroy);
    listener_remove(&server->new_constraint);
    server->active_constraint = NULL;

    listener_remove(&server->output_manager_apply);
    listener_remove(&server->output_manager_test);
    listener_remove(&server->output_manager_destroy);

    input_destroy(server);

    #ifdef __NetBSD__
        server_netbsd_reload_destroy(server);
    #else
        if (server->reload_source) {
            wl_event_source_remove(server->reload_source);
            server->reload_source = NULL;
        }
        if (server->reload_fd >= 0) {
            close(server->reload_fd);
            server->reload_fd = -1;
        }
    #endif

    listener_remove(&server->new_xdg_decoration);
    listener_remove(&server->request_set_cursor_shape);

    config_destroy(&server->config);

    if (server->wallpaper_buffer) {
        wlr_buffer_unlock(server->wallpaper_buffer);
        server->wallpaper_buffer = NULL;
    }

    ipc_destroy(&server->ipc);

    if (server->display) {
        wl_display_destroy_clients(server->display);
        wl_display_destroy(server->display);
        server->display = NULL;
    }
}
