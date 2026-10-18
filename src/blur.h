#ifndef BLUR_H
#define BLUR_H

#include <stdbool.h>
#include <GLES2/gl2.h>

#define BLUR_MAX_TAPS 17

typedef struct BlurRect {
	int x;
	int y;
	int w;
	int h;
	float strength;
} BlurRect;

typedef struct BlurContext {
	GLuint copy_prog;
	GLuint blur_prog;

	GLint copy_u_tex;
	GLint copy_u_uv_rect;
	GLint copy_u_alpha;

	GLint blur_u_tex;
	GLint blur_u_texel;
	GLint blur_u_dir;
	GLint blur_u_weights;
	GLint blur_u_uv_rect;

	GLuint quad_vbo;

	GLuint tex_src;
	GLuint fbo_a;
	GLuint fbo_b;
	GLuint tex_a;
	GLuint tex_b;

	int src_w;
	int src_h;
	int half_w;
	int half_h;

	float weights_table[BLUR_MAX_TAPS + 1][BLUR_MAX_TAPS];

	bool initialized;
} BlurContext;

bool blur_init(BlurContext *ctx, int radius);
void blur_fini(BlurContext *ctx);
bool blur_apply_full(BlurContext *ctx, GLuint src_fbo, GLuint dst_fbo, int w, int h, int radius);

#endif
