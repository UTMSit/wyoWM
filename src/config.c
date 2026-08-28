#define _GNU_SOURCE
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <linux/input-event-codes.h>
#include <wlr/types/wlr_keyboard.h>

typedef struct KeyNameEntry {
    const char *name;
    uint32_t keycode;
} KeyNameEntry;

static const KeyNameEntry key_name_table[] = {
    { "LEFT", KEY_LEFT },
    { "RIGHT", KEY_RIGHT },
    { "UP", KEY_UP },
    { "DOWN", KEY_DOWN },
    { "ESC", KEY_ESC },
    { "ESCAPE", KEY_ESC },
    { "ENTER", KEY_ENTER },
    { "RETURN", KEY_ENTER },
    { "SPACE", KEY_SPACE },
    { "TAB", KEY_TAB },
    { "BACKSPACE", KEY_BACKSPACE },
    { "DELETE", KEY_DELETE },
    { "HOME", KEY_HOME },
    { "END", KEY_END },
    { "PAGEUP", KEY_PAGEUP },
    { "PAGEDOWN", KEY_PAGEDOWN },
    { "F1", KEY_F1 },
    { "F2", KEY_F2 },
    { "F3", KEY_F3 },
    { "F4", KEY_F4 },
    { "F5", KEY_F5 },
    { "F6", KEY_F6 },
    { "F7", KEY_F7 },
    { "F8", KEY_F8 },
    { "F9", KEY_F9 },
    { "F10", KEY_F10 },
    { "F11", KEY_F11 },
    { "F12", KEY_F12 },
    { "PRINT", KEY_SYSRQ },
    { "PRINTSCREEN", KEY_SYSRQ },
    { "SYSRQ", KEY_SYSRQ },
};

static uint32_t keycode_from_name(const char *name) {
    if (!name || !*name) {
        return 0;
    }

    size_t len = strlen(name);

    if (len == 1) {
        return 0;
    }

    for (size_t i = 0; i < sizeof(key_name_table) / sizeof(key_name_table[0]); i++) {
        if (!strcasecmp(name, key_name_table[i].name)) {
            return key_name_table[i].keycode;
        }
    }

    return 0;
}

static char *trim(char *s) {
    while (isspace((unsigned char)*s)) {
        s++;
    }

    if (*s == '\0') {
        return s;
    }

    char *end = s + strlen(s) - 1;
    while (end > s && isspace((unsigned char)*end)) {
        *end = '\0';
        end--;
    }

    return s;
}

static bool parse_color(const char *text, float out[4]) {
    if (!text || !out) {
        return false;
    }

    while (*text == '#' || *text == ' ') {
        text++;
    }

    if (strncasecmp(text, "0x", 2) == 0) {
        text += 2;
    }

    size_t len = strlen(text);
    if (len != 6 && len != 8) {
        return false;
    }

    unsigned long v = strtoul(text, NULL, 16);

    if (len == 6) {
        out[0] = (float)((v >> 16) & 255) / 255.0f;
        out[1] = (float)((v >> 8) & 255) / 255.0f;
        out[2] = (float)(v & 255) / 255.0f;
        out[3] = 1.0f;
    } else {
        out[0] = (float)((v >> 24) & 255) / 255.0f;
        out[1] = (float)((v >> 16) & 255) / 255.0f;
        out[2] = (float)((v >> 8) & 255) / 255.0f;
        out[3] = (float)(v & 255) / 255.0f;
    }

    return true;
}

static bool parse_direction(const char *s, int *arg) {
    if (!s || !arg) {
        return false;
    }

    if (!strcasecmp(s, "left")) {
        *arg = 0;
        return true;
    }

    if (!strcasecmp(s, "down")) {
        *arg = 1;
        return true;
    }

    if (!strcasecmp(s, "up")) {
        *arg = 2;
        return true;
    }

    if (!strcasecmp(s, "right")) {
        *arg = 3;
        return true;
    }

    return false;
}

