#ifndef RENDER_H
#define RENDER_H
#include <stdbool.h>
struct Output;
bool output_render_custom(struct Output *output);
bool output_has_fullscreen_view(struct Output *output);
bool output_try_direct_scanout(struct Output *output);
#endif
