#include "blur.h"
#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

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
	"uniform float u_alpha;\n"
	"void main() {\n"
	"    vec4 c = texture2D(u_tex, v_uv);\n"
	"    gl_FragColor = vec4(c.rgb * u_alpha, c.a * u_alpha);\n"
	"}\n";

static const char *BLUR_FRAG_SRC =
	"precision mediump float;\n"
	"varying vec2 v_uv;\n"
	"uniform sampler2D u_tex;\n"
	"uniform vec2 u_texel;\n"
	"uniform vec2 u_dir;\n"
	"uniform float u_weights[17];\n"
	"void main() {\n"
	"    vec4 sum = texture2D(u_tex, v_uv) * u_weights[0];\n"
	"    for (int i = 1; i <= 16; i++) {\n"
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
	if (radius < 1) {
		radius = 1;
	}
	if (radius > BLUR_MAX_TAPS) {
		radius = BLUR_MAX_TAPS;
	}

	float sigma = (float)radius / 2.0f;
	if (sigma < 0.5f) {
		sigma = 0.5f;
	}

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

static void compute_weights_table(BlurContext *ctx) {
	memset(ctx->weights_table, 0, sizeof(ctx->weights_table));
	ctx->weights_table[0][0] = 1.0f;

	for (int r = 1; r <= BLUR_MAX_TAPS; r++) {
		compute_weights(ctx->weights_table[r], r);
	}
}

static void create_texture(GLuint *tex, int w, int h) {
	glGenTextures(1, tex);
	glBindTexture(GL_TEXTURE_2D, *tex);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glBindTexture(GL_TEXTURE_2D, 0);
}

