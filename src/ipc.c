#define _GNU_SOURCE
#include "ipc.h"
#include "server.h"
#include "workspace.h"
#include "output.h"
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <ctype.h>

static char *ipc_trim(char *s) {
    if (!s) return s;
    while (*s && isspace((unsigned char)*s)) s++;
    if (*s == '\0') return s;
    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }
    return s;
}

static bool ipc_write_all(int fd, const void *data, size_t len) {
    const char *p = data;
    while (len > 0) {
        ssize_t n = write(fd, p, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (n == 0) return false;
        p += (size_t)n;
        len -= (size_t)n;
    }
    return true;
}

static void ipc_client_remove(IPCClient *client) {
    if (!client) return;
    if (client->source) {
        wl_event_source_remove(client->source);
        client->source = NULL;
    }
    if (client->fd >= 0) {
        close(client->fd);
        client->fd = -1;
    }
    if (!wl_list_empty(&client->link)) {
        wl_list_remove(&client->link);
        wl_list_init(&client->link);
    }
    free(client);
}

static void ipc_build_workspaces(Server *server, char *out, size_t out_size) {
    if (!out || out_size == 0) return;

    int active_id = 0;
    Output *active = server->active_output;
    if (active && active->active_workspace) {
        active_id = active->active_workspace->id;
    }

    size_t off = 0;
    int n = snprintf(out + off, out_size - off, "{\"active\":%d,\"workspaces\":[", active_id);
    if (n < 0 || (size_t)n >= out_size - off) {
        out[out_size - 1] = '\0';
        return;
    }
    off += (size_t)n;

    Workspace *ws;
    bool first = true;
    wl_list_for_each(ws, &server->workspaces, link) {
        bool focused = (active && active->active_workspace == ws);
        bool empty = wl_list_empty(&ws->views);

        const char *output_name = "";
        if (ws->output && ws->output->wlr_output && ws->output->wlr_output->name[0]) {
            output_name = ws->output->wlr_output->name;
        }

        if (!first) {
            n = snprintf(out + off, out_size - off, ",");
            if (n < 0 || (size_t)n >= out_size - off) {
                out[out_size - 1] = '\0';
                return;
            }
            off += (size_t)n;
        }
        first = false;

        n = snprintf(out + off, out_size - off,
            "{\"id\":%d,\"focused\":%s,\"empty\":%s,\"output\":\"%s\"}",
            ws->id,
            focused ? "true" : "false",
            empty ? "true" : "false",
            output_name);

        if (n < 0 || (size_t)n >= out_size - off) {
            out[out_size - 1] = '\0';
            return;
        }
        off += (size_t)n;
    }

    n = snprintf(out + off, out_size - off, "]}");
    if (n < 0 || (size_t)n >= out_size - off) {
        out[out_size - 1] = '\0';
    }
}

static void ipc_process_command(IPCClient *client, char *line) {
    if (!client || !line) return;

    char *cmd = ipc_trim(line);
    if (!cmd || !*cmd) return;

    Server *server = client->server;
    if (!server) return;

    if (!strcasecmp(cmd, "workspaces")) {
        char buf[8192];
        ipc_build_workspaces(server, buf, sizeof(buf));
        ipc_write_all(client->fd, buf, strlen(buf));
        ipc_write_all(client->fd, "\n", 1);
        return;
    }

    if (!strncasecmp(cmd, "ws ", 3) || !strncasecmp(cmd, "workspace ", 10)) {
        char *num;
        if (!strncasecmp(cmd, "ws ", 3)) {
            num = cmd + 3;
        } else {
            num = cmd + 10;
        }

        int id = atoi(ipc_trim(num));
        if (id == 0) id = 10;

        if (id >= 1 && id <= 10) {
            Output *out = server->active_output;
            if (!out && server->focused_view && server->focused_view->output) {
                out = server->focused_view->output;
            }
            if (!out && !wl_list_empty(&server->outputs)) {
                out = wl_container_of(server->outputs.next, out, link);
            }

            if (out) {
                server_switch_workspace(server, out, id);
            }
        }

        char buf[8192];
        ipc_build_workspaces(server, buf, sizeof(buf));
        ipc_write_all(client->fd, buf, strlen(buf));
        ipc_write_all(client->fd, "\n", 1);
        return;
    }

    const char *err = "{\"error\":\"unknown command\"}\n";
    ipc_write_all(client->fd, err, strlen(err));
}

static int ipc_client_fd(int fd, uint32_t mask, void *data) {
    IPCClient *client = data;
    if (!client) return 0;

    if (mask & WL_EVENT_READABLE) {
        ssize_t n = read(fd, client->buf + client->len, sizeof(client->buf) - client->len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
            ipc_client_remove(client);
            return 0;
        }
        if (n == 0) {
            ipc_client_remove(client);
            return 0;
        }

        client->len += (size_t)n;

        for (;;) {
            char *nl = memchr(client->buf, '\n', client->len);
            if (!nl) break;

            size_t line_len = (size_t)(nl - client->buf);
            char line[sizeof(client->buf)];
            if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;

            memcpy(line, client->buf, line_len);
            line[line_len] = '\0';

            size_t consumed = (size_t)(nl - client->buf) + 1;
            memmove(client->buf, client->buf + consumed, client->len - consumed);
            client->len -= consumed;

            ipc_process_command(client, line);
        }

        if (client->len == sizeof(client->buf)) {
            client->len = 0;
        }
    }

    if (mask & (WL_EVENT_HANGUP | WL_EVENT_ERROR)) {
        ipc_client_remove(client);
    }

    return 0;
}

static int ipc_listen_fd(int fd, uint32_t mask, void *data) {
    IPCServer *ipc = data;
    if (!ipc) return 0;

    if (mask & WL_EVENT_READABLE) {
        int client_fd = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (client_fd < 0) return 0;

        IPCClient *client = calloc(1, sizeof(IPCClient));
        if (!client) {
            close(client_fd);
            return 0;
        }

        client->fd = client_fd;
        client->server = ipc->server;
        client->len = 0;
        wl_list_init(&client->link);
        wl_list_insert(&ipc->clients, &client->link);

        client->source = wl_event_loop_add_fd(
            ipc->loop, client_fd, WL_EVENT_READABLE, ipc_client_fd, client);

        if (!client->source) {
            ipc_client_remove(client);
        }
    }

    return 0;
}

int ipc_init(IPCServer *ipc, Server *compositor, struct wl_event_loop *loop, const char *path) {
    if (!ipc || !loop || !path || !*path) return -1;

    memset(ipc, 0, sizeof(*ipc));
    ipc->socket_fd = -1;
    ipc->loop = loop;
    ipc->server = compositor;
    wl_list_init(&ipc->clients);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    unlink(path);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return -1;
    }

    chmod(path, 0600);

    if (listen(fd, 8) < 0) {
        close(fd);
        return -1;
    }

    char *copy = strdup(path);
    if (!copy) {
        close(fd);
        return -1;
    }

    ipc->socket_fd = fd;
    ipc->socket_path = copy;

    ipc->listen_source = wl_event_loop_add_fd(
        loop, fd, WL_EVENT_READABLE, ipc_listen_fd, ipc);

    if (!ipc->listen_source) {
        close(fd);
        free(copy);
        ipc->socket_fd = -1;
        ipc->socket_path = NULL;
        return -1;
    }

    return 0;
}

void ipc_destroy(IPCServer *ipc) {
    if (!ipc) return;

    if (ipc->listen_source) {
        wl_event_source_remove(ipc->listen_source);
        ipc->listen_source = NULL;
    }

    IPCClient *client, *tmp;
    wl_list_for_each_safe(client, tmp, &ipc->clients, link) {
        ipc_client_remove(client);
    }
    wl_list_init(&ipc->clients);

    if (ipc->socket_fd >= 0) {
        close(ipc->socket_fd);
        ipc->socket_fd = -1;
    }

    if (ipc->socket_path) {
        unlink(ipc->socket_path);
        free(ipc->socket_path);
        ipc->socket_path = NULL;
    }
}
