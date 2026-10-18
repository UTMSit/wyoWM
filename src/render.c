#include "render.h"
#include "output.h"
#include "server.h"
#include "blur.h"
#include <stdlib.h>
#include <time.h>
#include <math.h>
#include <string.h>
#include <pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/render/pass.h>
#include <wlr/render/gles2.h>
#include <wlr/render/egl.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/util/box.h>
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <drm_fourcc.h>

#define MAX_BLUR_RECTS 64
#define BLUR_MODE_ALL 0
#define BLUR_MODE_SKIP 1
#define BLUR_MODE_ONLY 2

static BlurContext g_blur = {0};
static bool g_blur_tried = false;
static struct wlr_buffer *g_offscreen = NULL;
static int g_offscreen_w = 0;
static int g_offscreen_h = 0;
typedef struct GLScope {
    EGLDisplay old_dpy;
    EGLContext old_ctx;
    EGLSurface old_draw;
    EGLSurface old_read;
    bool restore;
} GLScope;

static bool gl_scope_begin(Server *server, GLScope *scope) {
    struct wlr_egl *egl = wlr_gles2_renderer_get_egl(server->renderer);
    if (!egl) return false;

    EGLDisplay egl_dpy = wlr_egl_get_display(egl);
    EGLContext egl_ctx = wlr_egl_get_context(egl);
    if (egl_dpy == EGL_NO_DISPLAY || egl_ctx == EGL_NO_CONTEXT) return false;

    scope->old_dpy = eglGetCurrentDisplay();
    scope->old_ctx = eglGetCurrentContext();
    scope->old_draw = eglGetCurrentSurface(EGL_DRAW);
    scope->old_read = eglGetCurrentSurface(EGL_READ);
    scope->restore = scope->old_dpy != EGL_NO_DISPLAY && scope->old_ctx != EGL_NO_CONTEXT;

    if (!eglMakeCurrent(egl_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_ctx)) {
        scope->restore = false;
        return false;
    }

    return true;
}

static void gl_scope_end(Server *server, GLScope *scope) {
    if (scope->restore) {
        eglMakeCurrent(scope->old_dpy, scope->old_draw, scope->old_read, scope->old_ctx);
        return;
    }

    struct wlr_egl *egl = wlr_gles2_renderer_get_egl(server->renderer);
    if (egl) {
        eglMakeCurrent(
            wlr_egl_get_display(egl),
            EGL_NO_SURFACE,
            EGL_NO_SURFACE,
            wlr_egl_get_context(egl)
        );
    }
}

typedef struct FrameTextureList {
    struct wlr_texture **items;
    size_t count;
    size_t capacity;
} FrameTextureList;

typedef struct FrameSurfaceList {
    struct wlr_surface **items;
    size_t count;
    size_t capacity;
} FrameSurfaceList;

static void ftl_push(FrameTextureList *list, struct wlr_texture *texture) {
    if (!list || !texture) return;
    if (list->count == list->capacity) {
        size_t cap = list->capacity ? list->capacity * 2 : 16;
        struct wlr_texture **items = realloc(list->items, cap * sizeof(*items));
        if (!items) return;
        list->items = items;
        list->capacity = cap;
    }
    list->items[list->count++] = texture;
}

static void ftl_finish(FrameTextureList *list) {
    if (!list) return;
    for (size_t i = 0; i < list->count; i++) {
        if (list->items[i]) wlr_texture_destroy(list->items[i]);
    }
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static void fsl_push(FrameSurfaceList *list, struct wlr_surface *surface) {
    if (!list || !surface) return;
    if (list->count == list->capacity) {
        size_t cap = list->capacity ? list->capacity * 2 : 32;
        struct wlr_surface **items = realloc(list->items, cap * sizeof(*items));
        if (!items) return;
        list->items = items;
        list->capacity = cap;
    }
    list->items[list->count++] = surface;
}

static void fsl_finish(FrameSurfaceList *list) {
    if (!list) return;
    free(list->items);
    list->items = NULL;
    list->count = 0;
    list->capacity = 0;
}

static bool surface_needs_blur(struct wlr_surface *surface) {
    if (!surface) return false;
    struct wlr_xdg_surface *xdg = wlr_xdg_surface_try_from_wlr_surface(surface);
    if (xdg && xdg->role == WLR_XDG_SURFACE_ROLE_TOPLEVEL && xdg->toplevel) {
        const char *app_id = xdg->toplevel->app_id;
        if (app_id && strcmp(app_id, "kitty") == 0) return true;
    }
    struct wlr_layer_surface_v1 *ls = wlr_layer_surface_v1_try_from_wlr_surface(surface);
    if (ls) {
        const char *ns = ls->namespace;
        if (ns && strcmp(ns, "waybar") == 0) return true;
    }
    return false;
}

static int collect_blur_rects(
    struct wlr_scene_tree *tree,
    int offset_x, int offset_y,
    BlurRect *rects, int max_rects, int count
) {
    if (!tree || !tree->node.enabled || count >= max_rects) return count;
    struct wlr_scene_node *child;
    wl_list_for_each(child, &tree->children, link) {
        if (!child->enabled) continue;
        int cx = offset_x + child->x;
        int cy = offset_y + child->y;
        if (child->type == WLR_SCENE_NODE_TREE) {
            struct wlr_scene_tree *sub = wlr_scene_tree_from_node(child);
            count = collect_blur_rects(sub, cx, cy, rects, max_rects, count);
        } else if (child->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(child);
            if (!sb->buffer) continue;
            struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);
            if (!ss || !ss->surface) continue;
            if (!surface_needs_blur(ss->surface)) continue;
            int w = sb->dst_width > 0 ? sb->dst_width : (int)sb->buffer->width;
            int h = sb->dst_height > 0 ? sb->dst_height : (int)sb->buffer->height;
            if (w <= 0 || h <= 0) continue;
            if (count < max_rects) {
                rects[count].x = cx;
                rects[count].y = cy;
                rects[count].w = w;
                rects[count].h = h;
                count++;
            }
        }
    }
    return count;
}

