#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: wyoctl <command> [args...]\n");
        return 1;
    }

    const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!runtime_dir || !*runtime_dir) {
        runtime_dir = "/tmp";
    }

    char path[1024];
    snprintf(path, sizeof(path), "%s/wyowm.sock", runtime_dir);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect");
        close(fd);
        return 1;
    }

    char cmd[4096] = {0};
    size_t off = 0;

    for (int i = 1; i < argc; i++) {
        if (i > 1) {
            cmd[off++] = ' ';
        }
        size_t len = strlen(argv[i]);
        if (off + len >= sizeof(cmd) - 1) break;
        memcpy(cmd + off, argv[i], len);
        off += len;
    }
    cmd[off++] = '\n';

    ssize_t w = write(fd, cmd, off);
    if (w != (ssize_t)off) {
        perror("write");
        close(fd);
        return 1;
    }

    char buf[65536];
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
        fwrite(buf, 1, (size_t)n, stdout);
    }

    close(fd);
    return 0;
}
