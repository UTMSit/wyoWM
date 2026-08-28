#define _GNU_SOURCE
#include "wallpaper.h"
#include "output.h"
#include "server.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <drm_fourcc.h>
#include <gdk-pixbuf/gdk-pixbuf.h>
#include <wlr/render/drm_format_set.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/pass.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_buffer.h>

bool wallpaper_load_file(
    struct wlr_renderer *renderer,
    struct wlr_allocator *allocator,
    const char *path,
    struct wlr_buffer **buffer,
    int *width,
    int *height
) {
    if (!renderer || !allocator || !path || !*path || !buffer) return false;

    GError *load_error = NULL;
    GdkPixbuf *pixbuf = gdk_pixbuf_new_from_file(path, &load_error);
    if (!pixbuf) {
        fprintf(stderr, "wyoWM: wallpaper: cannot load %s: %s\n", path, load_error ? load_error->message : "unknown");
        if (load_error) g_error_free(load_error);
        return false;
    }

    GdkPixbuf *source = pixbuf;
    GdkPixbuf *alpha = NULL;
    if (gdk_pixbuf_get_n_channels(pixbuf) < 4) {
        alpha = gdk_pixbuf_add_alpha(pixbuf, FALSE, 0, 0, 0);
        if (!alpha) { g_object_unref(pixbuf); return false; }
        source = alpha;
    }

    int w = gdk_pixbuf_get_width(source);
    int h = gdk_pixbuf_get_height(source);
    if (w <= 0 || h <= 0) {
        if (alpha) g_object_unref(alpha);
        g_object_unref(pixbuf);
        return false;
    }

    int src_stride = gdk_pixbuf_get_rowstride(source);
    const guchar *src = gdk_pixbuf_get_pixels(source);

    size_t stride = (size_t)w * 4;
    size_t pixel_size = stride * (size_t)h;
    uint8_t *pixels = malloc(pixel_size);
    if (!pixels) {
        if (alpha) g_object_unref(alpha);
        g_object_unref(pixbuf);
        return false;
    }

    for (int y = 0; y < h; y++) {
        const guchar *src_row = src + (size_t)y * (size_t)src_stride;
        uint8_t *dst_row = pixels + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            const guchar *px = src_row + (size_t)x * 4;
            uint8_t *dst = dst_row + (size_t)x * 4;
            dst[0] = px[2];
            dst[1] = px[1];
            dst[2] = px[0];
            dst[3] = px[3];
        }
    }

    struct wlr_texture *texture = wlr_texture_from_pixels(
        renderer, DRM_FORMAT_ARGB8888, (uint32_t)stride, (uint32_t)w, (uint32_t)h, pixels
    );
    free(pixels);
    if (alpha) g_object_unref(alpha);
    g_object_unref(pixbuf);

    if (!texture) {
        fprintf(stderr, "wyoWM: wallpaper: wlr_texture_from_pixels failed\n");
        return false;
    }

    const struct wlr_drm_format_set *formats = wlr_renderer_get_texture_formats(renderer, WLR_BUFFER_CAP_DATA_PTR);
    if (!formats) {
        formats = wlr_renderer_get_texture_formats(renderer, 0);
    }

    struct wlr_buffer *buf = NULL;
    if (formats) {
        const struct wlr_drm_format *format = wlr_drm_format_set_get(formats, DRM_FORMAT_ARGB8888);
        if (!format) format = wlr_drm_format_set_get(formats, DRM_FORMAT_XRGB8888);
        if (format) {
            buf = wlr_allocator_create_buffer(allocator, w, h, format);
        }
    }

    if (!buf) {
        fprintf(stderr, "wyoWM: wallpaper: wlr_allocator_create_buffer failed\n");
        wlr_texture_destroy(texture);
        return false;
    }

    wlr_buffer_lock(buf);
    struct wlr_buffer_pass_options opts = {0};
    struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(renderer, buf, &opts);
    if (pass) {
        struct wlr_render_texture_options tex_opts = {0};
        tex_opts.texture = texture;
        wlr_render_pass_add_texture(pass, &tex_opts);
        wlr_render_pass_submit(pass);
    } else {
        fprintf(stderr, "wyoWM: wallpaper: wlr_renderer_begin_buffer_pass failed\n");
    }
    wlr_buffer_unlock(buf);

    (void)wlr_buffer_lock(buf);

    wlr_texture_destroy(texture);

    *buffer = buf;
    if (width) *width = w;
    if (height) *height = h;
    return true;
}

