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

    if (output->wallpaper_fade_anim.active) {
        animation_update(&output->wallpaper_fade_anim, now);
        if (output->wallpaper_fade) {
            wlr_scene_buffer_set_opacity(output->wallpaper_fade, (float)output->wallpaper_fade_anim.current);
        }
        if (animation_finished(&output->wallpaper_fade_anim)) {
            if (output->wallpaper_fade) {
                wlr_scene_node_destroy(&output->wallpaper_fade->node);
                output->wallpaper_fade = NULL;
            }
        }
        active = true;
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

static bool output_try_state(struct wlr_output *wlr_output, struct wlr_output_mode *mode) {
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true);
    if (mode) {
        wlr_output_state_set_mode(&state, mode);
    }
    bool ok = wlr_output_commit_state(wlr_output, &state);
    wlr_output_state_finish(&state);
    return ok;
}

static bool output_commit_enabled(struct wlr_output *wlr_output) {
    struct wlr_output_mode *preferred = wlr_output_preferred_mode(wlr_output);

    if (preferred && output_try_state(wlr_output, preferred)) {
        return true;
    }

    if (wlr_output->current_mode && output_try_state(wlr_output, wlr_output->current_mode)) {
        return true;
    }

    struct wlr_output_mode *mode;
    wl_list_for_each(mode, &wlr_output->modes, link) {
        if (mode == preferred || mode == wlr_output->current_mode) {
            continue;
        }
        if (output_try_state(wlr_output, mode)) {
            return true;
        }
    }

    return output_try_state(wlr_output, NULL);
}

Output *output_create(Server *server, struct wlr_output *wlr_output) {
    Output *output = calloc(1, sizeof(Output));
    if (!output) return NULL;

    output->server = server;
    output->wlr_output = wlr_output;

    dwindle_init(&output->layout);
    dwindle_set_gaps(&output->layout, server->config.gaps_in, server->config.gaps_out);

    wlr_output_init_render(wlr_output, server->allocator, server->renderer);
    if (!output_commit_enabled(wlr_output)) {
        free(output);
        return NULL;
    }

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

    wlr_scene_output_layout_add_output(
        server->scene_layout,
        output->layout_output,
        output->scene_output
    );

    output->frame.notify = handle_frame;
    wl_signal_add(&wlr_output->events.frame, &output->frame);

    output->destroy.notify = handle_destroy;
    wl_signal_add(&wlr_output->events.destroy, &output->destroy);

    wl_list_insert(&server->outputs, &output->link);
    ext_workspace_output_add(server, output);

    Workspace *ws = workspace_first_free(server);
    if (!ws) {
        ws = workspace_find(server, 1);
    }
    output->active_workspace = ws;
    output->wallpaper_fade = NULL;
    animation_init(&output->wallpaper_fade_anim, 0.0);
    if (ws) {
        ws->output = output;
        ws->active = true;
    }

    output_update_geometry(output);
    wallpaper_configure_output(output);

    if (ws) {
        workspace_show(ws, output);
    }

    View *view;
    wl_list_for_each(view, &server->views, link) {
        if (view->output || view->workspace) continue;

        view->output = output;

        if (!view->mapped) continue;

        if (output->active_workspace) {
            view->workspace = output->active_workspace;
            dwindle_add_view(&output->layout, view);
            view->tiled = true;
        }
    }

    if (!server->active_output) {
        server->active_output = output;
    }

    if (!server->shutting_down) {
        server_arrange(server);
        ipc_notify_workspaces(server);
    }

    return output;
}

void output_destroy(Output *output) {
    if (!output) return;

    Server *server = output->server;
    bool shutting_down = server->shutting_down;

    if (output->active_workspace) {
        workspace_hide(output->active_workspace);
        output->active_workspace = NULL;
    }
    Workspace *ws_cleanup;
    wl_list_for_each(ws_cleanup, &server->workspaces, link) {
        if (ws_cleanup->last_output == output) {
            ws_cleanup->last_output = NULL;
        }
    }

    if (!shutting_down) {
        Output *fallback = NULL;
        Output *candidate;

        wl_list_for_each(candidate, &server->outputs, link) {
            if (candidate != output) {
                fallback = candidate;
                break;
            }
        }

        if (server->active_output == output) {
            server->active_output = fallback;
        }

        if (server->focused_view &&
            (!server->focused_view->output || server->focused_view->output == output)) {
            View *next = NULL;

            if (fallback && fallback->active_workspace) {
                next = workspace_focused_view(fallback->active_workspace);
                if (!next) {
                    next = workspace_first_view(fallback->active_workspace);
                }
            }

            server_focus_view(server, next);
        }
    }

    wl_list_remove(&output->frame.link);
    wl_list_remove(&output->destroy.link);
    wl_list_remove(&output->link);

    dwindle_destroy(&output->layout);

    if (output->wallpaper_fade) {
        wlr_scene_node_destroy(&output->wallpaper_fade->node);
        output->wallpaper_fade = NULL;
    }

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
        ipc_notify_workspaces(server);
    }
}
