#include "input.h"
#include "server.h"
#include "view.h"
#include "output.h"
#include "layout.h"
#include "config.h"
#include "layer.h"
#include <wlr/types/wlr_layer_shell_v1.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>
#include <wlr/types/wlr_pointer.h>

#define BIND_MODIFIER_MASK (WLR_MODIFIER_SHIFT | WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT | WLR_MODIFIER_LOGO)

static void direction_to_delta(int dir, int *dx, int *dy) {
    if (dx) *dx = 0;
    if (dy) *dy = 0;

    if (dir == 0) {
        if (dx) *dx = -1;
    } else if (dir == 1) {
        if (dy) *dy = 1;
    } else if (dir == 2) {
        if (dy) *dy = -1;
    } else if (dir == 3) {
        if (dx) *dx = 1;
    }
}

static bool spawn_command(const char *command) {
    pid_t pid = fork();
    if (pid < 0) return false;

    if (pid == 0) {
        pid_t grandchild = fork();

        if (grandchild == 0) {
            setsid();

            int devnull = open("/dev/null", O_RDWR);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                if (devnull > STDERR_FILENO) {
                    close(devnull);
                }
            }

            int logfile = open("/tmp/wyowm-spawn.log", O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (logfile >= 0) {
                dup2(logfile, STDOUT_FILENO);
                dup2(logfile, STDERR_FILENO);
                if (logfile > STDERR_FILENO) {
                    close(logfile);
                }
            }

            execl("/bin/sh", "sh", "-c", command, (char *)0);
            _exit(127);
        }

        _exit(grandchild < 0 ? 126 : 0);
    }

    int status;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) {
            break;
        }
    }

    return true;
}

void input_spawn_command(const char *command) {
	spawn_command(command);
}

static Output *find_output_in_direction(Server *server, Output *current, int dir) {
    if (!server || !current) return NULL;

    Output *best = NULL;
    int best_dist = INT_MAX;

    Output *o;
    wl_list_for_each(o, &server->outputs, link) {
        if (o == current || o->width <= 0 || o->height <= 0) continue;

        bool h_overlap = o->y < current->y + current->height &&
                         o->y + o->height > current->y;
        bool v_overlap = o->x < current->x + current->width &&
                         o->x + o->width > current->x;

        int dist = INT_MAX;

        if (dir == 0) {
            if (o->x + o->width <= current->x && h_overlap) {
                dist = current->x - (o->x + o->width);
            }
        } else if (dir == 1) {
            if (o->y >= current->y + current->height && v_overlap) {
                dist = o->y - (current->y + current->height);
            }
        } else if (dir == 2) {
            if (o->y + o->height <= current->y && h_overlap) {
                dist = current->y - (o->y + o->height);
            }
        } else if (dir == 3) {
            if (o->x >= current->x + current->width && v_overlap) {
                dist = o->x - (current->x + current->width);
            }
        }

        if (dist != INT_MAX && dist < best_dist) {
            best_dist = dist;
            best = o;
        }
    }

    return best;
}

static void focus_output_direction(Server *server, int dir) {
    Output *current = server->active_output;

    if (!current && server->focused_view) {
        current = server->focused_view->output;
    }

    if (!current && !wl_list_empty(&server->outputs)) {
        current = wl_container_of(server->outputs.next, current, link);
    }

    if (!current) return;

    Output *target = find_output_in_direction(server, current, dir);
    if (!target) return;

    View *view = dwindle_focused_view(&target->layout);
    if (!view) view = dwindle_first_view(&target->layout);

    if (view) {
        server_focus_view(server, view);
        return;
    }

    server->active_output = target;

    if (server->cursor) {
        wlr_cursor_warp(
            server->cursor,
            NULL,
            target->x + target->width / 2,
            target->y + target->height / 2
        );
    }

    server_focus_view(server, NULL);
    wlr_seat_pointer_clear_focus(server->seat);
    wlr_seat_keyboard_notify_clear_focus(server->seat);
}

