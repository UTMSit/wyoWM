#ifndef BLUR_H
#define BLUR_H

#include <stdbool.h>
#include <GLES2/gl2.h>

#define BLUR_MAX_TAPS 9

typedef struct BlurRect {
    int x, y, w, h;
} BlurRect;

typedef struct BlurContext {
    GLuint copy_prog;
    GLuint blur_prog;
    GLint copy_u_tex;
    GLint copy_u_uv_rect;
    GLint blur_u_tex;
    GLint blur_u_texel;
    GLint blur_u_dir;
    GLint blur_u_weights;
    GLint blur_u_uv_rect;
    GLuint quad_vbo;
    GLuint fbo_a;
    GLuint fbo_b;
    GLuint tex_a;
    GLuint tex_b;
    int tex_w;
    int tex_h;
    float weights[BLUR_MAX_TAPS];
    int radius;
    bool initialized;
} BlurContext;

bool blur_init(BlurContext *ctx, int radius);
void blur_fini(BlurContext *ctx);
void blur_apply_region(BlurContext *ctx, GLuint src_fbo, int src_w, int src_h,
                       int rx, int ry, int rw, int rh, int downsample);

#endif
