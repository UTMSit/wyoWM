#ifndef IPC_H
#define IPC_H

#include <stddef.h>
#include <sys/types.h>
#include <wayland-server-core.h>

struct Server;

typedef struct IPCClient {
    int fd;
    struct wl_event_source *source;
    struct wl_list link;
    struct Server *server;
    char buf[4096];
    size_t len;
} IPCClient;

typedef struct {
    int socket_fd;
    char *socket_path;
    struct wl_event_loop *loop;
    struct wl_event_source *listen_source;
    struct wl_list clients;
    struct Server *server;
} IPCServer;

int ipc_init(IPCServer *server, struct Server *compositor, struct wl_event_loop *loop, const char *path);
void ipc_destroy(IPCServer *server);

#endif
