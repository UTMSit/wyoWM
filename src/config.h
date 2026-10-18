#ifndef CONFIG_H
#define CONFIG_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <wayland-server-core.h>
#include <xkbcommon/xkbcommon.h>
#include "animation.h"

#define MAX_BEZIERS 32
#define MAX_ANIM_RULES 16
#define MAX_BLUR_RULES 32

typedef enum {
    BLUR_MATCH_APP_ID,
    BLUR_MATCH_CLASS,
    BLUR_MATCH_INSTANCE,
    BLUR_MATCH_NAMESPACE,
    BLUR_MATCH_TITLE
} BlurMatchType;

typedef struct {
    BlurMatchType type;
    char pattern[128];
    int radius;
    double strength;
} ConfigBlurRule;

typedef struct {
	char name[64];
	BezierCurve curve;
} ConfigBezierEntry;

typedef struct {
	char type[64];
	bool enabled;
	int duration_ms;
	char curve_name[64];
} ConfigAnimRule;

typedef enum {
    ACTION_NONE,
    ACTION_EXEC,
    ACTION_CLOSE,
    ACTION_QUIT,
    ACTION_TOGGLE_FLOATING,
    ACTION_TOGGLE_FULLSCREEN,
    ACTION_CENTER,
    ACTION_FOCUS_DIRECTION,
    ACTION_MOVE_DIRECTION,
    ACTION_MOVE_OUTPUT_DIRECTION,
    ACTION_RESIZE_DIRECTION,
    ACTION_SWITCH_VT,
    ACTION_WORKSPACE
} ConfigAction;

typedef struct ConfigKeybind {
    uint32_t modifiers;
    uint32_t keycode;
    xkb_keysym_t keysym;
    ConfigAction action;
    char *command;
    int arg;
    struct wl_list link;
} ConfigKeybind;

typedef enum {
    WALLPAPER_MODE_FILL,
    WALLPAPER_MODE_FIT,
    WALLPAPER_MODE_STRETCH,
    WALLPAPER_MODE_CENTER,
    WALLPAPER_MODE_TILE
} WallpaperMode;

typedef struct ConfigExecOnce {
	char *command;
	struct wl_list link;
} ConfigExecOnce;

typedef struct Config {
	int border_width;
	float border_color_focused[4];
	float border_color_unfocused[4];
	float background_color[4];
	int gaps_in;
	int gaps_out;
	double corner_radius;
	double active_opacity;
	double inactive_opacity;
	int animation_duration_ms;
	int anim_window_duration_ms;
	int anim_window_curve;
	int anim_opacity_duration_ms;
	int anim_opacity_curve;
	char wallpaper_path[1024];
    WallpaperMode wallpaper_mode;
    char kb_rules[64];
    char kb_model[64];
    char kb_layouts[256];
    char kb_variant[128];
    char kb_options[256];
    char sticky_apps[1024];
    char cursor_theme[128];
    int cursor_size;
    ConfigBlurRule blur_rules[MAX_BLUR_RULES];
    int blur_rule_count;
    struct wl_list keybinds;
	struct wl_list exec_once;
	bool has_keybinds;
	ConfigBezierEntry beziers[MAX_BEZIERS];
	int bezier_count;
	ConfigAnimRule anim_rules[MAX_ANIM_RULES];
	int anim_rule_count;
} Config;

void config_init_defaults(Config *config);
bool config_load_file(Config *config, const char *path);
void config_load_default_keybinds(Config *config);
void config_free_keybinds(Config *config);
void config_destroy(Config *config);
bool config_default_path(char *out, size_t out_size);
void config_load_workspace_keybinds(Config *config);

#endif
