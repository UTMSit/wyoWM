#ifndef EXT_WORKSPACE_H
#define EXT_WORKSPACE_H

struct Server;
struct Output;

void ext_workspace_init(struct Server *server);
void ext_workspace_destroy(struct Server *server);
void ext_workspace_sync(struct Server *server);
void ext_workspace_output_add(struct Server *server, struct Output *output);
void ipc_notify_workspaces(struct Server *server);

#endif
