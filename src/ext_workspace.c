#include "ext_workspace.h"
#include "server.h"
#include "workspace.h"
#include "output.h"
#include <wlr/types/wlr_ext_workspace_v1.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void handle_commit(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, ext_workspace_commit);
    struct wlr_ext_workspace_v1_commit_event *event = data;

    struct wlr_ext_workspace_v1_request *req;
    wl_list_for_each(req, event->requests, link) {
        if (req->type == WLR_EXT_WORKSPACE_V1_REQUEST_ACTIVATE) {
            struct wlr_ext_workspace_handle_v1 *handle = req->activate.workspace;
            if (!handle) continue;

            Workspace *ws = handle->data;
            if (!ws) continue;

            Output *out = server->active_output;
            if (!out && server->focused_view && server->focused_view->output) {
                out = server->focused_view->output;
            }
            if (!out && !wl_list_empty(&server->outputs)) {
                out = wl_container_of(server->outputs.next, out, link);
            }

            if (out) {
                server_switch_workspace(server, out, ws->id);
            }
        }
    }
}

static void handle_destroy(struct wl_listener *listener, void *data) {
    Server *server = wl_container_of(listener, server, ext_workspace_destroy_listener);
    (void)data;

    wl_list_remove(&server->ext_workspace_commit.link);
    wl_list_remove(&server->ext_workspace_destroy_listener.link);
    server->ext_workspace_manager = NULL;
    server->ext_workspace_group = NULL;
}

void ext_workspace_init(Server *server) {
    if (!server || !server->display) return;

    server->ext_workspace_manager = wlr_ext_workspace_manager_v1_create(server->display, 1);
    if (!server->ext_workspace_manager) return;

    uint32_t group_caps = 1;
    server->ext_workspace_group = wlr_ext_workspace_group_handle_v1_create(
        server->ext_workspace_manager, group_caps);

    if (server->ext_workspace_group) {
        Output *out;
        wl_list_for_each(out, &server->outputs, link) {
            wlr_ext_workspace_group_handle_v1_output_enter(
                server->ext_workspace_group, out->wlr_output);
        }
    }

    Workspace *ws;
    wl_list_for_each(ws, &server->workspaces, link) {
        char id_str[16];
        char name_str[16];
        snprintf(id_str, sizeof(id_str), "%d", ws->id);
        snprintf(name_str, sizeof(name_str), "%d", ws->id);

        uint32_t ws_caps = 3;
        ws->handle = wlr_ext_workspace_handle_v1_create(
            server->ext_workspace_manager, id_str, ws_caps);

        if (ws->handle) {
            ws->handle->data = ws;
            wlr_ext_workspace_handle_v1_set_name(ws->handle, name_str);
            wlr_ext_workspace_handle_v1_set_group(ws->handle, server->ext_workspace_group);
        }
    }

    server->ext_workspace_commit.notify = handle_commit;
    wl_signal_add(&server->ext_workspace_manager->events.commit, &server->ext_workspace_commit);

    server->ext_workspace_destroy_listener.notify = handle_destroy;
    wl_signal_add(&server->ext_workspace_manager->events.destroy, &server->ext_workspace_destroy_listener);
}

void ext_workspace_destroy(Server *server) {
    if (!server) return;

    if (server->ext_workspace_manager) {
        if (!wl_list_empty(&server->ext_workspace_commit.link)) {
            wl_list_remove(&server->ext_workspace_commit.link);
            wl_list_init(&server->ext_workspace_commit.link);
        }

        if (!wl_list_empty(&server->ext_workspace_destroy_listener.link)) {
            wl_list_remove(&server->ext_workspace_destroy_listener.link);
            wl_list_init(&server->ext_workspace_destroy_listener.link);
        }
    }

    Workspace *ws;
    wl_list_for_each(ws, &server->workspaces, link) {
        if (ws->handle) {
            wlr_ext_workspace_handle_v1_destroy(ws->handle);
            ws->handle = NULL;
        }
    }

    if (server->ext_workspace_group) {
        wlr_ext_workspace_group_handle_v1_destroy(server->ext_workspace_group);
        server->ext_workspace_group = NULL;
    }

    if (server->ext_workspace_manager) {
        server->ext_workspace_manager = NULL;
    }
}

void ext_workspace_output_add(Server *server, Output *output) {
    if (!server || !output || !output->wlr_output) return;
    if (!server->ext_workspace_group) return;
    wlr_ext_workspace_group_handle_v1_output_enter(
        server->ext_workspace_group,
        output->wlr_output
    );
}
void ext_workspace_sync(Server *server) {
    if (!server || !server->ext_workspace_manager) return;

    Output *active_out = server->active_output;
    Workspace *active_ws = NULL;

    if (active_out && active_out->active_workspace) {
        active_ws = active_out->active_workspace;
    }

    Workspace *ws;
    wl_list_for_each(ws, &server->workspaces, link) {
        if (!ws->handle) continue;

        bool is_active = (ws == active_ws);
        wlr_ext_workspace_handle_v1_set_active(ws->handle, is_active);
    }
}

void ipc_notify_workspaces(Server *server) {
    ext_workspace_sync(server);
}
