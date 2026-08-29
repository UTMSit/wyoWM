#ifndef WORKSPACE_H
#define WORKSPACE_H

#include <stdbool.h>
#include <wayland-server.h>
#include "view.h"

struct Server;
struct Output;
struct wlr_ext_workspace_handle_v1;

typedef struct Workspace {
    int id;
    struct wl_list views;
    struct wl_list link;
    bool active;
    struct Output *output;
    struct Output *last_output;
    struct wlr_ext_workspace_handle_v1 *handle;
} Workspace;

void workspaces_init(struct Server *server);
void workspaces_destroy(struct Server *server);

Workspace *workspace_create(int id);
void workspace_destroy(Workspace *workspace);

void workspace_add_view(Workspace *workspace, View *view);
void workspace_remove_view(Workspace *workspace, View *view);

View *workspace_focused_view(Workspace *workspace);
View *workspace_first_view(Workspace *workspace);
View *workspace_get_focused(Workspace *workspace);
bool workspace_empty(Workspace *workspace);
void server_switch_workspace(struct Server *server, struct Output *output, int id);
Workspace *workspace_find(struct Server *server, int id);
Workspace *workspace_first_free(struct Server *server);
void workspace_show(Workspace *workspace, struct Output *output);
void workspace_hide(Workspace *workspace);
#endif
