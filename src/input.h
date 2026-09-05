#ifndef INPUT_H
#define INPUT_H

#include <wayland-server.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>

struct Server;
struct View;

typedef struct Keyboard {
    struct Server *server;
    struct wlr_input_device *device;
    struct wlr_keyboard *wlr_keyboard;

    struct wl_listener key;
    struct wl_listener modifiers;
    struct wl_listener destroy;

    struct wl_list link;
} Keyboard;

void input_init(struct Server *server);
void input_destroy(struct Server *server);
void input_spawn_command(const char *command);
void input_reload_keymaps(struct Server *server);
void input_reload_cursor(struct Server *server);
#endif