static void add_bind(Config *config, uint32_t modifiers, uint32_t keycode,
                     xkb_keysym_t keysym, ConfigAction action,
                     const char *command, int arg) {
    ConfigKeybind *bind = calloc(1, sizeof(ConfigKeybind));
    if (!bind) {
        return;
    }

    bind->modifiers = modifiers;
    bind->keycode = keycode;
    bind->keysym = keysym;
    bind->action = action;
    bind->command = command ? strdup(command) : NULL;
    bind->arg = arg;

    wl_list_insert(&config->keybinds, &bind->link);
    config->has_keybinds = true;
}

static void set_wallpaper_path(Config *config, const char *value) {
    if (!value || !*value) {
        config->wallpaper_path[0] = '\0';
        return;
    }

    if (value[0] == '~' && value[1] == '/') {
        const char *home = getenv("HOME");
        snprintf(config->wallpaper_path, sizeof(config->wallpaper_path), "%s%s",
                 home ? home : "", value + 1);
    } else {
        snprintf(config->wallpaper_path, sizeof(config->wallpaper_path), "%s", value);
    }
}

static double clamp_opacity(double v) {
    if (v < 0.0) return 0.0;
    if (v > 1.0) return 1.0;
    return v;
}

static void parse_general_line(Config *config, const char *key, const char *value) {
    if (!strcasecmp(key, "border_width")) {
        config->border_width = atoi(value);
    } else if (!strcasecmp(key, "border_color_focused")) {
        parse_color(value, config->border_color_focused);
    } else if (!strcasecmp(key, "border_color_unfocused")) {
        parse_color(value, config->border_color_unfocused);
    } else if (!strcasecmp(key, "background_color")) {
        parse_color(value, config->background_color);
    } else if (!strcasecmp(key, "gaps_in")) {
        config->gaps_in = atoi(value);
    } else if (!strcasecmp(key, "gaps_out")) {
        config->gaps_out = atoi(value);
    } else if (!strcasecmp(key, "corner_radius")) {
        config->corner_radius = atof(value);
    } else if (!strcasecmp(key, "active_opacity")) {
        config->active_opacity = clamp_opacity(atof(value));
    } else if (!strcasecmp(key, "inactive_opacity")) {
        config->inactive_opacity = clamp_opacity(atof(value));
    } else if (!strcasecmp(key, "animation_duration_ms")) {
        int duration = atoi(value);
        if (duration < 0) duration = 0;
        config->animation_duration_ms = duration;
    } else if (!strcasecmp(key, "wallpaper")) {
        set_wallpaper_path(config, value);
    } else if (!strcasecmp(key, "wallpaper_mode")) {
        if (!strcasecmp(value, "fill")) {
            config->wallpaper_mode = WALLPAPER_MODE_FILL;
        } else if (!strcasecmp(value, "fit")) {
            config->wallpaper_mode = WALLPAPER_MODE_FIT;
        } else if (!strcasecmp(value, "stretch")) {
            config->wallpaper_mode = WALLPAPER_MODE_STRETCH;
        } else if (!strcasecmp(value, "center")) {
            config->wallpaper_mode = WALLPAPER_MODE_CENTER;
        } else if (!strcasecmp(value, "tile")) {
            config->wallpaper_mode = WALLPAPER_MODE_TILE;
        }
    }
}