static void focus_direction(Server *server, int dir) {
    int dx = 0;
    int dy = 0;
    direction_to_delta(dir, &dx, &dy);

    View *current = server->focused_view;

    if (current && current->output && !current->floating && !current->fullscreen) {
        DwindleLayout *layout = &current->output->layout;

        dwindle_move_focus(layout, dx, dy);

        View *next = dwindle_focused_view(layout);
        if (next && next != current) {
            server_focus_view(server, next);
            return;
        }
    }

    focus_output_direction(server, dir);
}

static void move_view_to_output_direction(Server *server, int dir) {
    View *view = server->focused_view;
    if (!view) return;

    Output *source = view->output ? view->output : server->active_output;
    Output *target = find_output_in_direction(server, source, dir);

    if (!target) {
        if (view->floating && !view->fullscreen) {
            int dx = 0;
            int dy = 0;
            direction_to_delta(dir, &dx, &dy);
            view_move_by(view, dx * 40, dy * 40);
        }
        return;
    }

    if (view->fullscreen) {
        view->output = target;
        view_set_geometry(view, 0, 0, target->width, target->height);
        server_focus_view(server, view);
        server_arrange(server);
        return;
    }

    if (view->tiled && source) {
        dwindle_remove_view(&source->layout, view);
        view->tiled = false;
    }

    view->output = target;

    if (view->floating) {
        int width = view->width > 0 ? view->width : 800;
        int height = view->height > 0 ? view->height : 600;

        int x = (target->width - width) / 2;
        int y = (target->height - height) / 2;

        if (x < 0) x = 0;
        if (y < 0) y = 0;

        view_set_geometry(view, x, y, width, height);
    } else {
        dwindle_add_view(&target->layout, view);
        view->tiled = true;
    }

    server_focus_view(server, view);
    server_arrange(server);
}

static void move_view_in_direction(Server *server, int dir) {
    View *view = server->focused_view;
    if (!view || view->fullscreen) return;

    int dx = 0;
    int dy = 0;
    direction_to_delta(dir, &dx, &dy);

    if (view->floating) {
        view_move_by(view, dx * 40, dy * 40);
        return;
    }

    if (view->tiled && view->output) {
        dwindle_focus(&view->output->layout, view);

        if (dwindle_move_view(&view->output->layout, dx, dy)) {
            View *moved = dwindle_focused_view(&view->output->layout);
            if (moved) {
                server_focus_view(server, moved);
            }
            server_arrange(server);
            return;
        }
    }

    move_view_to_output_direction(server, dir);
}

static void resize_view_in_direction(Server *server, int dir) {
    View *view = server->focused_view;
    if (!view || view->fullscreen) return;

    int dx = 0;
    int dy = 0;
    direction_to_delta(dir, &dx, &dy);

    dx *= 40;
    dy *= 40;

    if (view->tiled && view->output) {
        dwindle_resize(&view->output->layout, dx, dy);
        wlr_output_schedule_frame(view->output->wlr_output);
        return;
    }

    int offset_x = 0;
    int offset_y = 0;

    if (view->output) {
        offset_x = view->output->x;
        offset_y = view->output->y;
    }

    int rel_x = view->x - offset_x;
    int rel_y = view->y - offset_y;

    int new_width = view->width + dx;
    int new_height = view->height + dy;

    if (new_width < 100) new_width = 100;
    if (new_height < 100) new_height = 100;

    view_set_geometry(view, rel_x, rel_y, new_width, new_height);
}