static void attach_fbo(GLuint *fbo, GLuint tex) {
	glGenFramebuffers(1, fbo);
	glBindFramebuffer(GL_FRAMEBUFFER, *fbo);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

static void ensure_sizes(BlurContext *ctx, int w, int h, int hw, int hh) {
	if (ctx->src_w != w || ctx->src_h != h) {
		if (ctx->tex_src) {
			glDeleteTextures(1, &ctx->tex_src);
		}
		ctx->tex_src = 0;
		ctx->src_w = w;
		ctx->src_h = h;
		create_texture(&ctx->tex_src, w, h);
	}

	if (ctx->half_w != hw || ctx->half_h != hh) {
		if (ctx->tex_a) {
			glDeleteTextures(1, &ctx->tex_a);
		}
		if (ctx->tex_b) {
			glDeleteTextures(1, &ctx->tex_b);
		}
		if (ctx->fbo_a) {
			glDeleteFramebuffers(1, &ctx->fbo_a);
		}
		if (ctx->fbo_b) {
			glDeleteFramebuffers(1, &ctx->fbo_b);
		}

		ctx->tex_a = 0;
		ctx->tex_b = 0;
		ctx->fbo_a = 0;
		ctx->fbo_b = 0;
		ctx->half_w = hw;
		ctx->half_h = hh;

		create_texture(&ctx->tex_a, hw, hh);
		create_texture(&ctx->tex_b, hw, hh);
		attach_fbo(&ctx->fbo_a, ctx->tex_a);
		attach_fbo(&ctx->fbo_b, ctx->tex_b);
	}
}

static void draw_quad(void) {
	glEnableVertexAttribArray(0);
	glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableVertexAttribArray(0);
}

bool blur_init(BlurContext *ctx, int radius) {
	(void)radius;

	if (!ctx) {
		return false;
	}

	memset(ctx, 0, sizeof(*ctx));
	compute_weights_table(ctx);

	GLuint vert = compile_shader(GL_VERTEX_SHADER, VERT_SRC);
	GLuint copy_frag = compile_shader(GL_FRAGMENT_SHADER, COPY_FRAG_SRC);
	GLuint blur_frag = compile_shader(GL_FRAGMENT_SHADER, BLUR_FRAG_SRC);

	if (!vert || !copy_frag || !blur_frag) {
		if (vert) {
			glDeleteShader(vert);
		}
		if (copy_frag) {
			glDeleteShader(copy_frag);
		}
		if (blur_frag) {
			glDeleteShader(blur_frag);
		}
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
	ctx->copy_u_alpha = glGetUniformLocation(ctx->copy_prog, "u_alpha");

	ctx->blur_u_tex = glGetUniformLocation(ctx->blur_prog, "u_tex");
	ctx->blur_u_texel = glGetUniformLocation(ctx->blur_prog, "u_texel");
	ctx->blur_u_dir = glGetUniformLocation(ctx->blur_prog, "u_dir");
	ctx->blur_u_uv_rect = glGetUniformLocation(ctx->blur_prog, "u_uv_rect");
	ctx->blur_u_weights = glGetUniformLocation(ctx->blur_prog, "u_weights[0]");

	if (ctx->blur_u_weights < 0) {
		ctx->blur_u_weights = glGetUniformLocation(ctx->blur_prog, "u_weights");
	}

	if (ctx->blur_u_weights < 0 || ctx->copy_u_alpha < 0) {
		fprintf(stderr, "wyoWM blur: uniform not found\n");
		blur_fini(ctx);
		return false;
	}

	static const float quad[] = {
		-1.0f, -1.0f,
		 1.0f, -1.0f,
		-1.0f,  1.0f,
		 1.0f,  1.0f
	};

	glGenBuffers(1, &ctx->quad_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, ctx->quad_vbo);
	glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	ctx->initialized = true;
	fprintf(stderr, "wyoWM: blur initialized\n");
	return true;
}

void blur_fini(BlurContext *ctx) {
	if (!ctx) {
		return;
	}

	if (ctx->copy_prog) {
		glDeleteProgram(ctx->copy_prog);
	}
	if (ctx->blur_prog) {
		glDeleteProgram(ctx->blur_prog);
	}
	if (ctx->quad_vbo) {
		glDeleteBuffers(1, &ctx->quad_vbo);
	}
	if (ctx->tex_src) {
		glDeleteTextures(1, &ctx->tex_src);
	}
	if (ctx->tex_a) {
		glDeleteTextures(1, &ctx->tex_a);
	}
	if (ctx->tex_b) {
		glDeleteTextures(1, &ctx->tex_b);
	}
	if (ctx->fbo_a) {
		glDeleteFramebuffers(1, &ctx->fbo_a);
	}
	if (ctx->fbo_b) {
		glDeleteFramebuffers(1, &ctx->fbo_b);
	}

	memset(ctx, 0, sizeof(*ctx));
}

bool blur_apply_full(BlurContext *ctx, GLuint src_fbo, GLuint dst_fbo, int w, int h, int radius) {
	if (!ctx || !ctx->initialized) {
		return false;
	}
	if (!src_fbo || !dst_fbo) {
		return false;
	}
	if (w <= 0 || h <= 0) {
		return false;
	}

	if (radius < 1) {
		radius = 1;
	}
	if (radius > BLUR_MAX_TAPS) {
		radius = BLUR_MAX_TAPS;
	}

	int hw = (w + 1) / 2;
	int hh = (h + 1) / 2;
	if (hw < 1) {
		hw = 1;
	}
	if (hh < 1) {
		hh = 1;
	}

	ensure_sizes(ctx, w, h, hw, hh);

	GLint old_fbo = 0;
	GLint old_viewport[4] = {0};
	GLboolean old_blend = GL_FALSE;
	GLboolean old_scissor = GL_FALSE;

	glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fbo);
	glGetIntegerv(GL_VIEWPORT, old_viewport);
	old_blend = glIsEnabled(GL_BLEND);
	old_scissor = glIsEnabled(GL_SCISSOR_TEST);

	glDisable(GL_BLEND);
	glDisable(GL_SCISSOR_TEST);
	glBindBuffer(GL_ARRAY_BUFFER, ctx->quad_vbo);
	glActiveTexture(GL_TEXTURE0);

	glBindFramebuffer(GL_FRAMEBUFFER, src_fbo);
	glBindTexture(GL_TEXTURE_2D, ctx->tex_src);
	glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);

	glUseProgram(ctx->copy_prog);
	glUniform1i(ctx->copy_u_tex, 0);
	glUniform1f(ctx->copy_u_alpha, 1.0f);
	glUniform4f(ctx->copy_u_uv_rect, 0.0f, 0.0f, 1.0f, 1.0f);

	glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo_a);
	glViewport(0, 0, hw, hh);
	glBindTexture(GL_TEXTURE_2D, ctx->tex_src);
	draw_quad();

	int half_radius = radius / 2;
	if (half_radius < 1) {
		half_radius = 1;
	}
	if (half_radius > BLUR_MAX_TAPS) {
		half_radius = BLUR_MAX_TAPS;
	}

	const float *weights = ctx->weights_table[half_radius];

	glUseProgram(ctx->blur_prog);
	glUniform1i(ctx->blur_u_tex, 0);
	glUniform1fv(ctx->blur_u_weights, BLUR_MAX_TAPS, weights);
	glUniform4f(ctx->blur_u_uv_rect, 0.0f, 0.0f, 1.0f, 1.0f);
	glUniform2f(ctx->blur_u_texel, 1.0f / (float)hw, 1.0f / (float)hh);

	glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo_b);
	glViewport(0, 0, hw, hh);
	glBindTexture(GL_TEXTURE_2D, ctx->tex_a);
	glUniform2f(ctx->blur_u_dir, 1.0f, 0.0f);
	draw_quad();

	glBindFramebuffer(GL_FRAMEBUFFER, ctx->fbo_a);
	glViewport(0, 0, hw, hh);
	glBindTexture(GL_TEXTURE_2D, ctx->tex_b);
	glUniform2f(ctx->blur_u_dir, 0.0f, 1.0f);
	draw_quad();

	glUseProgram(ctx->copy_prog);
	glUniform1i(ctx->copy_u_tex, 0);
	glUniform1f(ctx->copy_u_alpha, 1.0f);
	glUniform4f(ctx->copy_u_uv_rect, 0.0f, 0.0f, 1.0f, 1.0f);

	glBindFramebuffer(GL_FRAMEBUFFER, dst_fbo);
	glViewport(0, 0, w, h);
	glBindTexture(GL_TEXTURE_2D, ctx->tex_a);
	draw_quad();

	glBindBuffer(GL_ARRAY_BUFFER, 0);
	glBindTexture(GL_TEXTURE_2D, 0);

	if (old_blend) {
		glEnable(GL_BLEND);
	}
	if (old_scissor) {
		glEnable(GL_SCISSOR_TEST);
	}

	glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)old_fbo);
	glViewport(old_viewport[0], old_viewport[1], old_viewport[2], old_viewport[3]);

	return true;
}
