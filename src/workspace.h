#ifndef WORKSPACE_H
#define WORKSPACE_H

#include <wayland-server.h>
#include "view.h"

typedef struct {
    int id;
    struct wl_list views;
    struct wl_list link;
    bool active;
} Workspace;

Workspace *workspace_create(int id);
void workspace_destroy(Workspace *workspace);
void workspace_add_view(Workspace *workspace, View *view);
void workspace_remove_view(Workspace *workspace, View *view);
View *workspace_get_focused(Workspace *workspace);

#endif
