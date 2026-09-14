#include "blur.h"
#include <stdlib.h>
#include <math.h>
#include <stdio.h>

static const char *VERT_SRC =
    "attribute vec2 a_pos;\n"
    "uniform vec4 u_uv_rect;\n"
    "varying vec2 v_uv;\n"
    "void main() {\n"
    "    v_uv = u_uv_rect.xy + (a_pos * 0.5 + 0.5) * u_uv_rect.zw;\n"
    "    gl_Position = vec4(a_pos, 0.0, 1.0);\n"
    "}\n";

static const char *COPY_FRAG_SRC =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "void main() {\n"
    "    gl_FragColor = texture2D(u_tex, v_uv);\n"
    "}\n";

static const char *BLUR_FRAG_SRC =
    "precision mediump float;\n"
    "varying vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform vec2 u_texel;\n"
    "uniform vec2 u_dir;\n"
    "uniform float u_weights[9];\n"
    "void main() {\n"
    "    vec4 sum = texture2D(u_tex, v_uv) * u_weights[0];\n"
    "    for (int i = 1; i <= 8; i++) {\n"
    "        vec2 off = u_dir * u_texel * float(i);\n"
    "        sum += texture2D(u_tex, v_uv + off) * u_weights[i];\n"
    "        sum += texture2D(u_tex, v_uv - off) * u_weights[i];\n"
    "    }\n"
    "    gl_FragColor = sum;\n"
    "}\n";

static GLuint compile_shader(GLenum type, const char *src) {
    GLuint shader = glCreateShader(type);
    glShaderSource(shader, 1, &src, NULL);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(shader, sizeof(log), NULL, log);
        fprintf(stderr, "wyoWM blur: shader error: %s\n", log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static GLuint link_program(GLuint vert, GLuint frag) {
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vert);
    glAttachShader(prog, frag);
    glBindAttribLocation(prog, 0, "a_pos");
    glLinkProgram(prog);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "wyoWM blur: link error: %s\n", log);
        glDeleteProgram(prog);
        return 0;
    }
    glDetachShader(prog, vert);
    glDetachShader(prog, frag);
    return prog;
}

static void compute_weights(float *weights, int radius) {
    float sigma = (float)radius / 2.5f;
    if (sigma < 0.5f) sigma = 0.5f;
    float sum = 0.0f;
    for (int i = 0; i < BLUR_MAX_TAPS; i++) {
        if (i <= radius) {
            float x = (float)i;
            weights[i] = expf(-(x * x) / (2.0f * sigma * sigma));
        } else {
            weights[i] = 0.0f;
        }
        sum += (i == 0) ? weights[i] : 2.0f * weights[i];
    }
    if (sum > 0.0f) {
        for (int i = 0; i < BLUR_MAX_TAPS; i++) {
            weights[i] /= sum;
        }
    }
}

static void create_temp_texture(GLuint *tex, int w, int h) {
    glGenTextures(1, tex);
    glBindTexture(GL_TEXTURE_2D, *tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
}

static void ensure_temp_size(BlurContext *ctx, int w, int h) {
    if (ctx->tex_w >= w && ctx->tex_h >= h) return;
    if (ctx->tex_a) glDeleteTextures(1, &ctx->tex_a);
    if (ctx->tex_b) glDeleteTextures(1, &ctx->tex_b);
    if (ctx->fbo_a) glDeleteFramebuffers(1, &ctx->fbo_a);
    if (ctx->fbo_b) glDeleteFramebuffers(1, &ctx->fbo_b);
    ctx->tex_a = 0;
    ctx->tex_b = 0;
    ctx->fbo_a = 0;
    ctx->fbo_b = 0;
    ctx->tex_w = w;
    ctx->tex_h = h;
    create_temp_texture(&ctx->tex_a, w, h);
    create_temp_texture(&ctx->tex_b, w, h);
    glGenFramebuffers(1, &ctx->fbo_a);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo_a);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx->tex_a, 0);
    glGenFramebuffers(1, &ctx->fbo_b);
    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo_b);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, ctx->tex_b, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void draw_quad(void) {
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    glDisableVertexAttribArray(0);
}

