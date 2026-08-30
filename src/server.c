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
#include <sys/signalfd.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_relative_pointer_v1.h>
#include <wlr/types/wlr_pointer_gestures_v1.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_presentation_time.h>

#ifndef SFD_NONBLOCK
#define SFD_NONBLOCK O_NONBLOCK
#endif

#ifndef SFD_CLOEXEC
#define SFD_CLOEXEC O_CLOEXEC
#endif

static inline void listener_remove(struct wl_listener *listener) {
    if (!wl_list_empty(&listener->link)) {
        wl_list_remove(&listener->link);
        wl_list_init(&listener->link);
    }
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

    if (server->wallpaper_buffer) {
        wlr_buffer_unlock(server->wallpaper_buffer);
        server->wallpaper_buffer = NULL;
    }

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

    View *view;
    wl_list_for_each(view, &server->views, link) {
        view_refresh_decorations(view);
    }
    input_reload_keymaps(server);
    server_arrange(server);
}

void server_reload_config(Server *server) {
    reload_config(server);
}

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

static void handle_decoration_destroy(struct wl_listener *listener, void *data) {
    View *view = wl_container_of(listener, view, decoration_destroy);
    (void)data;
    view->decoration = NULL;
    view->decoration_mode_set = false;
    listener_remove(&view->decoration_destroy);
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
		bool transient = view->type == VIEW_TYPE_XDG &&
		                 view->xdg.toplevel &&
		                 view->xdg.toplevel->parent != NULL;
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

    server_focus_view(server, view);
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

    if (view->workspace) {
        workspace_remove_view(view->workspace, view);
    }

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

void server_focus_view(Server *server, View *view) {
    if (!server || server->shutting_down) return;
    if (view && (!view->mapped || !view->output)) {
        view = NULL;
    }
    if (view && view->output) {
        server->active_output = view->output;
    }
    if (server->focused_view == view && !server->focused_surface) {
        if (view && view->output) {
            dwindle_focus(&view->output->layout, view);
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
    if (view->output) {
        dwindle_focus(&view->output->layout, view);
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
        }
    } else {
        if (server->seat) {
            wlr_seat_keyboard_notify_clear_focus(server->seat);
        }
    }
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
if (server->active_output) {
next = dwindle_focused_view(&server->active_output->layout);
if (!next) next = dwindle_first_view(&server->active_output->layout);
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

    wl_list_for_each(output, &server->outputs, link) {
        dwindle_set_gaps(&output->layout, server->config.gaps_in, server->config.gaps_out);

        output->layout.origin_x = output->usable_x;
        output->layout.origin_y = output->usable_y;

        int width = output->usable_width > 0 ? output->usable_width : output->width;
        int height = output->usable_height > 0 ? output->usable_height : output->height;

        dwindle_arrange(&output->layout, width, height);
    }
}

bool server_init(Server *server) {
    memset(server, 0, sizeof(Server));
    server->shutting_down = false;
    server->reload_fd = -1;

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
    wlr_relative_pointer_manager_v1_create(server->display);
    wlr_pointer_gestures_v1_create(server->display);
    wlr_screencopy_manager_v1_create(server->display);

    typedef void *(*wyo_versioned_global_fn)(struct wl_display *display, uint32_t version);

    wyo_versioned_global_fn create_cursor_shape = (wyo_versioned_global_fn)dlsym(RTLD_DEFAULT, "wlr_cursor_shape_manager_v1_create");
    if (create_cursor_shape) create_cursor_shape(server->display, 1);

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
    server->layer_tree = wlr_scene_tree_create(server->scene_tree);
    if (!server->view_tree || !server->layer_tree) return false;

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
    wl_list_init(&server->focused_surface_destroy.link);

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

    char ipc_path[1024];
    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (runtime_dir && *runtime_dir) {
        snprintf(ipc_path, sizeof(ipc_path), "%s/wyowm.sock", runtime_dir);
    } else {
        snprintf(ipc_path, sizeof(ipc_path), "/tmp/wyowm.sock");
    }

    ipc_init(&server->ipc, server, server->loop, ipc_path);

    return true;
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

void server_run(Server *server) {
    const char *socket = wl_display_add_socket_auto(server->display);
    if (!socket) {
        exit(1);
    }

    if (!wlr_backend_start(server->backend)) {
        exit(1);
    }

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
    Workspace *ws, *tmp_ws;
    wl_list_for_each_safe(ws, tmp_ws, &server->workspaces, link) {
        workspace_destroy(ws);
    }
    layer_shell_destroy(server);

    while (!wl_list_empty(&server->outputs)) {
        Output *output = wl_container_of(server->outputs.next, output, link);
        output_destroy(output);
    }

    workspaces_destroy(server);

    while (!wl_list_empty(&server->views)) {
        View *view = wl_container_of(server->views.next, view, link);
        view_cleanup_for_shutdown(view);
    }

   	listener_remove(&server->new_output);
	listener_remove(&server->new_toplevel);
	listener_remove(&server->request_set_selection);
	listener_remove(&server->xdg_activation_request_activate);
    input_destroy(server);

    if (server->reload_source) {
        wl_event_source_remove(server->reload_source);
        server->reload_source = NULL;
    }

    if (server->reload_fd >= 0) {
        close(server->reload_fd);
        server->reload_fd = -1;
    }

   	if (!wl_list_empty(&server->new_xdg_decoration.link)) {
		wl_list_remove(&server->new_xdg_decoration.link);
	}

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
