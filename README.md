# wyoWM

Wayland compositor based on wlroots 0.20. Uses dwindle tiling layout. Written in C.

## Features

- Dwindle tiling layout
- Directional focus, move, resize, and swap
- 10 workspaces with last-output memory
- Multi-monitor support with configurable output positions
- Floating and fullscreen windows
- Pointer drag: Super + left button moves, Super + right button resizes
- Borders, gaps, corner radius, and opacity
- Customizable animations with cubic bezier curves
- Wallpaper with fill, fit, stretch, and center modes
- Text config with hot reload
- XKB layout config for keyboard switching
- IPC socket and wyoctl tool
- Layer shell support for bars and menus
- XWayland support for legacy X11 applications
- Tearing control protocol for gaming
- Output management protocol (wlr-randr compatible)

## Protocols

xdg-shell, layer-shell, ext-workspace, foreign-toplevel, screencopy, viewporter, fractional-scale, xdg-activation, relative-pointer, pointer-gestures, presentation-time, xdg-decoration (server side), xdg-output, tearing-control, wlr-output-management.

## Requirements

- wlroots 0.20
- wayland-server
- xkbcommon
- libinput
- libdrm, gbm, pixman-1
- gdk-pixbuf-2.0
- UMK build system: https://github.com/user12msd4c/umk

## Build and install

```bash
umk update
```

Builds wyowm and wyoctl, installs binaries and session files.

Other targets:

```bash
umk build
umk install
umk clean
umk uninstall
```

## Installed files

- `/usr/local/bin/wyowm`
- `/usr/local/bin/wyoctl`
- `/usr/local/bin/wyowm-session`
- `/usr/local/bin/wyoconf-reload`
- `/usr/share/wayland-sessions/wyowm.desktop`

## Start

Select wyoWM in display manager or run `wyowm-session` from TTY.

## Configuration

Config file: `~/.config/wyoWM/wyowm.conf`

Compositor creates default config on first run if file does not exist. Reload without restart: `wyoconf-reload` or send SIGUSR1.

### Syntax

Lines use format: `key = value`

Comments start with `#`.

Whitespace around `=` is ignored.

### General settings

| Key | Type | Description |
|-----|------|-------------|
| `border_width` | integer | Border width in pixels |
| `border_color_focused` | hex | Focused window border color (0xRRGGBB or 0xRRGGBBAA) |
| `border_color_unfocused` | hex | Unfocused window border color |
| `background_color` | hex | Desktop background color |
| `gaps_in` | integer | Gaps between tiled windows |
| `gaps_out` | integer | Gaps around screen edges |
| `corner_radius` | number | Window corner radius |
| `active_opacity` | float | Opacity for focused windows (0.0 to 1.0) |
| `inactive_opacity` | float | Opacity for unfocused windows (0.0 to 1.0) |
| `wallpaper` | path | Wallpaper image path (~ expands to $HOME) |
| `wallpaper_mode` | string | fill, fit, stretch, center, tile |
| `kb_layouts` | string | XKB layout list (e.g., us,ru) |
| `kb_variant` | string | XKB variant |
| `kb_options` | string | XKB options (e.g., grp:alt_shift_toggle) |
| `kb_model` | string | XKB keyboard model |
| `kb_rules` | string | XKB rules |
| `cursor_theme` | string | Cursor theme name |
| `cursor_size` | integer | Cursor size in pixels |
| `sticky` | string | Comma-separated app_id list for sticky windows |
| `smart_gaps` | integer | 1 removes outer gaps when workspace has one tiled window |

Example:

```conf
general.border_width = 2
general.border_color_focused = 0x4d80ff
general.border_color_unfocused = 0x333333
general.background_color = 0x0a0d12
general.gaps_in = 4
general.gaps_out = 8
general.corner_radius = 0
general.active_opacity = 1.0
general.inactive_opacity = 1.0
general.wallpaper = ~/Pictures/wallpaper.png
general.wallpaper_mode = fill
general.kb_layouts = us,ru
general.kb_options = grp:alt_shift_toggle
general.cursor_theme = default
general.cursor_size = 24
```

### Output configuration

Configure monitor position, resolution, and refresh rate.

Format: `general.output = <name>,<mode>,<position>`

| Field | Description |
|-------|-------------|
| `<name>` | Output name (HDMI-A-1, DP-1, eDP-1, etc.) |
| `<mode>` | `preferable`, `disable`, or `WIDTHxHEIGHT@REFRESH` |
| `<position>` | X and Y coordinates as `XxY` |

Examples:

```conf
# Use preferred mode, position at origin
general.output = HDMI-A-1,preferable,0x0

# Custom resolution and refresh, position right of first monitor
general.output = DP-1,2560x1440@144,1920x0

# Disable internal laptop display
general.output = eDP-1,disable

# Use preferred mode, position below first monitor
general.output = DP-2,preferable,0x1440
```

Use `wlr-randr` to get output names and available modes.

### Window rules

Apply actions to windows at map time.

Format: `windowrule = <action>, <match>:<pattern>`

Actions:

| Action | Description |
|--------|-------------|
| `float` | open as floating, centered |
| `tile` | open as tiled |
| `sticky` | open as sticky on all workspaces |
| `fullscreen` | open fullscreen |
| `center` | center the window |
| `workspace N` | assign window to workspace N (1 to 10, opens in background if workspace is hidden) |

Match types:

- `app_id:<name>` - XDG app_id
- `class:<name>` - X11 WM_CLASS
- `instance:<name>` - X11 instance
- `title:<name>` - window title

Pattern `*` matches any value. Rules apply in config order.

