// src/render.c
#include "render.h"
#include "output.h"
#include "server.h"
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/pass.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/box.h>
#include <time.h>

static void render_scene_tree(
    struct wlr_render_pass *pass,
    struct wlr_scene_tree *tree,
    int x,
    int y
) {
    if (!tree || !tree->node.enabled) return;

    struct wlr_scene_node *child;
    wl_list_for_each(child, &tree->children, link) {
        if (!child->enabled) continue;

        int child_x = x + child->x;
        int child_y = y + child->y;

        if (child->type == WLR_SCENE_NODE_TREE) {
            struct wlr_scene_tree *subtree = wlr_scene_tree_from_node(child);
            render_scene_tree(pass, subtree, child_x, child_y);
        } else if (child->type == WLR_SCENE_NODE_RECT) {
            struct wlr_scene_rect *rect = wlr_scene_rect_from_node(child);
            struct wlr_render_rect_options opts = {0};
            opts.box.x = child_x;
            opts.box.y = child_y;
            opts.box.width = rect->width;
            opts.box.height = rect->height;
            opts.color.r = rect->color[0];
            opts.color.g = rect->color[1];
            opts.color.b = rect->color[2];
            opts.color.a = rect->color[3];
            wlr_render_pass_add_rect(pass, &opts);
        } else if (child->type == WLR_SCENE_NODE_BUFFER) {
            struct wlr_scene_buffer *buffer = wlr_scene_buffer_from_node(child);
            if (!buffer->buffer) continue;

            struct wlr_texture *texture = wlr_texture_from_buffer(
                pass->renderer, buffer->buffer);
            if (!texture) continue;

            struct wlr_render_texture_options opts = {0};
            opts.texture = texture;
            opts.dst_box.x = child_x;
            opts.dst_box.y = child_y;
            opts.dst_box.width = buffer->dst_width > 0 ? buffer->dst_width : texture->width;
            opts.dst_box.height = buffer->dst_height > 0 ? buffer->dst_height : texture->height;
            if (buffer->opacity < 1.0f) {
                opts.alpha = &buffer->opacity;
            }
            wlr_render_pass_add_texture(pass, &opts);
            wlr_texture_destroy(texture);
        }
    }
}

void output_render_custom(struct Output *output) {
    if (!output || !output->wlr_output || !output->server) return;

    Server *server = output->server;
    struct wlr_output *wlr_output = output->wlr_output;

    struct wlr_buffer *buffer = wlr_allocator_create_buffer(
        server->allocator,
        output->width,
        output->height,
        &wlr_output->swapchain->format
    );
    if (!buffer) return;

    struct wlr_buffer_pass_options opts = {0};
    struct wlr_render_pass *pass = wlr_renderer_begin_buffer_pass(
        server->renderer, buffer, &opts);
    if (!pass) {
        wlr_buffer_unlock(buffer);
        return;
    }

    if (server->scene_tree) {
        render_scene_tree(pass, server->scene_tree, 0, 0);
    }

    bool ok = wlr_render_pass_submit(pass);
    if (ok) {
        wlr_output_attach_render(wlr_output, NULL);
        wlr_output_commit(wlr_output);
    }

    wlr_buffer_unlock(buffer);
}