static void render_scene_tree(
    struct wlr_render_pass *pass,
    struct wlr_renderer *renderer,
    struct wlr_scene_tree *tree,
    int offset_x, int offset_y, float scale,
    FrameTextureList *owned, FrameSurfaceList *surfaces,
    int blur_mode
) {
    if (!tree || !tree->node.enabled) return;

    struct wlr_scene_node *child;
    wl_list_for_each(child, &tree->children, link) {
        if (!child->enabled) continue;

        float base_x = (float)(offset_x + child->x) * scale;
        float base_y = (float)(offset_y + child->y) * scale;

        if (child->type == WLR_SCENE_NODE_TREE) {
            struct wlr_scene_tree *sub = wlr_scene_tree_from_node(child);
            render_scene_tree(pass, renderer, sub,
                offset_x + child->x, offset_y + child->y,
                scale, owned, surfaces, blur_mode);
        } else if (child->type == WLR_SCENE_NODE_RECT) {
            if (blur_mode == BLUR_MODE_ONLY) continue;

            struct wlr_scene_rect *rect = wlr_scene_rect_from_node(child);
            if (rect->width <= 0 || rect->height <= 0) continue;
            if (rect->color[3] <= 0.0f) continue;

            struct wlr_render_rect_options opts = {0};
            opts.box.x = (int)lround(base_x);
            opts.box.y = (int)lround(base_y);
            opts.box.width = (int)lround((float)rect->width * scale);
            opts.box.height = (int)lround((float)rect->height * scale);
            opts.color.r = rect->color[0] * rect->color[3];
            opts.color.g = rect->color[1] * rect->color[3];
            opts.color.b = rect->color[2] * rect->color[3];
            opts.color.a = rect->color[3];
            wlr_render_pass_add_rect(pass, &opts);
        } else if (child->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_buffer *sb = wlr_scene_buffer_from_node(child);
            if (!sb->buffer) continue;
            if (sb->opacity <= 0.0f) continue;

            struct wlr_scene_surface *ss = wlr_scene_surface_try_from_buffer(sb);

            bool needs_blur = false;
            if (ss && ss->surface) {
                struct wlr_surface *root = wlr_surface_get_root_surface(ss->surface);
                needs_blur = surface_needs_blur(ss->surface) ||
                             (root && surface_needs_blur(root));
            }

            if (blur_mode == BLUR_MODE_SKIP && needs_blur) {
                continue;
            }
            if (blur_mode == BLUR_MODE_ONLY && !needs_blur) {
                continue;
            }

            struct wlr_texture *texture = NULL;
            if (ss && ss->surface) {
                texture = wlr_surface_get_texture(ss->surface);
            }
            if (!texture) {
                texture = wlr_texture_from_buffer(renderer, sb->buffer);
                if (!texture) continue;
                ftl_push(owned, texture);
            }

            int dst_w = sb->dst_width;
            int dst_h = sb->dst_height;
            if (dst_w <= 0) {
                dst_w = sb->src_box.width > 0.0
                    ? (int)sb->src_box.width : (int)texture->width;
            }
            if (dst_h <= 0) {
                dst_h = sb->src_box.height > 0.0
                    ? (int)sb->src_box.height : (int)texture->height;
            }
            if (dst_w <= 0 || dst_h <= 0) continue;

            struct wlr_render_texture_options opts = {0};
            opts.texture = texture;
            opts.src_box = sb->src_box;
            opts.dst_box.x = (int)lround(base_x);
            opts.dst_box.y = (int)lround(base_y);
            opts.dst_box.width = (int)lround((float)dst_w * scale);
            opts.dst_box.height = (int)lround((float)dst_h * scale);
            opts.transform = sb->transform;
            opts.filter_mode = sb->filter_mode;
            if (sb->opacity < 1.0f) {
                opts.alpha = &sb->opacity;
            }
            wlr_render_pass_add_texture(pass, &opts);

            if (ss && ss->surface) {
                fsl_push(surfaces, ss->surface);
            }
        }
    }
}

