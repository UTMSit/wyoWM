#ifndef WYO_SERVER_NETBSD_H
#define WYO_SERVER_NETBSD_H

#include <stdbool.h>
#include "server.h"

bool server_netbsd_reload_init(Server *server);
void server_netbsd_reload_destroy(Server *server);

#endif
