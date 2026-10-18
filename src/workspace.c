#include "workspace.h"
#include "server.h"
#include "output.h"
#include "view.h"
#include "layout.h"
#include "ext_workspace.h"
#include <stdlib.h>
#include "input.h"
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_xcursor_manager.h>

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
	dwindle_init(&ws->layout);
	wl_list_init(&ws->views);
	wl_list_init(&ws->link);
	return ws;
}

void workspace_destroy(Workspace *ws) {
	if (!ws) return;
	workspace_clear_saved(ws);
	dwindle_destroy(&ws->layout);
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

    WorkspaceView *existing;
    wl_list_for_each(existing, &ws->views, link) {
        if (existing->view == view) {
            view->workspace = ws;
            view->output = ws->output;
            dwindle_add_view(&ws->layout, view);
            return;
        }
    }

    WorkspaceView *wv = calloc(1, sizeof(WorkspaceView));
    if (!wv) return;

    view->workspace = ws;
    view->output = ws->output;

    wv->view = view;
    wl_list_insert(ws->views.prev, &wv->link);

    dwindle_add_view(&ws->layout, view);

    if (!ws->output && view->root_tree) {
        wlr_scene_node_set_enabled(&view->root_tree->node, false);
    }
}

void workspace_remove_view(Workspace *ws, View *view) {
    if (!ws || !view) return;

    dwindle_remove_view(&ws->layout, view);

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

View *workspace_focused_view(Workspace *ws) {
	if (!ws) return NULL;
	return dwindle_focused_view(&ws->layout);
}

View *workspace_first_view(Workspace *ws) {
	if (!ws) return NULL;
	return dwindle_first_view(&ws->layout);
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

bool workspace_empty(Server *server, Workspace *ws) {
	if (!ws) return true;
	if (!server) return wl_list_empty(&ws->views);

	View *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->workspace == ws && view->mapped && !view->sticky) {
			return false;
		}
	}

	return true;
}

void workspace_hide(Workspace *ws) {
	if (!ws) return;

	if (!ws->output) {
		if (wl_list_empty(&ws->views)) {
			ws->last_output = NULL;
		}
		return;
	}

	Output *out = ws->output;
	Server *server = out->server;

	View *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->workspace == ws && view->output == out && view->mapped && !view->sticky) {
			if (view->root_tree) {
				wlr_scene_node_set_enabled(&view->root_tree->node, false);
			}
			view->output = NULL;
		}
	}

	if (workspace_empty(server, ws)) {
		ws->last_output = NULL;
	}

	ws->output = NULL;
}

void workspace_show(Workspace *ws, Output *out) {
	if (!ws || !out) return;
	ws->output = out;
	ws->last_output = out;
	Server *server = out->server;
	View *view;
	wl_list_for_each(view, &server->views, link) {
		if (view->workspace == ws && view->mapped && !view->sticky) {
			view->output = out;
			if (view->root_tree) {
				wlr_scene_node_set_enabled(&view->root_tree->node, true);
			}
			if (view->fullscreen) {
				view_set_geometry(view, 0, 0, out->width, out->height);
			} else if (view->floating) {
				int rel_x = view->x - out->x;
				int rel_y = view->y - out->y;
				if (rel_x < 0 || rel_y < 0 || rel_x >= out->width || rel_y >= out->height) {
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
			}
		}
	}
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

	View *source_fs = output_fullscreen_view(output);
	if (source_fs && server->active_constraint) {
		wlr_pointer_constraint_v1_send_deactivated(server->active_constraint);
		server->active_constraint = NULL;
		server->cursor_hidden = false;
		server->client_cursor_hidden = false;
		server->client_cursor_hidden_time = 0;
		if (server->xcursor_manager) {
			wlr_cursor_set_xcursor(server->cursor, server->xcursor_manager, "left_ptr");
		}
	}

	if (workspace_empty(server, target) && target->output && target->output != output) {
		Output *old_out = target->output;
		bool was_active = (old_out->active_workspace == target);

		workspace_hide(target);
		target->last_output = NULL;

		if (was_active) {
			Workspace *replacement = NULL;
			Workspace *w;

			wl_list_for_each(w, &server->workspaces, link) {
				if (w != target && !w->output && workspace_empty(server, w)) {
					replacement = w;
					break;
				}
			}

			if (!replacement) {
				wl_list_for_each(w, &server->workspaces, link) {
					if (w != target && !w->output) {
						replacement = w;
						break;
					}
				}
			}

			old_out->active_workspace = replacement;

			if (replacement) {
				workspace_show(replacement, old_out);
			}
		}
	}

	Output *dest = output;

	if (target->output) {
		dest = target->output;
	} else if (target->last_output && !workspace_empty(server, target)) {
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

			dest->render_pending = true;

			if (dest->wlr_output) {
				wlr_output_schedule_frame(dest->wlr_output);
			}
		}

		input_refocus_pointer(server);
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
	input_refocus_pointer(server);
	ext_workspace_sync(server);
}
