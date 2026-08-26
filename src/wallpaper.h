#ifndef WALLPAPER_H
#define WALLPAPER_H

#include <stdbool.h>
#include <wlr/render/allocator.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_buffer.h>

struct Output;

bool wallpaper_load_file(struct wlr_renderer *renderer, struct wlr_allocator *allocator, const char *path,
                         struct wlr_buffer **buffer, int *width, int *height);
void wallpaper_configure_output(struct Output *output);

#endif