Example:

```conf
windowrule = float, app_id:pavucontrol
windowrule = float, class:Steam
windowrule = workspace 3, app_id:firefox
windowrule = sticky, app_id:vult
```

### Animation curves

Define custom cubic bezier easing curves.

Format: `bezier = <name>, <p1x>, <p1y>, <p2x>, <p2y>`

Control points p1 and p2 define curve shape. Values typically 0.0 to 1.0 but can exceed for overshoot effects.

Example:

```conf
bezier = easeOutBack, 0.34, 1.56, 0.64, 1.0
bezier = easeOutQuint, 0.22, 1.0, 0.36, 1.0
bezier = mySmoothCurve, 0.0, 0.0, 0.2, 1.0
```

### Animation rules

Apply curves to animation types.

Format: `animation = <type>, <enabled>, <duration_multiplier>, <curve>`

| Field | Description |
|-------|-------------|
| `<type>` | `windows` (geometry/movement) or `fade` (opacity/borders) |
| `<enabled>` | `1` to enable, `0` to disable |
| `<duration_multiplier>` | Duration in 100ms units (1=100ms, 2=200ms, 4=400ms) |
| `<curve>` | Name of defined bezier curve or `default` |

Example:

```conf
animation = windows, 1, 4, easeOutBack
animation = fade, 1, 3, easeOutQuint
```

### Blur rules

Apply blur effect to specific windows.

Format: `blur = <pattern>, <radius>, <strength>`

| Field | Description |
|-------|-------------|
| `<pattern>` | Match pattern with prefix |
| `<radius>` | Blur radius in pixels |
| `<strength>` | Blur intensity (0.0 to 1.0) |

Pattern prefixes:

- `app_id:<name>` - match XDG app_id
- `class:<name>` - match X11 class
- `instance:<name>` - match X11 instance
- `title:<name>` - match window title
- `namespace:<name>` - match layer shell namespace

Example:

```conf
blur = app_id:kitty, 6, 1.0
blur = class:firefox, 8, 0.8
blur = namespace:waybar, 4, 0.6
```

### Keybinds

Format: `bind.<modifiers>.<key> = <action>`

#### Modifiers

- `super` - Super/Logo/Windows key (MOD4)
- `shift` - Shift
- `ctrl` or `control` - Control
- `alt` - Alt

Combine multiple modifiers with dots: `bind.super.shift.c`

#### Keys

- Letters: `a` to `z`
- Digits: `0` to `9`
- Function keys: `F1` to `F12`
- Arrows: `left`, `right`, `up`, `down`
- Special: `ESC`, `ENTER`, `TAB`, `SPACE`, `BACKSPACE`, `DELETE`, `HOME`, `END`, `PAGEUP`, `PAGEDOWN`, `PRINT`

#### Actions

| Action | Description |
|--------|-------------|
| `exec <command>` | Run shell command |
| `close` | Close focused window |
| `quit` | Stop compositor |
| `togglefloating` | Switch between tiled and floating |
| `fullscreen` | Toggle fullscreen |
| `center` | Center floating window on output |
| `focus <direction>` | Focus window in direction (left/down/up/right) |
| `move <direction>` | Move window in direction |
| `moveoutput <direction>` | Move window to adjacent output |
| `resize <direction>` | Resize tiled window or floating window |
| `workspace N` | Activate workspace 1-10 (0 means 10) |
| `vt N` | Switch to virtual terminal N |

Example:

```conf
bind.super.q = exec kitty
bind.super.e = exec nautilus
bind.super.c = close
bind.super.v = togglefloating
bind.super.f = fullscreen
bind.super.shift.c = center
bind.super.esc = quit
bind.super.left = focus left
bind.super.right = focus right
bind.super.up = focus up
bind.super.down = focus down
bind.super.shift.left = move left
bind.super.shift.right = move right
bind.super.ctrl.h = resize left
bind.super.ctrl.l = resize right
bind.super.1 = workspace 1
bind.super.2 = workspace 2
bind.super.0 = workspace 10
bind.ctrl.alt.F1 = vt 1
bind.ctrl.alt.F2 = vt 2
```

### Startup commands

Run commands once at compositor startup.

Format: `exec-once = <command>`

Example:

```conf
exec-once = waybar
exec-once = mako
exec-once = fcitx5
```

## Workspace rules

- Each output shows one workspace
- Visible workspace belongs to its output
- Hidden workspace remembers last output
- Activate visible workspace on another output: focus and cursor move there, both outputs keep their workspaces
- Activate hidden workspace with last output: workspace opens on last output, focus and cursor move there
- Activate hidden workspace without last output: workspace opens on requesting output

## IPC

Socket: `$XDG_RUNTIME_DIR/wyowm.sock` or `/tmp/wyowm.sock`

Commands:

- `workspaces` - print workspace state as JSON
- `ws N` - activate workspace N

Use wyoctl tool:

```bash
wyoctl workspaces
wyoctl ws 3
```

## Screen sharing

Compositor exposes wlr-screencopy protocol. Install `xdg-desktop-portal` and `xdg-desktop-portal-wlr` for OBS and Discord. Compositor starts both portals at startup when binaries exist.

## Logs

- `/tmp/wyowm.log` - compositor log
- `/tmp/wyowm-spawn.log` - stdout and stderr of spawned commands

## Output management

Use `wlr-randr` to query and configure outputs at runtime:

```bash
wlr-randr
wlr-randr --output DP-1 --mode 2560x1440@144 --pos 1920,0
```

Output configuration from config file applies at startup. Runtime changes via wlr-randr override config but do not persist.