static bool handle_keybind_action(Server *server, const ConfigKeybind *bind) {
    switch (bind->action) {
    case ACTION_NONE:
        return true;

    case ACTION_EXEC:
        if (bind->command) {
            spawn_command(bind->command);
        }
        return true;

    case ACTION_CLOSE:
        if (server->focused_view) {
            view_close(server->focused_view);
        }
        return true;

    case ACTION_QUIT:
        wl_display_terminate(server->display);
        return true;

    case ACTION_SWITCH_VT:
        if (server->session) {
            wlr_session_change_vt(server->session, (unsigned)bind->arg);
        }
        return true;

    case ACTION_TOGGLE_FLOATING: {
        View *view = server->focused_view;
        if (!view || view->fullscreen) return true;

        if (!view->output) {
            view->output = server->active_output;
        }

        if (!view->output) return true;

        if (view->tiled) {
            dwindle_remove_view(&view->output->layout, view);
            view->tiled = false;
            view->floating = true;

            if (view->width >= view->output->width - 16 &&
                view->height >= view->output->height - 16) {
                int width = view->output->width * 8 / 10;
                int height = view->output->height * 8 / 10;

                int x = (view->output->width - width) / 2;
                int y = (view->output->height - height) / 2;

                if (x < 0) x = 0;
                if (y < 0) y = 0;

                view_set_geometry(view, x, y, width, height);
            }
        } else if (view->floating) {
            view->floating = false;
            dwindle_add_view(&view->output->layout, view);
            view->tiled = true;
            server_arrange(server);
        }

        return true;
    }

    case ACTION_TOGGLE_FULLSCREEN:
        if (server->focused_view) {
            view_toggle_fullscreen(server->focused_view);
        }
        return true;

    case ACTION_CENTER:
        if (server->focused_view) {
            view_center(server->focused_view);
            server_arrange(server);
        }
        return true;

    case ACTION_FOCUS_DIRECTION:
        focus_direction(server, bind->arg);
        return true;

    case ACTION_MOVE_DIRECTION:
        move_view_in_direction(server, bind->arg);
        return true;

    case ACTION_MOVE_OUTPUT_DIRECTION:
        move_view_to_output_direction(server, bind->arg);
        return true;

    case ACTION_RESIZE_DIRECTION:
        resize_view_in_direction(server, bind->arg);
        return true;
    }

    return false;
}

static bool handle_keybind_event(Server *server, uint32_t modifiers,
                                 uint32_t keycode,
                                 const xkb_keysym_t *syms, int nsyms) {
    ConfigKeybind *bind;

    wl_list_for_each(bind, &server->config.keybinds, link) {
        if ((modifiers & BIND_MODIFIER_MASK) != bind->modifiers) {
            continue;
        }

        bool match = false;

        if (bind->keycode != 0 && bind->keycode == keycode) {
            match = true;
        }

        if (!match && bind->keysym != XKB_KEY_NoSymbol && syms) {
            for (int i = 0; i < nsyms; i++) {
                xkb_keysym_t lower = xkb_keysym_to_lower(syms[i]);

                if (bind->keysym == lower || bind->keysym == syms[i]) {
                    match = true;
                    break;
                }
            }
        }

        if (match && handle_keybind_action(server, bind)) {
            return true;
        }
    }

    return false;
}

static View *view_from_surface(Server *server, struct wlr_surface *surface) {
    if (!surface) return NULL;

    struct wlr_surface *root = wlr_surface_get_root_surface(surface);
    if (!root) return NULL;

    View *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->type == VIEW_TYPE_XDG &&
            view->mapped &&
            view->xdg.xdg_surface &&
            view->xdg.xdg_surface->surface == root) {
            return view;
        }
    }

    return NULL;
}

static struct wlr_surface *surface_at_cursor(Server *server, double *sx, double *sy) {
    struct wlr_scene_node *node = wlr_scene_node_at(
        &server->scene_tree->node,
        server->cursor->x,
        server->cursor->y,
        sx,
        sy
    );

    if (!node || node->type != WLR_SCENE_NODE_BUFFER) {
        return NULL;
    }

    struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
    struct wlr_scene_surface *scene_surface = wlr_scene_surface_try_from_buffer(scene_buffer);

    if (!scene_surface) {
        return NULL;
    }

    return scene_surface->surface;
}