static void parse_config_line(Config *config, const char *line) {
	char *copy = strdup(line);
	if (!copy) return;

	char *eq = strchr(copy, '=');
	if (!eq) {
		free(copy);
		return;
	}

	*eq = '\0';
	char *left = trim(copy);
	char *right = trim(eq + 1);

	if (!*left || !*right) {
		free(copy);
		return;
	}

	if (!strncasecmp(left, "bind.", 5)) {
		char *keys = left + 5;
		char *keys_copy = strdup(keys);
		if (!keys_copy) {
			free(copy);
			return;
		}

		uint32_t modifiers = 0;
		char *saveptr = NULL;
		char *token = strtok_r(keys_copy, ".", &saveptr);
		char *key_name = NULL;

		while (token) {
			char *t = trim(token);
			if (!*t) {
				token = strtok_r(NULL, ".", &saveptr);
				continue;
			}
			if (!strcasecmp(t, "SUPER") || !strcasecmp(t, "LOGO") || !strcasecmp(t, "MOD4")) {
				modifiers |= WLR_MODIFIER_LOGO;
			} else if (!strcasecmp(t, "SHIFT")) {
				modifiers |= WLR_MODIFIER_SHIFT;
			} else if (!strcasecmp(t, "CTRL") || !strcasecmp(t, "CONTROL")) {
				modifiers |= WLR_MODIFIER_CTRL;
			} else if (!strcasecmp(t, "ALT")) {
				modifiers |= WLR_MODIFIER_ALT;
			} else {
				key_name = t;
			}
			token = strtok_r(NULL, ".", &saveptr);
		}

		if (key_name) {
			uint32_t keycode = keycode_from_name(key_name);
			xkb_keysym_t keysym = xkb_keysym_from_name(key_name, XKB_KEYSYM_CASE_INSENSITIVE);
			if (keysym != XKB_KEY_NoSymbol) {
				keysym = xkb_keysym_to_lower(keysym);
			}
			if (keycode != 0 || keysym != XKB_KEY_NoSymbol) {
				ConfigAction action = ACTION_NONE;
				const char *command = NULL;
				int arg = 0;

				if (!strncasecmp(right, "exec", 4)) {
					char *cmd = trim(right + 4);
					if (*cmd) {
						action = ACTION_EXEC;
						command = cmd;
					}
				} else if (!strcasecmp(right, "close")) {
					action = ACTION_CLOSE;
				} else if (!strcasecmp(right, "quit")) {
					action = ACTION_QUIT;
				} else if (!strcasecmp(right, "togglefloating")) {
					action = ACTION_TOGGLE_FLOATING;
				} else if (!strcasecmp(right, "fullscreen")) {
					action = ACTION_TOGGLE_FULLSCREEN;
				} else if (!strcasecmp(right, "center")) {
					action = ACTION_CENTER;
				} else if (!strncasecmp(right, "moveoutput", 10)) {
					char *dir = trim(right + 10);
					if (parse_direction(dir, &arg)) action = ACTION_MOVE_OUTPUT_DIRECTION;
				} else if (!strncasecmp(right, "move", 4)) {
					char *dir = trim(right + 4);
					if (parse_direction(dir, &arg)) action = ACTION_MOVE_DIRECTION;
				} else if (!strncasecmp(right, "focus", 5)) {
					char *dir = trim(right + 5);
					if (parse_direction(dir, &arg)) action = ACTION_FOCUS_DIRECTION;
				} else if (!strncasecmp(right, "resize", 6)) {
					char *dir = trim(right + 6);
					if (parse_direction(dir, &arg)) action = ACTION_RESIZE_DIRECTION;
				} else if (!strncasecmp(right, "vt", 2)) {
					char *num = trim(right + 2);
					action = ACTION_SWITCH_VT;
					arg = atoi(num);
				}

				if (action != ACTION_NONE) {
					add_bind(config, modifiers, keycode, keysym, action, command, arg);
				}
			}
		}
		free(keys_copy);
	} else if (!strncasecmp(left, "general.", 8)) {
		char *key = left + 8;
		parse_general_line(config, key, right);
	} else if (!strcasecmp(left, "exec-once") || !strcasecmp(left, "exec_once")) {
		ConfigExecOnce *exec = calloc(1, sizeof(ConfigExecOnce));
		if (exec) {
			exec->command = strdup(right);
			wl_list_insert(&config->exec_once, &exec->link);
		}
	}

	free(copy);
}