bool blur_init(BlurContext *ctx, int radius) {
    if (!ctx) return false;
    ctx->initialized = false;
    ctx->radius = radius;
    ctx->copy_prog = 0;
    ctx->blur_prog = 0;
    ctx->quad_vbo = 0;
    ctx->fbo_a = 0;
    ctx->fbo_b = 0;
    ctx->tex_a = 0;
    ctx->tex_b = 0;
    ctx->tex_w = 0;
    ctx->tex_h = 0;
    compute_weights(ctx->weights, radius);
    GLuint vert = compile_shader(GL_VERTEX_SHADER, VERT_SRC);
    GLuint copy_frag = compile_shader(GL_FRAGMENT_SHADER, COPY_FRAG_SRC);
    GLuint blur_frag = compile_shader(GL_FRAGMENT_SHADER, BLUR_FRAG_SRC);
    if (!vert || !copy_frag || !blur_frag) {
        if (vert) glDeleteShader(vert);
        if (copy_frag) glDeleteShader(copy_frag);
        if (blur_frag) glDeleteShader(blur_frag);
        return false;
    }
    ctx->copy_prog = link_program(vert, copy_frag);
    ctx->blur_prog = link_program(vert, blur_frag);
    glDeleteShader(vert);
    glDeleteShader(copy_frag);
    glDeleteShader(blur_frag);
    if (!ctx->copy_prog || !ctx->blur_prog) {
        blur_fini(ctx);
        return false;
    }
    ctx->copy_u_tex = glGetUniformLocation(ctx->copy_prog, "u_tex");
    ctx->copy_u_uv_rect = glGetUniformLocation(ctx->copy_prog, "u_uv_rect");
    ctx->blur_u_tex = glGetUniformLocation(ctx->blur_prog, "u_tex");
    ctx->blur_u_texel = glGetUniformLocation(ctx->blur_prog, "u_texel");
    ctx->blur_u_dir = glGetUniformLocation(ctx->blur_prog, "u_dir");
    ctx->blur_u_weights = glGetUniformLocation(ctx->blur_prog, "u_weights[0]");
    ctx->blur_u_uv_rect = glGetUniformLocation(ctx->blur_prog, "u_uv_rect");
    static const float quad[] = { -1.0f, -1.0f, 1.0f, -1.0f, -1.0f, 1.0f, 1.0f, 1.0f };
    glGenBuffers(1, &ctx->quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    ctx->initialized = true;
    fprintf(stderr, "wyoWM: blur initialized, radius=%d\n", radius);
    return true;
}

void blur_fini(BlurContext *ctx) {
    if (!ctx) return;
    if (ctx->copy_prog) glDeleteProgram(ctx->copy_prog);
    if (ctx->blur_prog) glDeleteProgram(ctx->blur_prog);
    if (ctx->quad_vbo) glDeleteBuffers(1, &ctx->quad_vbo);
    if (ctx->tex_a) glDeleteTextures(1, &ctx->tex_a);
    if (ctx->tex_b) glDeleteTextures(1, &ctx->tex_b);
    if (ctx->fbo_a) glDeleteFramebuffers(1, &ctx->fbo_a);
    if (ctx->fbo_b) glDeleteFramebuffers(1, &ctx->fbo_b);
    ctx->copy_prog = 0;
    ctx->blur_prog = 0;
    ctx->quad_vbo = 0;
    ctx->tex_a = 0;
    ctx->tex_b = 0;
    ctx->fbo_a = 0;
    ctx->fbo_b = 0;
    ctx->tex_w = 0;
    ctx->tex_h = 0;
    ctx->initialized = false;
}

void blur_apply_region(BlurContext *ctx, GLuint src_fbo, int src_w, int src_h,
                       int rx, int ry, int rw, int rh, int downsample) {
    if (!ctx || !ctx->initialized) return;

    if (rx < 0) { rw += rx; rx = 0; }
    if (ry < 0) { rh += ry; ry = 0; }
    if (rx + rw > src_w) rw = src_w - rx;
    if (ry + rh > src_h) rh = src_h - ry;
    if (rw <= 0 || rh <= 0) return;

    if (downsample < 1) downsample = 1;

    int ds_w = rw / downsample;
    int ds_h = rh / downsample;
    if (ds_w < 1) ds_w = 1;
    if (ds_h < 1) ds_h = 1;

    ensure_temp_size(ctx, ds_w, ds_h);

    GLint old_fbo = 0;
    GLint old_viewport[4] = {0};
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
    glGetIntegerv(GL_VIEWPORT, old_viewport);
    GLboolean old_blend = glIsEnabled(GL_BLEND);
    GLboolean old_scissor = glIsEnabled(GL_SCISSOR_TEST);

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glBindBuffer(GL_ARRAY_BUFFER, ctx->quad_vbo);

    GLint src_tex = 0;
    glBindFramebuffer(GL_FRAMEBUFFER, src_fbo);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &src_tex);

    if (src_tex == 0) {
        glBindBuffer(GL_ARRAY_BUFFER, 0);
        if (old_blend) glEnable(GL_BLEND);
        if (old_scissor) glEnable(GL_SCISSOR_TEST);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_fbo);
        glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
        return;
    }

    int gl_y = src_h - ry - rh;
    if (gl_y < 0) gl_y = 0;
    if (gl_y + rh > src_h) gl_y = src_h - rh;

    float uv_x = (float)rx / (float)src_w;
    float uv_y = (float)gl_y / (float)src_h;
    float uv_w = (float)rw / (float)src_w;
    float uv_h = (float)rh / (float)src_h;

    glUseProgram(ctx->copy_prog);
    glUniform1i(ctx->copy_u_tex, 0);
    glActiveTexture(GL_TEXTURE0);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo_a);
    glViewport(0, 0, ds_w, ds_h);
    glBindTexture(GL_TEXTURE_2D, (GLuint)src_tex);
    glUniform4f(ctx->copy_u_uv_rect, uv_x, uv_y, uv_w, uv_h);
    draw_quad();

    glUseProgram(ctx->blur_prog);
    glUniform1i(ctx->blur_u_tex, 0);
    glUniform1fv(ctx->blur_u_weights, BLUR_MAX_TAPS, ctx->weights);
    glUniform4f(ctx->blur_u_uv_rect, 0.0f, 0.0f, 1.0f, 1.0f);
    glUniform2f(ctx->blur_u_texel, 1.0f / (float)ds_w, 1.0f / (float)ds_h);

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo_b);
    glViewport(0, 0, ds_w, ds_h);
    glBindTexture(GL_TEXTURE_2D, ctx->tex_a);
    glUniform2f(ctx->blur_u_dir, 1.0f, 0.0f);
    draw_quad();

    glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo_a);
    glViewport(0, 0, ds_w, ds_h);
    glBindTexture(GL_TEXTURE_2D, ctx->tex_b);
    glUniform2f(ctx->blur_u_dir, 0.0f, 1.0f);
    draw_quad();

    glUseProgram(ctx->copy_prog);
    glUniform1i(ctx->copy_u_tex, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, src_fbo);
    glViewport(rx, gl_y, rw, rh);
    glBindTexture(GL_TEXTURE_2D, ctx->tex_a);
    glUniform4f(ctx->copy_u_uv_rect, 0.0f, 0.0f, 1.0f, 1.0f);
    draw_quad();

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindTexture(GL_TEXTURE_2D, 0);

    if (old_blend) glEnable(GL_BLEND);
    if (old_scissor) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_fbo);
    glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);
}