static void process_cursor(Server *server, uint64_t time_msec) {
    if (server->shutting_down) return;

    double sx = 0.0;
    double sy = 0.0;
    struct wlr_surface *surface = surface_at_cursor(server, &sx, &sy);

    if (surface) {
        View *view = view_from_surface(server, surface);

        if (view) {
            if (server->focused_view != view || server->focused_surface) {
                server_focus_view(server, view);
            }
        }

        wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
        wlr_seat_pointer_notify_motion(server->seat, time_msec, sx, sy);
        wlr_seat_pointer_notify_frame(server->seat);
        return;
    }

    View *view = NULL;
    View *v;
    wl_list_for_each(v, &server->views, link) {
        if (!v->mapped || v->fullscreen) continue;
        if (server->cursor->x >= v->x && server->cursor->x < v->x + v->width &&
            server->cursor->y >= v->y && server->cursor->y < v->y + v->height) {
            view = v;
            break;
        }
    }

    Output *cursor_output = output_at(server, server->cursor->x, server->cursor->y);
    if (cursor_output) {
        server->active_output = cursor_output;
    }

    if (view) {
        if (server->focused_view != view || server->focused_surface) {
            server_focus_view(server, view);
        }
        wlr_seat_pointer_clear_focus(server->seat);
        wlr_seat_pointer_notify_frame(server->seat);
        return;
    }

    server_focus_view(server, NULL);
    wlr_seat_keyboard_notify_clear_focus(server->seat);
    wlr_seat_pointer_clear_focus(server->seat);
    wlr_seat_pointer_notify_frame(server->seat);
}

static void process_drag(Server *server) {
    if (!server->drag.active || !server->drag.view) {
        server->drag.active = false;
        server->drag.move = false;
        server->drag.resize = false;
        server->drag.view = NULL;
        return;
    }

    View *view = server->drag.view;

    double total_dx = server->cursor->x - server->drag.grab_x;
    double total_dy = server->cursor->y - server->drag.grab_y;

    double inc_dx = server->cursor->x - server->drag.last_x;
    double inc_dy = server->cursor->y - server->drag.last_y;

    server->drag.last_x = server->cursor->x;
    server->drag.last_y = server->cursor->y;

    if (server->drag.move) {
        int new_x = server->drag.orig_x + (int)total_dx;
        int new_y = server->drag.orig_y + (int)total_dy;

        view_set_geometry(view, new_x, new_y, view->width, view->height);
    } else if (server->drag.resize) {
        if (server->drag.tiled_resize && view->output) {
            if (inc_dx != 0.0 || inc_dy != 0.0) {
                dwindle_resize(&view->output->layout, (int)inc_dx, (int)inc_dy);
                server_arrange(server);
            }
        } else {
            int offset_x = 0;
            int offset_y = 0;

            if (view->output) {
                offset_x = view->output->x;
                offset_y = view->output->y;
            }

            int rel_x = view->x - offset_x;
            int rel_y = view->y - offset_y;

            int new_width = server->drag.orig_width + (int)total_dx;
            int new_height = server->drag.orig_height + (int)total_dy;

            if (new_width < 100) new_width = 100;
            if (new_height < 100) new_height = 100;

            view_set_geometry(view, rel_x, rel_y, new_width, new_height);
        }
    }
}

static void handle_drag_destroy(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, drag_destroy);
    (void)data;

    if (server->drag_icon_tree) {
        wlr_scene_node_destroy(&server->drag_icon_tree->node);
        server->drag_icon_tree = NULL;
    }

    server->dnd_active = false;
    server->dnd_drag = NULL;

    wl_list_remove(&server->drag_focus.link);
    wl_list_init(&server->drag_focus.link);

    wl_list_remove(&server->drag_motion.link);
    wl_list_init(&server->drag_motion.link);

    wl_list_remove(&server->drag_drop.link);
    wl_list_init(&server->drag_drop.link);

    wl_list_remove(&server->drag_destroy.link);
    wl_list_init(&server->drag_destroy.link);
}

static void handle_drag_motion(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, drag_motion);
    (void)data;

    if (server->drag_icon_tree) {
        wlr_scene_node_set_position(
            &server->drag_icon_tree->node,
            (int)server->cursor->x,
            (int)server->cursor->y
        );
    }
}

static void handle_drag_drop(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;
}

static void handle_drag_focus(struct wl_listener *listener, void *data) {
	(void)listener;
	(void)data;
}

