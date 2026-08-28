#include "output.h"
#include "server.h"
#include "view.h"
#include "wallpaper.h"
#include <stdlib.h>
#include <time.h>
#include <wlr/util/box.h>

static void handle_frame(struct wl_listener *listener, void *data) {
    Output *output = wl_container_of(listener, output, frame);
    (void)data;

    Server *server = output->server;

    if (server->shutting_down || !output->scene_output) return;

    int64_t now = animation_now_ms();

    bool active = false;

    View *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->output == output) {
            if (view_frame_update(view, now)) {
                active = true;
            }
        }
    }

    wlr_scene_output_commit(output->scene_output, NULL);

    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    wlr_scene_output_send_frame_done(output->scene_output, &ts);

    if (active) {
        wlr_output_schedule_frame(output->wlr_output);
    }
}

static void handle_destroy(struct wl_listener *listener, void *data) {
    Output *output = wl_container_of(listener, output, destroy);
    (void)data;

    output_destroy(output);
}

void output_update_geometry(Output *output) {
    Server *server = output->server;
    struct wlr_box box = {0};

    wlr_output_layout_get_box(server->output_layout, output->wlr_output, &box);

    if (box.width > 0 && box.height > 0) {
        output->x = box.x;
        output->y = box.y;
        output->width = box.width;
        output->height = box.height;
    } else {
        output->x = 0;
        output->y = 0;
        output->width = output->wlr_output->width;
        output->height = output->wlr_output->height;
    }
}

Output *output_at(Server *server, double lx, double ly) {
    if (!server || !server->output_layout) return NULL;

    struct wlr_output *wlr_output = wlr_output_layout_output_at(server->output_layout, lx, ly);
    if (!wlr_output) return NULL;

    Output *output;
    wl_list_for_each(output, &server->outputs, link) {
        if (output->wlr_output == wlr_output) {
            return output;
        }
    }

    return NULL;
}

Output *output_create(Server *server, struct wlr_output *wlr_output) {
    Output *output = calloc(1, sizeof(Output));
    if (!output) return NULL;

    output->server = server;
    output->wlr_output = wlr_output;

    dwindle_init(&output->layout);
    dwindle_set_gaps(&output->layout, server->config.gaps_in, server->config.gaps_out);

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);

    struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    if (mode) {
        wlr_output_state_set_mode(&state, mode);
    }
    wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);

    output->layout_output = wlr_output_layout_add_auto(server->output_layout, wlr_output);
    if (!output->layout_output) {
        free(output);
        return NULL;
    }

    output->scene_output = wlr_scene_output_create(server->scene, wlr_output);
    if (!output->scene_output) {
        wlr_output_layout_remove(server->output_layout, wlr_output);
        free(output);
        return NULL;
    }

    wlr_scene_output_layout_add_output(server->scene_layout, output->layout_output, output->scene_output);

    output->frame.notify = handle_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    output->destroy.notify = handle_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wl_list_insert(&server->outputs, &output->link);

    output_update_geometry(output);
    wallpaper_configure_output(output);

    View *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->output) continue;

        view->output = output;

        if (!view->mapped) continue;

        if (view->fullscreen) {
            view_set_geometry(view, 0, 0, output->width, output->height);
        } else if (view->floating) {
            int width = view->width > 0 ? view->width : 800;
            int height = view->height > 0 ? view->height : 600;

            int x = (output->width - width) / 2;
            int y = (output->height - height) / 2;

            if (x < 0) x = 0;
            if (y < 0) y = 0;

            view_set_geometry(view, x, y, width, height);
        } else {
            dwindle_add_view(&output->layout, view);
            view->tiled = true;
        }
    }

    if (!server->active_output) {
        server->active_output = output;
    }

    if (!server->shutting_down) {
        server_arrange(server);
    }

    return output;
}

void output_destroy(Output *output) {
    if (!output) return;

    Server *server = output->server;
    bool shutting_down = server->shutting_down;

    if (shutting_down) {
        View *view;
        wl_list_for_each(view, &server->views, link) {
            if (view->output != output) continue;

            if (view->tiled) {
                dwindle_remove_view(&output->layout, view);
                view->tiled = false;
            }

            view->output = NULL;
        }
    } else {
        Output *fallback = NULL;
        Output *candidate;

        wl_list_for_each(candidate, &server->outputs, link) {
            if (candidate != output) {
                fallback = candidate;
                break;
            }
        }

        View *view;
        wl_list_for_each(view, &server->views, link) {
            if (view->output != output) continue;

            if (view->tiled) {
                dwindle_remove_view(&output->layout, view);
                view->tiled = false;
            }

            view->output = fallback;

            if (!fallback) continue;

            if (view->fullscreen) {
                view_set_geometry(view, 0, 0, fallback->width, fallback->height);
                continue;
            }

            if (!view->mapped) continue;

          		if (view->floating) {
			int rel_x = view->x - fallback->x;
			int rel_y = view->y - fallback->y;
			if (rel_x < 0 || rel_y < 0 ||
				rel_x >= fallback->width ||
				rel_y >= fallback->height) {
                    int width = view->width > 0 ? view->width : 800;
                    int height = view->height > 0 ? view->height : 600;

                    rel_x = (fallback->width - width) / 2;
                    rel_y = (fallback->height - height) / 2;

                    if (rel_x < 0) rel_x = 0;
                    if (rel_y < 0) rel_y = 0;

                    view_set_geometry(view, rel_x, rel_y, width, height);
                } else {
                    view_set_geometry(view, rel_x, rel_y, view->width, view->height);
                }
            } else {
                dwindle_add_view(&fallback->layout, view);
                view->tiled = true;
            }
        }

        if (server->active_output == output) {
            server->active_output = fallback;
        }

        if (server->focused_view && server->focused_view->output == NULL) {
            View *next = NULL;

            if (fallback) {
                next = dwindle_focused_view(&fallback->layout);
                if (!next) next = dwindle_first_view(&fallback->layout);
            }

            server_focus_view(server, next);
        } else if (server->focused_view) {
            server_focus_view(server, server->focused_view);
        } else if (fallback) {
            View *next = dwindle_focused_view(&fallback->layout);
            if (!next) next = dwindle_first_view(&fallback->layout);
            server_focus_view(server, next);
        } else {
            server_focus_view(server, NULL);
        }
    }

    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);

    dwindle_destroy(&output->layout);

    if (output->wallpaper) {
        wlr_scene_node_destroy(&output->wallpaper->node);
        output->wallpaper = NULL;
        output->wallpaper_buffer = NULL;
    }

    if (output->scene_output) {
        wlr_scene_output_destroy(output->scene_output);
        output->scene_output = NULL;
    }

    wlr_output_layout_remove(server->output_layout, output->wlr_output);

    free(output);

    if (!shutting_down) {
        server_arrange(server);
    }
}
