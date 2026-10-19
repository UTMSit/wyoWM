#define _GNU_SOURCE
#include "config.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <linux/input-event-codes.h>
#include <wlr/types/wlr_keyboard.h>
#include <sys/stat.h>
#include <sys/types.h>

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
        unsigned char c = (unsigned char)name[0];
        if (isdigit(c)) {
            if (c == '0') {
                return KEY_0;
            }
            return KEY_1 + (uint32_t)(c - '1');
        }
        char lower = (char)tolower(c);
        if (lower >= 'a' && lower <= 'z') {
            static const uint32_t letter_codes[26] = {
                KEY_A, KEY_B, KEY_C, KEY_D, KEY_E, KEY_F, KEY_G,
                KEY_H, KEY_I, KEY_J, KEY_K, KEY_L, KEY_M, KEY_N,
                KEY_O, KEY_P, KEY_Q, KEY_R, KEY_S, KEY_T, KEY_U,
                KEY_V, KEY_W, KEY_X, KEY_Y, KEY_Z
            };
            return letter_codes[lower - 'a'];
        }
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

static void parse_output_line(Config *config, const char *value) {
    if (!value || !*value) return;
    if (config->output_rule_count >= MAX_OUTPUT_RULES) return;

    char *copy = strdup(value);
    if (!copy) return;

    char *name = NULL;
    char *mode = NULL;
    char *pos = NULL;

    char *comma1 = strchr(copy, ',');
    if (!comma1) {
        free(copy);
        return;
    }
    *comma1 = '\0';
    name = copy;

    char *comma2 = strchr(comma1 + 1, ',');
    if (comma2) {
        *comma2 = '\0';
        mode = comma1 + 1;
        pos = comma2 + 1;
    } else {
        mode = comma1 + 1;
    }

    name = trim(name);
    mode = trim(mode);
    if (pos) pos = trim(pos);

    if (!*name || !*mode) {
        free(copy);
        return;
    }

    ConfigOutputRule *rule = &config->output_rules[config->output_rule_count];
    memset(rule, 0, sizeof(*rule));
    snprintf(rule->name, sizeof(rule->name), "%s", name);

    if (!strcasecmp(mode, "disable")) {
        rule->enabled = false;
        rule->use_preferred = false;
        rule->mode_width = 0;
        rule->mode_height = 0;
        rule->mode_refresh = 0;
    } else if (!strcasecmp(mode, "preferable") || !strcasecmp(mode, "preferred")) {
        rule->enabled = true;
        rule->use_preferred = true;
        rule->mode_width = 0;
        rule->mode_height = 0;
        rule->mode_refresh = 0;
    } else {
        rule->enabled = true;
        rule->use_preferred = false;
        int w = 0, h = 0, hz = 0;
        char *at = strchr(mode, '@');
        if (at) {
            *at = '\0';
            hz = atoi(at + 1);
        }
        char *x_char = strchr(mode, 'x');
        if (!x_char) x_char = strchr(mode, 'X');
        if (x_char) {
            w = atoi(mode);
            h = atoi(x_char + 1);
        }
        rule->mode_width = w;
        rule->mode_height = h;
        rule->mode_refresh = hz > 0 ? hz * 1000 : 0;
    }

    if (pos && *pos) {
        int x = 0, y = 0;
        char *x_char = strchr(pos, 'x');
        if (!x_char) x_char = strchr(pos, 'X');
        if (x_char) {
            x = atoi(pos);
            y = atoi(x_char + 1);
        }
        rule->x = x;
        rule->y = y;
    }

    config->output_rule_count++;
    free(copy);
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
    } else if (!strcasecmp(key, "anim_window_duration_ms")) {
        int duration = atoi(value);
        if (duration < 0) duration = 0;
        config->anim_window_duration_ms = duration;
    } else if (!strcasecmp(key, "anim_window_curve")) {
        if (!strcasecmp(value, "linear")) config->anim_window_curve = 0;
        else if (!strcasecmp(value, "ease_in")) config->anim_window_curve = 1;
        else if (!strcasecmp(value, "ease_out")) config->anim_window_curve = 2;
        else if (!strcasecmp(value, "ease_in_out")) config->anim_window_curve = 3;
    } else if (!strcasecmp(key, "anim_opacity_duration_ms")) {
        int duration = atoi(value);
        if (duration < 0) duration = 0;
        config->anim_opacity_duration_ms = duration;
    } else if (!strcasecmp(key, "anim_opacity_curve")) {
        if (!strcasecmp(value, "linear")) config->anim_opacity_curve = 0;
        else if (!strcasecmp(value, "ease_in")) config->anim_opacity_curve = 1;
        else if (!strcasecmp(value, "ease_out")) config->anim_opacity_curve = 2;
        else if (!strcasecmp(value, "ease_in_out")) config->anim_opacity_curve = 3;
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
    } else if (!strcasecmp(key, "kb_layouts") || !strcasecmp(key, "keyboard_layouts")) {
        snprintf(config->kb_layouts, sizeof(config->kb_layouts), "%s", value);
    } else if (!strcasecmp(key, "kb_variant") || !strcasecmp(key, "keyboard_variant")) {
        snprintf(config->kb_variant, sizeof(config->kb_variant), "%s", value);
    } else if (!strcasecmp(key, "kb_options") || !strcasecmp(key, "keyboard_options")) {
        snprintf(config->kb_options, sizeof(config->kb_options), "%s", value);
    } else if (!strcasecmp(key, "kb_model") || !strcasecmp(key, "keyboard_model")) {
        snprintf(config->kb_model, sizeof(config->kb_model), "%s", value);
    } else if (!strcasecmp(key, "kb_rules") || !strcasecmp(key, "keyboard_rules")) {
        snprintf(config->kb_rules, sizeof(config->kb_rules), "%s", value);
    } else if (!strcasecmp(key, "sticky")) {
        snprintf(config->sticky_apps, sizeof(config->sticky_apps), "%s", value);
    } else if (!strcasecmp(key, "cursor_theme")) {
        snprintf(config->cursor_theme, sizeof(config->cursor_theme), "%s", value);
    } else if (!strcasecmp(key, "cursor_size")) {
        int size = atoi(value);
        if (size < 1) size = 1;
        if (size > 256) size = 256;
        config->cursor_size = size;
    } else if (!strcasecmp(key, "output")) {
        parse_output_line(config, value);
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
                if (!strncasecmp(right, "exec", 4) &&
                    (right[4] == '\0' || isspace((unsigned char)right[4]))) {
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
                } else if (!strncasecmp(right, "vt", 2) &&
                           (right[2] == '\0' ||
                            isspace((unsigned char)right[2]) ||
                            isdigit((unsigned char)right[2]))) {
                    char *num = trim(right + 2);
                    char *end = NULL;
                    long vt = strtol(num, &end, 10);
                    if (end != num && *end == '\0' && vt >= 1 && vt <= 63) {
                        action = ACTION_SWITCH_VT;
                        arg = (int)vt;
                    }
                } else if (!strncasecmp(right, "workspace", 9) &&
                           (right[9] == '\0' ||
                            isspace((unsigned char)right[9]) ||
                            isdigit((unsigned char)right[9]))) {
                    char *num = trim(right + 9);
                    char *end = NULL;
                    long id = strtol(num, &end, 10);
                    if (end != num && *end == '\0') {
                        if (id == 0) {
                            id = 10;
                        }
                        if (id >= 1 && id <= 10) {
                            action = ACTION_WORKSPACE;
                            arg = (int)id;
                        }
                    }
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
    } else if (!strncasecmp(left, "bezier", 6) && left[6] == '\0') {
        char *eq2 = strchr(right, ',');
        if (eq2) {
            *eq2 = '\0';
            char *name = trim(right);
            char *vals = eq2 + 1;
            double p1x, p1y, p2x, p2y;
            if (sscanf(vals, "%lf,%lf,%lf,%lf", &p1x, &p1y, &p2x, &p2y) == 4) {
                if (config->bezier_count < MAX_BEZIERS) {
                    snprintf(config->beziers[config->bezier_count].name, 64, "%s", name);
                    config->beziers[config->bezier_count].curve.p1x = p1x;
                    config->beziers[config->bezier_count].curve.p1y = p1y;
                    config->beziers[config->bezier_count].curve.p2x = p2x;
                    config->beziers[config->bezier_count].curve.p2y = p2y;
                    config->bezier_count++;
                }
            }
        }
    } else if (!strncasecmp(left, "blur", 4) && left[4] == '\0') {
        char rule[256] = {0};
        int radius = 6;
        double strength = 1.0;
        char *comma = strchr(right, ',');
        if (comma) {
            size_t n = (size_t)(comma - right);
            if (n >= sizeof(rule)) {
                n = sizeof(rule) - 1;
            }
            memcpy(rule, right, n);
            rule[n] = '\0';
            char *rest = comma + 1;
            char *comma2 = strchr(rest, ',');
            if (comma2) {
                *comma2 = '\0';
                radius = atoi(trim(rest));
                strength = atof(trim(comma2 + 1));
            } else {
                radius = atoi(trim(rest));
            }
        } else {
            snprintf(rule, sizeof(rule), "%s", trim(right));
        }
        char *pattern = trim(rule);
        BlurMatchType type = BLUR_MATCH_APP_ID;
        if (!strncasecmp(pattern, "app_id:", 7)) {
            type = BLUR_MATCH_APP_ID;
            pattern += 7;
        } else if (!strncasecmp(pattern, "class:", 6)) {
            type = BLUR_MATCH_CLASS;
            pattern += 6;
        } else if (!strncasecmp(pattern, "instance:", 9)) {
            type = BLUR_MATCH_INSTANCE;
            pattern += 9;
        } else if (!strncasecmp(pattern, "namespace:", 10)) {
            type = BLUR_MATCH_NAMESPACE;
            pattern += 10;
        } else if (!strncasecmp(pattern, "title:", 6)) {
            type = BLUR_MATCH_TITLE;
            pattern += 6;
        }
        while (*pattern == ' ' || *pattern == '\t') {
            pattern++;
        }
        if (*pattern && config->blur_rule_count < MAX_BLUR_RULES) {
            ConfigBlurRule *r = &config->blur_rules[config->blur_rule_count++];
            r->type = type;
            snprintf(r->pattern, sizeof(r->pattern), "%s", pattern);
            r->radius = radius;
            r->strength = strength;
        }
    } else if (!strncasecmp(left, "animation", 9) && left[9] == '\0') {
        char *eq2 = strchr(right, ',');
        if (eq2) {
            *eq2 = '\0';
            char *type = trim(right);
            char *vals = eq2 + 1;
            int enabled, duration;
            char curve_name[64] = {0};
            if (sscanf(vals, "%d,%d,%63s", &enabled, &duration, curve_name) >= 3) {
                if (config->anim_rule_count < MAX_ANIM_RULES) {
                    snprintf(config->anim_rules[config->anim_rule_count].type, 64, "%s", type);
                    config->anim_rules[config->anim_rule_count].enabled = enabled != 0;
                    config->anim_rules[config->anim_rule_count].duration_ms = duration;
                    snprintf(config->anim_rules[config->anim_rule_count].curve_name, 64, "%s", curve_name);
                    config->anim_rule_count++;
                }
            }
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
    config->animation_duration_ms = 190;
    config->anim_window_duration_ms = 160;
    config->anim_window_curve = 2;
    config->anim_opacity_duration_ms = 160;
    config->anim_opacity_curve = 2;
    config->wallpaper_mode = WALLPAPER_MODE_FILL;
    config->has_keybinds = false;
    snprintf(config->cursor_theme, sizeof(config->cursor_theme), "default");
    config->cursor_size = 24;
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
    snprintf(config->kb_rules, sizeof(config->kb_rules), "evdev");
    snprintf(config->kb_model, sizeof(config->kb_model), "pc105");
    snprintf(config->kb_layouts, sizeof(config->kb_layouts), "us");
    config->output_rule_count = 0;
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

static xkb_keysym_t digit_keysym(int digit) {
    if (digit < 0) digit = 0;
    if (digit > 9) digit = 9;
    return (xkb_keysym_t)(XKB_KEY_0 + digit);
}

static bool has_super_keybind(Config *config, uint32_t keycode, xkb_keysym_t keysym) {
    if (!config) return false;
    ConfigKeybind *bind;
    wl_list_for_each(bind, &config->keybinds, link) {
        if (bind->modifiers != WLR_MODIFIER_LOGO) {
            continue;
        }
        if (bind->keycode != 0 && bind->keycode == keycode) {
            return true;
        }
        if (bind->keysym != XKB_KEY_NoSymbol && bind->keysym == keysym) {
            return true;
        }
    }
    return false;
}

void config_load_workspace_keybinds(Config *config) {
    if (!config) return;
    for (int id = 1; id <= 10; id++) {
        int digit = (id == 10) ? 0 : id;
        uint32_t keycode;
        if (digit == 0) {
            keycode = KEY_0;
        } else {
            keycode = KEY_1 + (uint32_t)(digit - 1);
        }
        xkb_keysym_t keysym = digit_keysym(digit);
        if (has_super_keybind(config, keycode, keysym)) {
            continue;
        }
        add_bind(
            config,
            WLR_MODIFIER_LOGO,
            keycode,
            keysym,
            ACTION_WORKSPACE,
            NULL,
            id
        );
    }
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

bool config_generate_default(const char *path) {
    if (!path || !*path) return false;
    char *dir = strdup(path);
    if (!dir) return false;
    char *last_slash = strrchr(dir, '/');
    if (last_slash) {
        *last_slash = '\0';
        mkdir(dir, 0755);
    }
    free(dir);
    FILE *fp = fopen(path, "w");
    if (!fp) return false;
    fprintf(fp, "# wyoWM configuration file\n");
    fprintf(fp, "# Reload with: wyoconf-reload or SIGUSR1\n");
    fprintf(fp, "\n");
    fprintf(fp, "# General settings\n");
    fprintf(fp, "general.border_width = 2\n");
    fprintf(fp, "general.border_color_focused = 0x4d80ff\n");
    fprintf(fp, "general.border_color_unfocused = 0x333333\n");
    fprintf(fp, "general.background_color = 0x0a0d12\n");
    fprintf(fp, "general.gaps_in = 4\n");
    fprintf(fp, "general.gaps_out = 8\n");
    fprintf(fp, "general.corner_radius = 0\n");
    fprintf(fp, "general.active_opacity = 1.0\n");
    fprintf(fp, "general.inactive_opacity = 1.0\n");
    fprintf(fp, "\n");
    fprintf(fp, "# Wallpaper\n");
    fprintf(fp, "# general.wallpaper = ~/Pictures/wallpaper.png\n");
    fprintf(fp, "general.wallpaper_mode = fill\n");
    fprintf(fp, "\n");
    fprintf(fp, "# Keyboard layout\n");
    fprintf(fp, "# general.kb_layouts = us,ru\n");
    fprintf(fp, "# general.kb_options = grp:alt_shift_toggle\n");
    fprintf(fp, "\n");
    fprintf(fp, "# Cursor\n");
    fprintf(fp, "general.cursor_theme = default\n");
    fprintf(fp, "general.cursor_size = 24\n");
    fprintf(fp, "\n");
    fprintf(fp, "# Output configuration\n");
    fprintf(fp, "# Format: general.output = <name>,<mode>,<position>\n");
    fprintf(fp, "# <name>: output name (HDMI-A-1, DP-1, eDP-1)\n");
    fprintf(fp, "# <mode>: preferable, disable, or WIDTHxHEIGHT@REFRESH\n");
    fprintf(fp, "# <position>: XxY coordinates\n");
    fprintf(fp, "# general.output = HDMI-A-1,preferable,0x0\n");
    fprintf(fp, "# general.output = DP-1,2560x1440@144,1920x0\n");
    fprintf(fp, "# general.output = eDP-1,disable\n");
    fprintf(fp, "\n");
    fprintf(fp, "# Animation curves (cubic bezier)\n");
    fprintf(fp, "bezier = easeOutBack, 0.34, 1.56, 0.64, 1.0\n");
    fprintf(fp, "bezier = easeOutQuint, 0.22, 1.0, 0.36, 1.0\n");
    fprintf(fp, "\n");
    fprintf(fp, "# Animation rules\n");
    fprintf(fp, "# Format: animation = <type>, <enabled>, <duration_multiplier>, <curve>\n");
    fprintf(fp, "# Types: windows, fade\n");
    fprintf(fp, "# Duration multiplier: 1 = 100ms, 2 = 200ms, 4 = 400ms\n");
    fprintf(fp, "animation = windows, 1, 4, easeOutBack\n");
    fprintf(fp, "animation = fade, 1, 3, easeOutQuint\n");
    fprintf(fp, "\n");
    fprintf(fp, "# Blur rules\n");
    fprintf(fp, "# Format: blur = <pattern>, <radius>, <strength>\n");
    fprintf(fp, "# Pattern: app_id:<name>, class:<name>, title:<name>, namespace:<name>\n");
    fprintf(fp, "# blur = app_id:kitty, 6, 1.0\n");
    fprintf(fp, "\n");
    fprintf(fp, "# Keybinds\n");
    fprintf(fp, "# Modifiers: super, shift, ctrl, alt\n");
    fprintf(fp, "# Keys: letters, digits, F1-F12, arrows, ESC, ENTER, TAB, SPACE\n");
    fprintf(fp, "bind.super.q = exec kitty\n");
    fprintf(fp, "bind.super.e = exec nautilus\n");
    fprintf(fp, "bind.super.h = exec firefox\n");
    fprintf(fp, "bind.super.c = close\n");
    fprintf(fp, "bind.super.v = togglefloating\n");
    fprintf(fp, "bind.super.f = fullscreen\n");
    fprintf(fp, "bind.super.shift.c = center\n");
    fprintf(fp, "bind.super.esc = quit\n");
    fprintf(fp, "bind.super.left = focus left\n");
    fprintf(fp, "bind.super.down = focus down\n");
    fprintf(fp, "bind.super.up = focus up\n");
    fprintf(fp, "bind.super.right = focus right\n");
    fprintf(fp, "bind.super.shift.left = move left\n");
    fprintf(fp, "bind.super.shift.down = move down\n");
    fprintf(fp, "bind.super.shift.up = move up\n");
    fprintf(fp, "bind.super.shift.right = move right\n");
    fprintf(fp, "bind.super.ctrl.h = resize left\n");
    fprintf(fp, "bind.super.ctrl.j = resize down\n");
    fprintf(fp, "bind.super.ctrl.k = resize up\n");
    fprintf(fp, "bind.super.ctrl.l = resize right\n");
    fprintf(fp, "bind.super.1 = workspace 1\n");
    fprintf(fp, "bind.super.2 = workspace 2\n");
    fprintf(fp, "bind.super.3 = workspace 3\n");
    fprintf(fp, "bind.super.4 = workspace 4\n");
    fprintf(fp, "bind.super.5 = workspace 5\n");
    fprintf(fp, "bind.super.6 = workspace 6\n");
    fprintf(fp, "bind.super.7 = workspace 7\n");
    fprintf(fp, "bind.super.8 = workspace 8\n");
    fprintf(fp, "bind.super.9 = workspace 9\n");
    fprintf(fp, "bind.super.0 = workspace 10\n");
    fprintf(fp, "\n");
    fprintf(fp, "# Startup commands\n");
    fprintf(fp, "# exec-once = waybar\n");
    fprintf(fp, "# exec-once = mako\n");
    fclose(fp);
    return true;
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