static void handle_start_drag(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, start_drag);
    struct wlr_drag *drag = data;

    server->dnd_active = true;
    server->dnd_drag = drag;

    server->drag_focus.notify = handle_drag_focus;
    wl_signal_add(&drag->events.focus, &server->drag_focus);

    server->drag_motion.notify = handle_drag_motion;
    wl_signal_add(&drag->events.motion, &server->drag_motion);

    server->drag_drop.notify = handle_drag_drop;
    wl_signal_add(&drag->events.drop, &server->drag_drop);

    server->drag_destroy.notify = handle_drag_destroy;
    wl_signal_add(&drag->events.destroy, &server->drag_destroy);

    if (drag->icon && drag->icon->surface) {
        server->drag_icon_tree = wlr_scene_tree_create(&server->scene->tree);
        if (server->drag_icon_tree) {
            wlr_scene_subsurface_tree_create(server->drag_icon_tree, drag->icon->surface);
            wlr_scene_node_raise_to_top(&server->drag_icon_tree->node);
            wlr_scene_node_set_position(
                &server->drag_icon_tree->node,
                (int)server->cursor->x,
                (int)server->cursor->y
            );
        }
    }
}

static void handle_request_start_drag(struct wl_listener *listener, void *data) {
	Server *server = wl_container_of(listener, server, request_start_drag);
	struct wlr_seat_request_start_drag_event *event = data;
	if (wlr_seat_validate_pointer_grab_serial(server->seat, event->origin, event->serial)) {
		wlr_seat_start_pointer_drag(server->seat, event->drag, event->serial);
	} else {
		wlr_data_source_destroy(event->drag->source);
	}
}

static void handle_cursor_motion(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, cursor_motion);
    struct wlr_pointer_motion_event *event = data;
    if (server->shutting_down) return;

    wlr_cursor_move(server->cursor, &event->pointer->base, event->delta_x, event->delta_y);

    if (server->drag_icon_tree) {
        wlr_scene_node_set_position(
            &server->drag_icon_tree->node,
            (int)server->cursor->x,
            (int)server->cursor->y
        );
    }

    if (server->drag.active) {
        process_drag(server);
    } else {
        process_cursor(server, event->time_msec);
    }
}

static void handle_cursor_motion_absolute(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, cursor_motion_absolute);
    struct wlr_pointer_motion_absolute_event *event = data;
    if (server->shutting_down) return;

    wlr_cursor_warp_absolute(server->cursor, &event->pointer->base, event->x, event->y);

    if (server->drag_icon_tree) {
        wlr_scene_node_set_position(
            &server->drag_icon_tree->node,
            (int)server->cursor->x,
            (int)server->cursor->y
        );
    }

    if (server->drag.active) {
        process_drag(server);
    } else {
        process_cursor(server, event->time_msec);
    }
}

static bool surface_allows_keyboard(Server *server, struct wlr_surface *surface) {
    if (!surface) return false;

    struct wlr_surface *root = wlr_surface_get_root_surface(surface);

    LayerSurface *layer;
    wl_list_for_each(layer, &server->layer_surfaces, link) {
        if (layer->layer_surface && layer->layer_surface->surface == root) {
            return layer->layer_surface->current.keyboard_interactive != 0;
        }
    }

    return true;
}

