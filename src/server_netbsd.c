#ifdef __NetBSD__

#include "server_netbsd.h"

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <wayland-server-core.h>

void server_reload_config(Server *server);

static volatile sig_atomic_t wyo_reload_write_fd = -1;

static void wyo_reload_signal_handler(int signo) {
    int fd = (int)wyo_reload_write_fd;
    unsigned char byte = (unsigned char)signo;

    if (fd >= 0) {
        ssize_t r = write(fd, &byte, 1);
        (void)r;
    }
}

static int wyo_reload_fd_set_flags(int fd, int get_cmd, int set_cmd, int flag) {
    int current = fcntl(fd, get_cmd);
    if (current == -1) {
        return -1;
    }

    return fcntl(fd, set_cmd, current | flag);
}

static int wyo_reload_event(int fd, uint32_t mask, void *data) {
    Server *server = data;
    unsigned char buf[128];

    (void)mask;

    for (;;) {
        ssize_t n = read(fd, buf, sizeof(buf));

        if (n > 0) {
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == (unsigned char)SIGUSR1) {
                    server_reload_config(server);
                }
            }
            continue;
        }

        if (n < 0 && errno == EINTR) {
            continue;
        }

        break;
    }

    return 0;
}

bool server_netbsd_reload_init(Server *server) {
    int fds[2];
    sigset_t mask;
    sigset_t old_mask;
    struct sigaction sa;

    if (pipe(fds) != 0) {
        return false;
    }

    if (wyo_reload_fd_set_flags(fds[0], F_GETFD, F_SETFD, FD_CLOEXEC) != 0 ||
        wyo_reload_fd_set_flags(fds[1], F_GETFD, F_SETFD, FD_CLOEXEC) != 0 ||
        wyo_reload_fd_set_flags(fds[0], F_GETFL, F_SETFL, O_NONBLOCK) != 0 ||
        wyo_reload_fd_set_flags(fds[1], F_GETFL, F_SETFL, O_NONBLOCK) != 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }

    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);

    if (sigprocmask(SIG_BLOCK, &mask, &old_mask) != 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = wyo_reload_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    if (sigaction(SIGUSR1, &sa, NULL) != 0) {
        sigprocmask(SIG_SETMASK, &old_mask, NULL);
        close(fds[0]);
        close(fds[1]);
        return false;
    }

    wyo_reload_write_fd = (sig_atomic_t)fds[1];

    if (sigprocmask(SIG_UNBLOCK, &mask, NULL) != 0) {
        wyo_reload_write_fd = -1;
        sigprocmask(SIG_SETMASK, &old_mask, NULL);
        close(fds[0]);
        close(fds[1]);
        return false;
    }

    server->reload_fd = fds[0];
    server->reload_source = wl_event_loop_add_fd(
        server->loop,
        fds[0],
        WL_EVENT_READABLE,
        wyo_reload_event,
        server
    );

    if (!server->reload_source) {
        wyo_reload_write_fd = -1;
        server->reload_fd = -1;
        close(fds[0]);
        close(fds[1]);
        sigprocmask(SIG_SETMASK, &old_mask, NULL);
        return false;
    }

    return true;
}

void server_netbsd_reload_destroy(Server *server) {
    sigset_t mask;
    sigset_t old_mask;
    bool blocked;
    int write_fd = -1;

    sigemptyset(&mask);
    sigaddset(&mask, SIGUSR1);
    blocked = sigprocmask(SIG_BLOCK, &mask, &old_mask) == 0;

    if (server->reload_source) {
        wl_event_source_remove(server->reload_source);
        server->reload_source = NULL;
    }

    if (wyo_reload_write_fd >= 0) {
        write_fd = (int)wyo_reload_write_fd;
        wyo_reload_write_fd = -1;
    }

    if (write_fd >= 0) {
        close(write_fd);
    }

    if (server->reload_fd >= 0) {
        close(server->reload_fd);
        server->reload_fd = -1;
    }

    if (blocked) {
        sigprocmask(SIG_SETMASK, &old_mask, NULL);
    }
}

#else

typedef int wyo_server_netbsd_unused;

#endif
