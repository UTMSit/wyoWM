#include "workspace.h"
#include "server.h"
#include "output.h"
#include "view.h"
#include "layout.h"
#include "ext_workspace.h"
#include <stdlib.h>

typedef struct WorkspaceView {
    View *view;
    struct wl_list link;
} WorkspaceView;

static void workspace_clear_saved(Workspace *ws) {
    if (!ws) return;

    WorkspaceView *wv;
    WorkspaceView *tmp;

    wl_list_for_each_safe(wv, tmp, &ws->views, link) {
        wl_list_remove(&wv->link);
        free(wv);
    }

    wl_list_init(&ws->views);
}

Workspace *workspace_create(int id) {
    Workspace *ws = calloc(1, sizeof(Workspace));
    if (!ws) return NULL;

    ws->id = id;
    ws->active = false;
    ws->output = NULL;
    ws->handle = NULL;

    wl_list_init(&ws->views);
    wl_list_init(&ws->link);

    return ws;
}

void workspace_destroy(Workspace *ws) {
    if (!ws) return;

    workspace_clear_saved(ws);

    if (!wl_list_empty(&ws->link)) {
        wl_list_remove(&ws->link);
        wl_list_init(&ws->link);
    }

    free(ws);
}

void workspaces_init(Server *server) {
    if (!server) return;

    wl_list_init(&server->workspaces);

    for (int i = 1; i <= 10; i++) {
        Workspace *ws = workspace_create(i);
        if (ws) {
            wl_list_insert(server->workspaces.prev, &ws->link);
        }
    }
}

void workspaces_destroy(Server *server) {
    if (!server) return;

    Workspace *ws;
    Workspace *tmp;

    wl_list_for_each_safe(ws, tmp, &server->workspaces, link) {
        workspace_destroy(ws);
    }

    wl_list_init(&server->workspaces);
}

void workspace_add_view(Workspace *ws, View *view) {
    if (!ws || !view) return;

    view->workspace = ws;

    if (ws->output) {
        view->output = ws->output;
        dwindle_add_view(&ws->output->layout, view);
    } else {
        view->output = NULL;

        WorkspaceView *existing;
        wl_list_for_each(existing, &ws->views, link) {
            if (existing->view == view) {
                return;
            }
        }

        WorkspaceView *wv = calloc(1, sizeof(WorkspaceView));
        if (!wv) return;

        wv->view = view;
        wl_list_insert(ws->views.prev, &wv->link);

        if (view->root_tree) {
            wlr_scene_node_set_enabled(&view->root_tree->node, false);
        }
    }
}

void workspace_remove_view(Workspace *ws, View *view) {
    if (!ws || !view) return;

    if (ws->output) {
        dwindle_remove_view(&ws->output->layout, view);
    } else {
        WorkspaceView *wv;
        WorkspaceView *tmp;

        wl_list_for_each_safe(wv, tmp, &ws->views, link) {
            if (wv->view == view) {
                wl_list_remove(&wv->link);
                free(wv);
                break;
            }
        }
    }
}

View *workspace_focused_view(Workspace *ws) {
    if (!ws || !ws->output) return NULL;
    return dwindle_focused_view(&ws->output->layout);
}

View *workspace_first_view(Workspace *ws) {
    if (!ws || !ws->output) return NULL;
    return dwindle_first_view(&ws->output->layout);
}

View *workspace_get_focused(Workspace *ws) {
    return workspace_focused_view(ws);
}
Workspace *workspace_find(Server *server, int id) {
    if (!server) return NULL;
    Workspace *ws;
    wl_list_for_each(ws, &server->workspaces, link) {
        if (ws->id == id) {
            return ws;
        }
    }
    return NULL;
}
Workspace *workspace_first_free(Server *server) {
    if (!server) return NULL;
    Workspace *ws;
    wl_list_for_each(ws, &server->workspaces, link) {
        if (!ws->output) {
            return ws;
        }
    }
    return NULL;
}

bool workspace_empty(Workspace *ws) {
    if (!ws) return true;

    if (ws->output) {
        Server *server = ws->output->server;

        View *view;
        wl_list_for_each(view, &server->views, link) {
            if (view->workspace == ws && view->mapped) {
                return false;
            }
        }

        return true;
    }

    return wl_list_empty(&ws->views);
}