static void handle_cursor_button(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, cursor_button);
    struct wlr_pointer_button_event *event = data;
    if (server->shutting_down) return;

    uint32_t modifiers = 0;
    if (server->active_keyboard) {
        modifiers = wlr_keyboard_get_modifiers(server->active_keyboard);
    }

    bool super_pressed = (modifiers & WLR_MODIFIER_LOGO) != 0;

    if (super_pressed && event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        double sx = 0.0;
        double sy = 0.0;
        struct wlr_surface *surface = surface_at_cursor(server, &sx, &sy);
        View *view = surface ? view_from_surface(server, surface) : NULL;

        if (!view && !surface) {
            View *v;
            wl_list_for_each(v, &server->views, link) {
                if (!v->mapped || v->fullscreen) continue;
                if (server->cursor->x >= v->x && server->cursor->x < v->x + v->width &&
                    server->cursor->y >= v->y && server->cursor->y < v->y + v->height) {
                    view = v;
                    break;
                }
            }
        }

        if (view && !view->fullscreen) {
            bool start_drag = false;

            if (event->button == BTN_LEFT) {
                server->drag.move = true;
                server->drag.resize = false;
                start_drag = true;
            } else if (event->button == BTN_RIGHT) {
                server->drag.move = false;
                server->drag.resize = true;
                start_drag = true;
            }

            if (start_drag) {
                server_focus_view(server, view);

                server->drag.active = true;
                server->drag.view = view;
                server->drag.output = view->output;
                server->drag.grab_x = server->cursor->x;
                server->drag.grab_y = server->cursor->y;
                server->drag.last_x = server->cursor->x;
                server->drag.last_y = server->cursor->y;
                server->drag.restore_tiled = false;
                server->drag.tiled_resize = false;

                int offset_x = 0;
                int offset_y = 0;
                if (view->output) {
                    offset_x = view->output->x;
                    offset_y = view->output->y;
                }

                server->drag.orig_x = view->x - offset_x;
                server->drag.orig_y = view->y - offset_y;
                server->drag.orig_width = view->width;
                server->drag.orig_height = view->height;

                if (server->drag.move) {
                    if (view->tiled && view->output) {
                        dwindle_remove_view(&view->output->layout, view);
                        view->tiled = false;
                        view->floating = true;
                        server->drag.restore_tiled = true;
                        server_arrange(server);
                    }
                } else if (server->drag.resize) {
                    if (view->tiled && view->output) {
                        server->drag.tiled_resize = true;
                    }
                }

                wlr_seat_pointer_clear_focus(server->seat);
                return;
            }
        }
    }

    if (event->state == WL_POINTER_BUTTON_STATE_RELEASED && server->drag.active) {
        View *view = server->drag.view;
        bool restore_tiled = server->drag.restore_tiled;

        if (view) {
            Output *target = output_at(server, server->cursor->x, server->cursor->y);

            if (target && target != view->output) {
                if (restore_tiled) {
                    view->output = target;
                } else {
                    int abs_x = view->x;
                    int abs_y = view->y;
                    view->output = target;
                    view_set_geometry(view, abs_x - target->x, abs_y - target->y,
                        view->width, view->height);
                }
            }

            if (restore_tiled) {
                Output *out = target ? target : view->output;
                view->floating = false;

                if (out) {
                    view->output = out;

                    int rel_x = (int)server->cursor->x - out->x;
                    int rel_y = (int)server->cursor->y - out->y;

                    View *under = dwindle_view_at(&out->layout, rel_x, rel_y);

                    if (under && under != view && under->tiled && !under->fullscreen) {
                        dwindle_place_view(&out->layout, view, under, rel_x, rel_y);
                        view->tiled = true;
                    } else {
                        dwindle_add_view(&out->layout, view);
                        view->tiled = true;
                    }
                }
            }
        }

        if (view) {
            server_focus_view(server, view);
            server_arrange(server);
        }

        server->drag.active = false;
        server->drag.move = false;
        server->drag.resize = false;
        server->drag.restore_tiled = false;
        server->drag.tiled_resize = false;
        server->drag.view = NULL;
        server->drag.output = NULL;

        process_cursor(server, event->time_msec);
        return;
    }

    if (event->state == WL_POINTER_BUTTON_STATE_PRESSED) {
        double sx = 0.0;
        double sy = 0.0;
        struct wlr_surface *surface = surface_at_cursor(server, &sx, &sy);

        if (surface && !view_from_surface(server, surface) &&
            surface_allows_keyboard(server, surface)) {
            server_focus_surface(server, surface);
        }
    }

    process_cursor(server, event->time_msec);

    wlr_seat_pointer_notify_button(
        server->seat,
        event->time_msec,
        event->button,
        event->state
    );

    wlr_seat_pointer_notify_frame(server->seat);
}


static int vt_from_keycode(uint32_t keycode) {
    switch (keycode) {
    case KEY_F1: return 1;
    case KEY_F2: return 2;
    case KEY_F3: return 3;
    case KEY_F4: return 4;
    case KEY_F5: return 5;
    case KEY_F6: return 6;
    case KEY_F7: return 7;
    case KEY_F8: return 8;
    case KEY_F9: return 9;
    case KEY_F10: return 10;
    case KEY_F11: return 11;
    case KEY_F12: return 12;
    default: return 0;
    }
}

