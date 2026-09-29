#ifndef GFX_RENDERING_API_H
#define GFX_RENDERING_API_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

struct ShaderProgram;

struct GfxRenderingAPI {
    bool (*z_is_from_0_to_1)(void);
    void (*unload_shader)(struct ShaderProgram *old_prg);
    void (*load_shader)(struct ShaderProgram *new_prg);
    struct ShaderProgram *(*create_and_load_new_shader)(uint32_t shader_id);
    struct ShaderProgram *(*lookup_shader)(uint32_t shader_id);
    void (*shader_get_info)(struct ShaderProgram *prg, uint8_t *num_inputs, bool used_textures[2]);
    uint32_t (*new_texture)(void);
    void (*select_texture)(int tile, uint32_t texture_id);
    void (*upload_texture)(const uint8_t *rgba32_buf, int width, int height);
    void (*set_sampler_parameters)(int sampler, bool linear_filter, uint32_t cms, uint32_t cmt);
    void (*set_depth_test)(bool depth_test);
    void (*set_depth_mask)(bool z_upd);
    void (*set_zmode_decal)(bool zmode_decal);
    void (*set_viewport)(int x, int y, int width, int height);
    void (*set_scissor)(int x, int y, int width, int height);
    void (*set_use_alpha)(bool use_alpha);
    void (*draw_triangles)(float buf_vbo[], size_t buf_vbo_len, size_t buf_vbo_num_tris);
    void (*init)(void);
    void (*on_resize)(void);
    void (*start_frame)(void);
    void (*end_frame)(void);
    void (*finish_render)(void);
    void (*fill_rect)(int x0, int y0, int x1, int y1, const uint8_t *rgba); // optional; fill 2d rect with color
    void (*tex_rect)(int x0, int y0, int x1, int y1, const float u0, const float v0, const float dudx, const float dvdy, const uint8_t *rgba);
    void (*set_fog_color)(const uint8_t *rgb); // optional; set global fog color
    void (*shutdown)(void); // optional
    /* OPTIONAL fast path: upload an N64 RGBA16 (RGB5551, big-endian) texture
     * straight from ROM, skipping the RGBA8888 intermediate that upload_texture
     * requires.  The generic path costs 10 bytes of memory traffic per texel
     * (expand to 4B in a stack buffer, read it back, write 2B) to produce 4
     * bytes of work; a backend whose native texel is 16-bit can do it in one
     * pass.  Returns 1 if it handled the upload, 0 to fall back to
     * upload_texture (e.g. a backend running in a non-16-bit mode).
     * NULL = not supported; callers must check. */
    int (*upload_texture_rgba16)(const uint8_t *src, int width, int height);
    /* OPTIONAL: the render mode's ALPHA_CVG_SEL bit -- the N64 "alpha drives
     * coverage" mode, where a zero-alpha texel gets zero coverage and is never
     * written.  It does NOT imply use_alpha: opaque surfaces set ALPHA_CVG_SEL
     * while their blender B input (G_BL_A_MEM) makes gfx_pc report use_alpha
     * false and strip the alpha cycle.  Backends that key transparency off a
     * reserved texel value need this to keep discarding transparent texels on
     * those surfaces.  NULL = not supported; callers must check. */
    void (*set_alpha_cvg_sel)(bool alpha_cvg_sel);
};

#endif
