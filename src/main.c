#include "server.h"
#include <signal.h>

int main(void) {
    sigset_t mask;
    (void)sigemptyset(&mask);
    (void)sigaddset(&mask, SIGUSR1);
    (void)sigprocmask(SIG_BLOCK, &mask, NULL);
    (void)signal(SIGPIPE, SIG_IGN);

    Server server;
    if (!server_init(&server)) {
        return 1;
    }
    server_run(&server);
    server_destroy(&server);
    return 0;
}