static void handle_key(struct wl_listener *listener, void *data) {
    Keyboard *keyboard = wl_container_of(listener, keyboard, key);
    Server *server = keyboard->server;

    if (server->shutting_down) return;

    struct wlr_keyboard_key_event *event = data;

    server->active_keyboard = keyboard->wlr_keyboard;

    bool handled = false;

    if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED) {
        uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);

        const xkb_keysym_t *syms = NULL;
        int nsyms = 0;

        struct xkb_state *state = keyboard->wlr_keyboard->xkb_state;
        if (state) {
            nsyms = xkb_state_key_get_syms(state, event->keycode + 8, &syms);
        }

        handled = handle_keybind_event(server, modifiers, event->keycode, syms, nsyms);

        if (!handled) {
            if ((modifiers & BIND_MODIFIER_MASK) == (WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT)) {
                int vt = vt_from_keycode(event->keycode);

                if (vt > 0 && server->session) {
                    wlr_session_change_vt(server->session, (unsigned)vt);
                    handled = true;
                }
            }
        }

        if (!handled) {
            if ((modifiers & BIND_MODIFIER_MASK) == WLR_MODIFIER_LOGO &&
                event->keycode == KEY_ESC) {
                wl_display_terminate(server->display);
                handled = true;
            }
        }
    }

    if (!handled) {
        wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
        wlr_seat_keyboard_notify_key(server->seat, event->time_msec, event->keycode, event->state);
    }
}

static void handle_modifiers(struct wl_listener *listener, void *data) {
    Keyboard *keyboard = wl_container_of(listener, keyboard, modifiers);
    Server *server = keyboard->server;
    (void)data;

    server->active_keyboard = keyboard->wlr_keyboard;

    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_modifiers(server->seat, &keyboard->wlr_keyboard->modifiers);
}

static void handle_keyboard_destroy(struct wl_listener *listener, void *data) {
    Keyboard *keyboard = wl_container_of(listener, keyboard, destroy);
    Server *server = keyboard->server;
    (void)data;

    wl_list_remove(&keyboard->key.link);
    wl_list_remove(&keyboard->modifiers.link);
    wl_list_remove(&keyboard->destroy.link);
    wl_list_remove(&keyboard->link);

    if (server->active_keyboard == keyboard->wlr_keyboard) {
        server->active_keyboard = NULL;
    }

    wlr_seat_set_keyboard(server->seat, NULL);

    free(keyboard);
}

static Keyboard *keyboard_create(Server *server, struct wlr_input_device *device) {
    Keyboard *keyboard = calloc(1, sizeof(Keyboard));
    if (!keyboard) return NULL;

    keyboard->server = server;
    keyboard->device = device;
    keyboard->wlr_keyboard = wlr_keyboard_from_input_device(device);

    if (!keyboard->wlr_keyboard) {
        free(keyboard);
        return NULL;
    }

    struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!context) {
        free(keyboard);
        return NULL;
    }

    struct xkb_keymap *keymap = xkb_keymap_new_from_names(
        context,
        NULL,
        XKB_KEYMAP_COMPILE_NO_FLAGS
    );

    if (!keymap) {
        xkb_context_unref(context);
        free(keyboard);
        return NULL;
    }

    wlr_keyboard_set_keymap(keyboard->wlr_keyboard, keymap);
    xkb_keymap_unref(keymap);
    xkb_context_unref(context);

    wlr_keyboard_set_repeat_info(keyboard->wlr_keyboard, 25, 600);

    keyboard->key.notify = handle_key;
    wl_signal_add(&keyboard->wlr_keyboard->events.key, &keyboard->key);

    keyboard->modifiers.notify = handle_modifiers;
    wl_signal_add(&keyboard->wlr_keyboard->events.modifiers, &keyboard->modifiers);

    keyboard->destroy.notify = handle_keyboard_destroy;
    wl_signal_add(&device->events.destroy, &keyboard->destroy);

    wl_list_insert(&server->keyboards, &keyboard->link);

    wlr_seat_set_keyboard(server->seat, keyboard->wlr_keyboard);
    server->active_keyboard = keyboard->wlr_keyboard;

    return keyboard;
}

