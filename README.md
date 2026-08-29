# wyoWM

wyoWM is a Wayland compositor. It is based on wlroots 0.20. It uses a dwindle tiling layout. It is written in C.

## Features

- Dwindle tiling layout.
- Directional focus, move, resize, and swap.
- 10 workspaces. A workspace remembers its last output.
- Multi-monitor support.
- Floating and fullscreen windows.
- Pointer drag: Super + left button moves a window, Super + right button resizes it.
- Borders, gaps, corner radius, and animations.
- Wallpaper with fill, fit, stretch, and center modes.
- Text config with hot reload.
- XKB layout config for keyboard switching.
- IPC socket and the wyoctl tool.
- Layer shell support for bars and menus.

## Protocols

xdg-shell, layer-shell, ext-workspace, foreign-toplevel, screencopy, viewporter, fractional-scale, xdg-activation, relative-pointer, pointer-gestures, presentation-time, xdg-decoration (server side), xdg-output.

## Requirements

- wlroots 0.20
- wayland-server
- xkbcommon
- libinput
- libdrm, gbm, pixman-1
- gdk-pixbuf-2.0
- UMK build system: https://github.com/user12msd4c/umk

## Build and install

    umk update

This command builds wyowm and wyoctl, installs the binaries, and installs the session files.

Other targets:

    umk build
    umk install
    umk clean
    umk uninstall

## Installed files

- /usr/local/bin/wyowm
- /usr/local/bin/wyoctl
- /usr/local/bin/wyowm-session
- /usr/local/bin/wyoconf-reload
- /usr/share/wayland-sessions/wyowm.desktop

## Start

Select wyoWM in the display manager. Or run wyowm-session from a TTY.

## Config

The config file is ~/.config/wyoWM/wyowm.conf. The compositor reads it at start. Use wyoconf-reload to apply changes without a restart.

### Example

    general.border_width = 2
    general.border_color_focused = 0x6699ffff
    general.border_color_unfocused = 0x4d4d59ff
    general.background_color = 0x0a0d12ff
    general.gaps_in = 4
    general.gaps_out = 8
    general.corner_radius = 8
    general.animation_duration_ms = 160
    general.wallpaper = ~/Pictures/wallpapers/wallpaper.jpg
    general.wallpaper_mode = fill
    general.kb_layouts = us,ru
    general.kb_options = grp:alt_shift_toggle

    exec-once = pipewire
    exec-once = wireplumber
    exec-once = waybar

    bind.super.q = exec kitty
    bind.super.c = close
    bind.super.v = togglefloating
    bind.super.f = fullscreen
    bind.super.1 = workspace 1

### General keys

| Key | Value |
|---|---|
| border_width | integer, pixels |
| border_color_focused | 0xRRGGBBAA |
| border_color_unfocused | 0xRRGGBBAA |
| background_color | 0xRRGGBBAA |
| gaps_in | integer |
| gaps_out | integer |
| corner_radius | number |
| active_opacity | 0.0 to 1.0, used for the open animation |
| inactive_opacity | parsed but not used |
| animation_duration_ms | integer, 0 disables animations |
| wallpaper | path, ~ is expanded |
| wallpaper_mode | fill, fit, stretch, center |
| kb_layouts | xkb layout list, example: us,ru |
| kb_variant | xkb variant |
| kb_options | xkb options, example: grp:alt_shift_toggle |
| kb_model | xkb model |
| kb_rules | xkb rules |

### Bind syntax

    bind.<modifiers>.<key> = <action>

Modifiers: super, shift, ctrl, alt. Keys: letters, digits, F1 to F12, arrows, ESC, ENTER, TAB, SPACE, PRINT.

Actions:

| Action | Effect |
|---|---|
| exec <command> | run a command |
| close | close the focused window |
| quit | stop the compositor |
| togglefloating | switch between tiled and floating |
| fullscreen | toggle fullscreen |
| center | center a floating window |
| focus left/down/up/right | focus a window in a direction |
| move left/down/up/right | move a window in a direction |
| moveoutput left/down/up/right | move a window to another output |
| resize left/down/up/right | resize the focused window |
| workspace N | activate workspace N, key 0 means 10 |
| vt N | switch virtual terminal |

If the config has no binds, the compositor loads a default set. Super+1 to Super+0 always exist unless you bind the same keys.

### exec-once

    exec-once = <command>

The command runs one time at startup.

## Workspace rules

- Each output shows one workspace.
- A visible workspace belongs to its output.
- A hidden workspace remembers its last output.
- Activate a workspace that is visible on another output: focus and cursor move to that output. Both outputs keep their workspaces.
- Activate a hidden workspace with a last output: the workspace opens on its last output. Focus and cursor move there.
- Activate a hidden workspace without a last output: it opens on the output that received the request.

## IPC

Socket path: $XDG_RUNTIME_DIR/wyowm.sock, fallback /tmp/wyowm.sock.

Commands:

    workspaces      print workspace state as JSON
    ws N            activate workspace N

Use the wyoctl tool:

    wyoctl workspaces
    wyoctl ws 3

## Screen sharing

The compositor exposes wlr-screencopy. For OBS and Discord, install xdg-desktop-portal and xdg-desktop-portal-wlr. The compositor starts both portals at startup when the binaries exist. The compositor writes ~/.config/xdg-desktop-portal/portals.conf one time if the file does not exist.

## Logs

- /tmp/wyowm.log: compositor log.
- /tmp/wyowm-spawn.log: stdout and stderr of spawned commands.