void wallpaper_configure_output(Output *output) {
    if (!output) return;
    Server *server = output->server;

    if (!server->wallpaper_buffer || server->wallpaper_width <= 0 || server->wallpaper_height <= 0) {
        if (output->wallpaper) {
            wlr_scene_node_destroy(&output->wallpaper->node);
            output->wallpaper = NULL;
            output->wallpaper_buffer = NULL;
        }
        return;
    }

    if (output->wallpaper && output->wallpaper_buffer != server->wallpaper_buffer) {
        wlr_scene_node_destroy(&output->wallpaper->node);
        output->wallpaper = NULL;
        output->wallpaper_buffer = NULL;
    }

    if (!output->wallpaper) {
        output->wallpaper = wlr_scene_buffer_create(server->scene_tree, server->wallpaper_buffer);
        if (!output->wallpaper) return;
        output->wallpaper_buffer = server->wallpaper_buffer;
        wlr_scene_node_lower_to_bottom(&output->wallpaper->node);
    }

    int tex_w = server->wallpaper_width;
    int tex_h = server->wallpaper_height;
    int out_w = output->width;
    int out_h = output->height;
    if (out_w <= 0 || out_h <= 0) return;

    WallpaperMode mode = server->config.wallpaper_mode;
    if (mode == WALLPAPER_MODE_TILE) mode = WALLPAPER_MODE_FILL;

    if (mode == WALLPAPER_MODE_STRETCH) {
        wlr_scene_node_set_position(&output->wallpaper->node, output->x, output->y);
        wlr_scene_buffer_set_source_box(output->wallpaper, NULL);
        wlr_scene_buffer_set_dest_size(output->wallpaper, out_w, out_h);
        return;
    }

    if (mode == WALLPAPER_MODE_FILL) {
        double scale_x = (double)out_w / (double)tex_w;
        double scale_y = (double)out_h / (double)tex_h;
        double scale = scale_x > scale_y ? scale_x : scale_y;
        double src_w = (double)out_w / scale;
        double src_h = (double)out_h / scale;
        double src_x = ((double)tex_w - src_w) / 2.0;
        double src_y = ((double)tex_h - src_h) / 2.0;
        struct wlr_fbox box = { .x = src_x, .y = src_y, .width = src_w, .height = src_h };
        wlr_scene_node_set_position(&output->wallpaper->node, output->x, output->y);
        wlr_scene_buffer_set_source_box(output->wallpaper, &box);
        wlr_scene_buffer_set_dest_size(output->wallpaper, out_w, out_h);
        return;
    }

    if (mode == WALLPAPER_MODE_FIT) {
        double scale_x = (double)out_w / (double)tex_w;
        double scale_y = (double)out_h / (double)tex_h;
        double scale = scale_x < scale_y ? scale_x : scale_y;
        int dest_w = (int)((double)tex_w * scale);
        int dest_h = (int)((double)tex_h * scale);
        int x = output->x + (out_w - dest_w) / 2;
        int y = output->y + (out_h - dest_h) / 2;
        wlr_scene_node_set_position(&output->wallpaper->node, x, y);
        wlr_scene_buffer_set_source_box(output->wallpaper, NULL);
        wlr_scene_buffer_set_dest_size(output->wallpaper, dest_w, dest_h);
        return;
    }

    int x = output->x + (out_w - tex_w) / 2;
    int y = output->y + (out_h - tex_h) / 2;
    wlr_scene_node_set_position(&output->wallpaper->node, x, y);
    wlr_scene_buffer_set_source_box(output->wallpaper, NULL);
    wlr_scene_buffer_set_dest_size(output->wallpaper, tex_w, tex_h);
}