static void handle_new_input(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, new_input);
    struct wlr_input_device *device = data;

    if (device->type == WLR_INPUT_DEVICE_KEYBOARD) {
        keyboard_create(server, device);
    } else if (device->type == WLR_INPUT_DEVICE_POINTER) {
        wlr_cursor_attach_input_device(server->cursor, device);
    }

    wlr_seat_set_capabilities(
        server->seat,
        WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER
    );
}

static void handle_cursor_axis(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, cursor_axis);
    struct wlr_pointer_axis_event *event = data;
    if (server->shutting_down) return;
    if (server->drag.active) return;

    if (server->drag_icon_tree) {
        wlr_scene_node_set_position(
            &server->drag_icon_tree->node,
            (int)server->cursor->x,
            (int)server->cursor->y
        );
    }

    process_cursor(server, event->time_msec);

    wlr_seat_pointer_notify_axis(
        server->seat,
        event->time_msec,
        event->orientation,
        event->delta,
        event->delta_discrete,
        event->source,
        event->relative_direction
    );

    wlr_seat_pointer_notify_frame(server->seat);
}

void input_init(Server *server) {
    wl_list_init(&server->keyboards);

    server->cursor = wlr_cursor_create();
    wlr_cursor_attach_output_layout(server->cursor, server->output_layout);

    server->xcursor_manager = wlr_xcursor_manager_create(NULL, 24);
    if (server->xcursor_manager) {
        wlr_xcursor_manager_load(server->xcursor_manager, 1.0f);
        wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, "left_ptr");
    }

    server->cursor_motion.notify = handle_cursor_motion;
    wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);

    server->cursor_motion_absolute.notify = handle_cursor_motion_absolute;
    wl_signal_add(&server->cursor->events.motion_absolute, &server->cursor_motion_absolute);

    server->cursor_button.notify = handle_cursor_button;
    wl_signal_add(&server->cursor->events.button, &server->cursor_button);

    server->cursor_axis.notify = handle_cursor_axis;
    wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);

    server->new_input.notify = handle_new_input;
    wl_signal_add(&server->backend->events.new_input, &server->new_input);

   	server->request_start_drag.notify = handle_request_start_drag;
	wl_signal_add(&server->seat->events.request_start_drag, &server->request_start_drag);

	server->start_drag.notify = handle_start_drag;
	wl_signal_add(&server->seat->events.start_drag, &server->start_drag);

    wlr_seat_set_capabilities(
        server->seat,
        WL_SEAT_CAPABILITY_KEYBOARD | WL_SEAT_CAPABILITY_POINTER
    );
}

void input_destroy(Server *server) {
    wl_list_remove(&server->new_input.link);

    wl_list_remove(&server->cursor_motion.link);
    wl_list_remove(&server->cursor_motion_absolute.link);
    wl_list_remove(&server->cursor_button.link);
    wl_list_remove(&server->cursor_axis.link);

    Keyboard *keyboard;
    Keyboard *tmp;

    wl_list_for_each_safe(keyboard, tmp, &server->keyboards, link) {
        wl_list_remove(&keyboard->key.link);
        wl_list_remove(&keyboard->modifiers.link);
        wl_list_remove(&keyboard->destroy.link);
        wl_list_remove(&keyboard->link);
        free(keyboard);
    }

    wl_list_init(&server->keyboards);
    server->active_keyboard = NULL;
   	wl_list_remove(&server->request_start_drag.link);
	wl_list_remove(&server->start_drag.link);
    wlr_seat_set_keyboard(server->seat, NULL);

    if (server->cursor) {
        wlr_cursor_destroy(server->cursor);
        server->cursor = NULL;
    }

    if (server->xcursor_manager) {
        wlr_xcursor_manager_destroy(server->xcursor_manager);
        server->xcursor_manager = NULL;
    }
}