void config_init_defaults(Config *config) {
	memset(config, 0, sizeof(Config));
	wl_list_init(&config->keybinds);
	wl_list_init(&config->exec_once);

    config->border_width = 2;
    config->gaps_in = 4;
    config->gaps_out = 8;
    config->corner_radius = 0.0;
    config->active_opacity = 1.0;
    config->inactive_opacity = 1.0;
    config->animation_duration_ms = 160;
    config->wallpaper_mode = WALLPAPER_MODE_FILL;
    config->has_keybinds = false;

    config->border_color_focused[0] = 0.4f;
    config->border_color_focused[1] = 0.6f;
    config->border_color_focused[2] = 1.0f;
    config->border_color_focused[3] = 1.0f;

    config->border_color_unfocused[0] = 0.3f;
    config->border_color_unfocused[1] = 0.3f;
    config->border_color_unfocused[2] = 0.35f;
    config->border_color_unfocused[3] = 1.0f;

    config->background_color[0] = 0.04f;
    config->background_color[1] = 0.05f;
    config->background_color[2] = 0.07f;
    config->background_color[3] = 1.0f;
}

bool config_load_file(Config *config, const char *path) {
	FILE *fp = fopen(path, "r");
	if (!fp) {
		return false;
	}

	char *line = NULL;
	size_t line_capacity = 0;
	while (getline(&line, &line_capacity, fp) >= 0) {
		char *s = trim(line);
		if (!*s || *s == '#') {
			continue;
		}
		parse_config_line(config, s);
	}
	free(line);
	fclose(fp);
	return true;
}