void workspace_hide(Workspace *ws) {
    if (!ws || !ws->output) return;

    Output *out = ws->output;
    Server *server = out->server;

    workspace_clear_saved(ws);

    View *view;

    wl_list_for_each(view, &server->views, link) {
        if (view->workspace == ws && view->output == out && view->mapped) {
            WorkspaceView *wv = calloc(1, sizeof(WorkspaceView));
            if (wv) {
                wv->view = view;
                wl_list_insert(ws->views.prev, &wv->link);
            }
        }
    }

    wl_list_for_each(view, &server->views, link) {
        if (view->workspace == ws && view->output == out && view->mapped) {
            if (view->tiled) {
                dwindle_remove_view(&out->layout, view);
                view->tiled = false;
            }

            if (view->root_tree) {
                wlr_scene_node_set_enabled(&view->root_tree->node, false);
            }

            view->output = NULL;
        }
    }

    ws->output = NULL;
}

void workspace_show(Workspace *ws, Output *out) {
    if (!ws || !out) return;
    ws->output = out;
    ws->last_output = out;

    WorkspaceView *wv;
    WorkspaceView *tmp;

    wl_list_for_each_safe(wv, tmp, &ws->views, link) {
        View *view = wv->view;

        if (view && view->mapped) {
            view->output = out;
            view->workspace = ws;

            if (view->root_tree) {
                wlr_scene_node_set_enabled(&view->root_tree->node, true);
            }

            if (view->fullscreen) {
                view_set_geometry(view, 0, 0, out->width, out->height);
            } else if (view->floating) {
                int rel_x = view->x - out->x;
                int rel_y = view->y - out->y;

                if (rel_x < 0 || rel_y < 0 ||
                    rel_x >= out->width || rel_y >= out->height) {
                    int width = view->width > 0 ? view->width : 800;
                    int height = view->height > 0 ? view->height : 600;

                    int x = (out->width - width) / 2;
                    int y = (out->height - height) / 2;

                    if (x < 0) x = 0;
                    if (y < 0) y = 0;

                    view_set_geometry(view, x, y, width, height);
                } else {
                    view_set_geometry(view, rel_x, rel_y, view->width, view->height);
                }
            } else {
                dwindle_add_view(&out->layout, view);
                view->tiled = true;
            }
        }

        wl_list_remove(&wv->link);
        free(wv);
    }

    wl_list_init(&ws->views);
}

void server_switch_workspace(Server *server, Output *output, int id) {
	if (!server || server->shutting_down || !output) return;

	Workspace *target = NULL;
	Workspace *current = output->active_workspace;
	Workspace *ws;
	wl_list_for_each(ws, &server->workspaces, link) {
		if (ws->id == id) {
			target = ws;
			break;
		}
	}

	if (!target || target == current) return;

	if (workspace_empty(target) && target->output && target->output != output) {
		Output *old_out = target->output;
		bool was_active = (old_out->active_workspace == target);
		workspace_hide(target);
		target->last_output = NULL;
		if (was_active) {
			Workspace *replacement = NULL;
			Workspace *w;
			wl_list_for_each(w, &server->workspaces, link) {
				if (w != target && !w->output) {
					replacement = w;
					break;
				}
			}
			if (replacement) {
				old_out->active_workspace = replacement;
				workspace_show(replacement, old_out);
			} else {
				old_out->active_workspace = NULL;
			}
		}
	}

	Output *dest = output;
	if (target->output) {
		dest = target->output;
	} else if (target->last_output) {
		dest = target->last_output;
	}

	if (dest != output) {
		Workspace *current_dest = dest->active_workspace;
		if (current_dest && current_dest != target) {
			workspace_hide(current_dest);
		}
		target->output = dest;
		dest->active_workspace = target;
		workspace_show(target, dest);

		server->active_output = dest;
		server_arrange(server);

		View *next = workspace_focused_view(target);
		if (!next) {
			next = workspace_first_view(target);
		}
		server_focus_view(server, next);

		if (server->cursor) {
			wlr_cursor_warp(
				server->cursor,
				NULL,
				dest->x + dest->width / 2.0,
				dest->y + dest->height / 2.0
			);
			if (dest->wlr_output) {
				wlr_output_schedule_frame(dest->wlr_output);
			}
		}
		ext_workspace_sync(server);
		return;
	}

	if (current) {
		workspace_hide(current);
	}

	target->output = output;
	output->active_workspace = target;
	workspace_show(target, output);

	server_arrange(server);

	View *next = workspace_focused_view(target);
	if (!next) {
		next = workspace_first_view(target);
	}
	server_focus_view(server, next);
	ext_workspace_sync(server);
}
