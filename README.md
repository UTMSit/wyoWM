# wyoWM

wyoWM is a Wayland compositor. It is based on wlroots 0.20. It uses a dwindle tiling layout. It is written in C.

## Features

- Dwindle tiling layout.
- Directional focus, move, resize, and swap.
- 10 workspaces. A workspace remembers its last output.
- Multi-monitor support.
- Floating and fullscreen windows.
- Pointer drag: Super + left button moves a window, Super + right button resizes it.
- Borders, gaps, corner radius, and opacity.
- Fully customizable animations with cubic bezier curves.
- Wallpaper with fill, fit, stretch, and center modes.
- Text config with hot reload.
- XKB layout config for keyboard switching.
- IPC socket and the wyoctl tool.
- Layer shell support for bars and menus.
- XWayland support for legacy X11 applications.
- Tearing control protocol for gaming.

## Protocols

xdg-shell, layer-shell, ext-workspace, foreign-toplevel, screencopy, viewporter, fractional-scale, xdg-activation, relative-pointer, pointer-gestures, presentation-time, xdg-decoration (server side), xdg-output, tearing-control.

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

### Animations

wyoWM supports Hyprland-style animation configuration using custom cubic bezier curves.

#### Bezier curves

Define custom easing curves with 4 control points.

    bezier = <name>, <p1x>, <p1y>, <p2x>, <p2y>

Example:

    bezier = easeOutBack, 0.34, 1.56, 0.64, 1.0
    bezier = easeOutQuint, 0.22, 1.0, 0.36, 1.0
    bezier = mySmoothCurve, 0.0, 0.0, 0.2, 1.0

#### Animation rules

Apply curves to specific animation types.

    animation = <type>, <enabled>, <speed>, <curve>

- `type`: `windows` (geometry/movement), `fade` (opacity/borders), `workspaces` (switching).
- `enabled`: `1` to enable, `0` to disable.
- `speed`: duration multiplier (e.g., `4` = 400ms, `2` = 200ms).
- `curve`: name of a defined bezier curve, or `default`.

Example:

    animation = windows, 1, 4, easeOutBack
    animation = fade, 1, 3, easeOutQuint

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
| active_opacity | 0.0 to 1.0 |
| inactive_opacity | 0.0 to 1.0 |
| wallpaper | path, ~ is expanded |
| wallpaper_mode | fill, fit, stretch, center |
| kb_layouts | xkb layout list, example: us,ru |
| kb_options | xkb options, example: grp:alt_shift_toggle |

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

The compositor exposes wlr-screencopy. For OBS and Discord, install xdg-desktop-portal and xdg-desktop-portal-wlr. The compositor starts both portals at startup when the binaries exist.

## Logs

- /tmp/wyowm.log: compositor log.
- /tmp/wyowm-spawn.log: stdout and stderr of spawned commands.