void config_load_default_keybinds(Config *config) {
    if (config->has_keybinds) {
        return;
    }

    add_bind(config, WLR_MODIFIER_LOGO, KEY_Q, XKB_KEY_NoSymbol, ACTION_EXEC, "kitty", 0);
    add_bind(config, WLR_MODIFIER_LOGO, KEY_E, XKB_KEY_NoSymbol, ACTION_EXEC, "nautilus", 0);
    add_bind(config, WLR_MODIFIER_LOGO, KEY_H, XKB_KEY_NoSymbol, ACTION_EXEC, "firefox", 0);
    add_bind(config, WLR_MODIFIER_LOGO, KEY_C, XKB_KEY_NoSymbol, ACTION_CLOSE, NULL, 0);
    add_bind(config, WLR_MODIFIER_LOGO, KEY_V, XKB_KEY_NoSymbol, ACTION_TOGGLE_FLOATING, NULL, 0);
    add_bind(config, WLR_MODIFIER_LOGO, KEY_F, XKB_KEY_NoSymbol, ACTION_TOGGLE_FULLSCREEN, NULL, 0);
    add_bind(config, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, KEY_C, XKB_KEY_NoSymbol, ACTION_CENTER, NULL, 0);
    add_bind(config, WLR_MODIFIER_LOGO, KEY_ESC, XKB_KEY_NoSymbol, ACTION_QUIT, NULL, 0);

    add_bind(config, WLR_MODIFIER_LOGO, KEY_LEFT, XKB_KEY_NoSymbol, ACTION_FOCUS_DIRECTION, NULL, 0);
    add_bind(config, WLR_MODIFIER_LOGO, KEY_DOWN, XKB_KEY_NoSymbol, ACTION_FOCUS_DIRECTION, NULL, 1);
    add_bind(config, WLR_MODIFIER_LOGO, KEY_UP, XKB_KEY_NoSymbol, ACTION_FOCUS_DIRECTION, NULL, 2);
    add_bind(config, WLR_MODIFIER_LOGO, KEY_RIGHT, XKB_KEY_NoSymbol, ACTION_FOCUS_DIRECTION, NULL, 3);

    add_bind(config, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, KEY_H, XKB_KEY_NoSymbol, ACTION_MOVE_DIRECTION, NULL, 0);
    add_bind(config, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, KEY_J, XKB_KEY_NoSymbol, ACTION_MOVE_DIRECTION, NULL, 1);
    add_bind(config, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, KEY_K, XKB_KEY_NoSymbol, ACTION_MOVE_DIRECTION, NULL, 2);
    add_bind(config, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, KEY_L, XKB_KEY_NoSymbol, ACTION_MOVE_DIRECTION, NULL, 3);

    add_bind(config, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, KEY_LEFT, XKB_KEY_NoSymbol, ACTION_MOVE_OUTPUT_DIRECTION, NULL, 0);
    add_bind(config, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, KEY_DOWN, XKB_KEY_NoSymbol, ACTION_MOVE_OUTPUT_DIRECTION, NULL, 1);
    add_bind(config, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, KEY_UP, XKB_KEY_NoSymbol, ACTION_MOVE_OUTPUT_DIRECTION, NULL, 2);
    add_bind(config, WLR_MODIFIER_LOGO | WLR_MODIFIER_SHIFT, KEY_RIGHT, XKB_KEY_NoSymbol, ACTION_MOVE_OUTPUT_DIRECTION, NULL, 3);

    add_bind(config, WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL, KEY_H, XKB_KEY_NoSymbol, ACTION_RESIZE_DIRECTION, NULL, 0);
    add_bind(config, WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL, KEY_J, XKB_KEY_NoSymbol, ACTION_RESIZE_DIRECTION, NULL, 1);
    add_bind(config, WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL, KEY_K, XKB_KEY_NoSymbol, ACTION_RESIZE_DIRECTION, NULL, 2);
    add_bind(config, WLR_MODIFIER_LOGO | WLR_MODIFIER_CTRL, KEY_L, XKB_KEY_NoSymbol, ACTION_RESIZE_DIRECTION, NULL, 3);

    add_bind(config, WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, KEY_F1, XKB_KEY_NoSymbol, ACTION_SWITCH_VT, NULL, 1);
    add_bind(config, WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, KEY_F2, XKB_KEY_NoSymbol, ACTION_SWITCH_VT, NULL, 2);
    add_bind(config, WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, KEY_F3, XKB_KEY_NoSymbol, ACTION_SWITCH_VT, NULL, 3);
    add_bind(config, WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, KEY_F4, XKB_KEY_NoSymbol, ACTION_SWITCH_VT, NULL, 4);
    add_bind(config, WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, KEY_F5, XKB_KEY_NoSymbol, ACTION_SWITCH_VT, NULL, 5);
    add_bind(config, WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, KEY_F6, XKB_KEY_NoSymbol, ACTION_SWITCH_VT, NULL, 6);
    add_bind(config, WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, KEY_F7, XKB_KEY_NoSymbol, ACTION_SWITCH_VT, NULL, 7);
    add_bind(config, WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, KEY_F8, XKB_KEY_NoSymbol, ACTION_SWITCH_VT, NULL, 8);
    add_bind(config, WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, KEY_F9, XKB_KEY_NoSymbol, ACTION_SWITCH_VT, NULL, 9);
    add_bind(config, WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, KEY_F10, XKB_KEY_NoSymbol, ACTION_SWITCH_VT, NULL, 10);
    add_bind(config, WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, KEY_F11, XKB_KEY_NoSymbol, ACTION_SWITCH_VT, NULL, 11);
    add_bind(config, WLR_MODIFIER_CTRL | WLR_MODIFIER_ALT, KEY_F12, XKB_KEY_NoSymbol, ACTION_SWITCH_VT, NULL, 12);
}

void config_free_keybinds(Config *config) {
    ConfigKeybind *bind;
    ConfigKeybind *tmp;

    wl_list_for_each_safe(bind, tmp, &config->keybinds, link) {
        wl_list_remove(&bind->link);
        free(bind->command);
        free(bind);
    }

    wl_list_init(&config->keybinds);
    config->has_keybinds = false;
}

void config_destroy(Config *config) {
	config_free_keybinds(config);
	ConfigExecOnce *exec, *tmp_exec;
	wl_list_for_each_safe(exec, tmp_exec, &config->exec_once, link) {
		wl_list_remove(&exec->link);
		free(exec->command);
		free(exec);
	}
	wl_list_init(&config->exec_once);
}

bool config_default_path(char *out, size_t out_size) {
    const char *xdg = getenv("XDG_CONFIG_HOME");

    if (xdg && *xdg) {
        snprintf(out, out_size, "%s/wyoWM/wyowm.conf", xdg);
        return true;
    }

    const char *home = getenv("HOME");
    if (!home || !*home) {
        return false;
    }

    snprintf(out, out_size, "%s/.config/wyoWM/wyowm.conf", home);
    return true;
}