static struct wlr_buffer *ensure_offscreen(Server *server, int w, int h) {
    if (g_offscreen && g_offscreen_w == w && g_offscreen_h == h) {
        return g_offscreen;
    }
    if (g_offscreen) {
        wlr_buffer_unlock(g_offscreen);
        g_offscreen = NULL;
    }

    const struct wlr_drm_format *fmt = NULL;
    const struct wlr_drm_format_set *formats =
        wlr_renderer_get_texture_formats(server->renderer, WLR_BUFFER_CAP_DMABUF);
    if (formats) {
        fmt = wlr_drm_format_set_get(formats, DRM_FORMAT_ARGB8888);
        if (!fmt) {
            fmt = wlr_drm_format_set_get(formats, DRM_FORMAT_XRGB8888);
        }
    }
    if (!fmt) {
        formats = wlr_renderer_get_texture_formats(server->renderer, 0);
        if (formats) {
            fmt = wlr_drm_format_set_get(formats, DRM_FORMAT_ARGB8888);
        }
    }
    if (!fmt) {
        formats = wlr_renderer_get_texture_formats(server->renderer, WLR_BUFFER_CAP_DATA_PTR);
        if (formats) {
            fmt = wlr_drm_format_set_get(formats, DRM_FORMAT_ARGB8888);
        }
    }
    if (!fmt) return NULL;

    struct wlr_buffer *buf = wlr_allocator_create_buffer(server->allocator, w, h, fmt);
    if (!buf) return NULL;

    wlr_buffer_lock(buf);
    g_offscreen = buf;
    g_offscreen_w = w;
    g_offscreen_h = h;
    return buf;
}

static bool render_direct(struct Output *output) {
    Server *server = output->server;
    struct wlr_output *wlr_output = output->wlr_output;
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    struct wlr_render_pass *pass = wlr_output_begin_render_pass(wlr_output, &state, NULL);
    if (!pass) {
        wlr_output_state_finish(&state);
        return false;
    }
    FrameTextureList owned = {0};
    FrameSurfaceList surfaces = {0};
    float scale = wlr_output->scale > 0.0f ? wlr_output->scale : 1.0f;
    render_scene_tree(pass, server->renderer, server->scene_tree,
        -output->x, -output->y, scale, &owned, &surfaces, BLUR_MODE_ALL);
    int dw = wlr_output->width > 0 ? wlr_output->width : 1;
    int dh = wlr_output->height > 0 ? wlr_output->height : 1;
    pixman_region32_t damage;
    pixman_region32_init_rect(&damage, 0, 0, dw, dh);
    wlr_output_state_set_damage(&state, &damage);
    wlr_output_add_software_cursors_to_render_pass(wlr_output, pass, &damage);
    bool ok = wlr_render_pass_submit(pass);
    if (ok) ok = wlr_output_commit_state(wlr_output, &state);
    if (ok) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        for (size_t i = 0; i < surfaces.count; i++) {
            if (surfaces.items[i]) wlr_surface_send_frame_done(surfaces.items[i], &now);
        }
    }
    pixman_region32_fini(&damage);
    ftl_finish(&owned);
    fsl_finish(&surfaces);
    wlr_output_state_finish(&state);
    return ok;
}

static bool render_with_blur(struct Output *output, BlurRect *rects, int num_rects) {
    Server *server = output->server;
    struct wlr_output *wlr_output = output->wlr_output;
    int ow = wlr_output->width;
    int oh = wlr_output->height;
    if (ow <= 0 || oh <= 0) return render_direct(output);

    struct wlr_buffer *offscreen = ensure_offscreen(server, ow, oh);
    if (!offscreen) return render_direct(output);

    struct wlr_buffer_pass_options bpass_opts = {0};
    struct wlr_render_pass *bpass = wlr_renderer_begin_buffer_pass(
        server->renderer, offscreen, &bpass_opts);
    if (!bpass) return render_direct(output);

    FrameTextureList owned = {0};
    FrameSurfaceList surfaces = {0};
    float scale = wlr_output->scale > 0.0f ? wlr_output->scale : 1.0f;

    render_scene_tree(bpass, server->renderer, server->scene_tree,
        -output->x, -output->y, scale, &owned, &surfaces, BLUR_MODE_SKIP);
    wlr_render_pass_submit(bpass);

    GLuint fbo = wlr_gles2_renderer_get_buffer_fbo(server->renderer, offscreen);
    if (!fbo) {
        ftl_finish(&owned);
        fsl_finish(&surfaces);
        return render_direct(output);
    }

    GLScope scope;
    if (!gl_scope_begin(server, &scope)) {
        ftl_finish(&owned);
        fsl_finish(&surfaces);
        return render_direct(output);
    }

    glFinish();

    for (int i = 0; i < num_rects; i++) {
        int rx = (int)lround((float)rects[i].x * scale);
        int ry = (int)lround((float)rects[i].y * scale);
        int rw = (int)lround((float)rects[i].w * scale);
        int rh = (int)lround((float)rects[i].h * scale);
        blur_apply_region(&g_blur, fbo, ow, oh, rx, ry, rw, rh, 2);
    }

    glFinish();
    gl_scope_end(server, &scope);

    struct wlr_texture *offscreen_tex = wlr_texture_from_buffer(server->renderer, offscreen);
    if (!offscreen_tex) {
        ftl_finish(&owned);
        fsl_finish(&surfaces);
        return render_direct(output);
    }

    struct wlr_output_state state;
    wlr_output_state_init(&state);
    struct wlr_render_pass *pass = wlr_output_begin_render_pass(wlr_output, &state, NULL);
    if (!pass) {
        if (offscreen_tex) wlr_texture_destroy(offscreen_tex);
        ftl_finish(&owned);
        fsl_finish(&surfaces);
        wlr_output_state_finish(&state);
        return false;
    }

    if (offscreen_tex) {
        struct wlr_render_texture_options bg = {0};
        bg.texture = offscreen_tex;
        bg.dst_box.x = 0;
        bg.dst_box.y = 0;
        bg.dst_box.width = ow;
        bg.dst_box.height = oh;
        wlr_render_pass_add_texture(pass, &bg);
    }

    render_scene_tree(pass, server->renderer, server->scene_tree,
        -output->x, -output->y, scale, &owned, &surfaces, BLUR_MODE_ONLY);

    int dw = ow > 0 ? ow : 1;
    int dh = oh > 0 ? oh : 1;
    pixman_region32_t damage;
    pixman_region32_init_rect(&damage, 0, 0, dw, dh);
    wlr_output_state_set_damage(&state, &damage);
    wlr_output_add_software_cursors_to_render_pass(wlr_output, pass, &damage);
    bool ok = wlr_render_pass_submit(pass);
    if (ok) ok = wlr_output_commit_state(wlr_output, &state);
    if (ok) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        for (size_t i = 0; i < surfaces.count; i++) {
            if (surfaces.items[i]) wlr_surface_send_frame_done(surfaces.items[i], &now);
        }
    }
    pixman_region32_fini(&damage);
    if (offscreen_tex) wlr_texture_destroy(offscreen_tex);
    ftl_finish(&owned);
    fsl_finish(&surfaces);
    wlr_output_state_finish(&state);
    return ok;
}

static bool output_has_fullscreen_view(Output *output) {
	View *view;
	wl_list_for_each(view, &output->server->views, link) {
		if (view->output == output && view->mapped && view->fullscreen) {
			return true;
		}
	}
	return false;
}

bool output_render_custom(struct Output *output) {
    if (!output || !output->wlr_output || !output->server) return false;
    struct wlr_output *wlr_output = output->wlr_output;
    if (!wlr_output->needs_frame && !output->render_pending) return false;
    output->render_pending = false;
    Server *server = output->server;

    if (!g_blur_tried) {
        g_blur_tried = true;
        if (wlr_renderer_is_gles2(server->renderer)) {
            GLScope scope;
            if (gl_scope_begin(server, &scope)) {
                blur_init(&g_blur, 6);
                gl_scope_end(server, &scope);
            }
        }
    }

    BlurRect rects[MAX_BLUR_RECTS];
    int num_rects = 0;
    if (g_blur.initialized) {
        num_rects = collect_blur_rects(server->scene_tree,
            -output->x, -output->y, rects, MAX_BLUR_RECTS, 0);
    }

    if (num_rects > 0 && g_blur.initialized && !output_has_fullscreen_view(output)) {
        return render_with_blur(output, rects, num_rects);
    }
    return render_direct(output);
}
