/* Synthetic display lists and GPU command capture; no ROM assets required. */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include "of_gpu.h"

static uint64_t digest = 1469598103934665603ULL;
static unsigned draws, uploads, states;
static int recording = 1;
static uint32_t slots[32][8];
static of_gpu_tri_state_t current_state;
static of_gpu_object_state_t current_object;
static unsigned finishes, fills;
static uint32_t fill_addr;
static uint16_t fill_width, fill_height, fill_stride;
static uint8_t fill_byte;
static void record_fill(uint32_t addr,uint16_t w,uint16_t h,uint16_t stride,uint8_t c) {
    fills++;fill_addr=addr;fill_width=w;fill_height=h;fill_stride=stride;fill_byte=c;
}
static int16_t last_x[3], last_y[3];
static int32_t last_depth[3];
static uint16_t last_rgb[3];
static uint16_t previous_rgb[3];
static of_gpu_tri_state_t previous_draw_state, last_draw_state;
static void record_finish(void) { finishes++; }
/* Flip fences: every token counts as reached unless gpu_lag models a GPU that
 * retires the pending flip only after fence_lag_polls more register reads. */
static uint32_t flip_tokens, fence_reached_value;
static int gpu_lag, fence_lag_polls;
static uint32_t record_flip(int idx) { (void)idx; return ++flip_tokens; }
static int record_fence_reached(uint32_t token) {
    if (!gpu_lag || (int32_t)(fence_reached_value - token) >= 0) return 1;
    if (fence_lag_polls > 0 && --fence_lag_polls == 0) { fence_reached_value = token; return 1; }
    return 0;
}
static void hash_bytes(const void *p, size_t n) {
    if (!recording) return;
    const unsigned char *b = p;
    while (n--) { digest ^= *b++; digest *= 1099511628211ULL; }
}
static void record_state(const of_gpu_tri_state_t *s) {
    current_state = *s;
    states++;
    hash_bytes(s, sizeof(*s));
}
static void record_object(const of_gpu_object_state_t *s) { current_object=*s; hash_bytes(s,sizeof(*s)); }
static void record_pal(const int16_t x[3], const int16_t y[3],
                       const int32_t s[3], const int32_t t[3],
                       const int32_t zi[3], const uint8_t light[3]) {
    draws++;
    hash_bytes(x,6); hash_bytes(y,6); hash_bytes(s,12); hash_bytes(t,12);
    hash_bytes(zi,12); hash_bytes(light,3);
}
static void record_load(uint8_t slot, int32_t x, int32_t y, int32_t w,
                        int32_t u, int32_t v, uint16_t rgb, uint32_t depth) {
    uint32_t words[] = {slot, x, y, w, u, v, rgb, depth};
    memcpy(slots[slot], words, sizeof(words));
    uploads++;
    hash_bytes(words, sizeof(words));
}
static void record_indexed(uint8_t a, uint8_t b, uint8_t c) {
    uint8_t idx[] = {a,b,c};
    draws++;
    hash_bytes(idx, sizeof(idx));
}
static void record_rgb(const int16_t x[3], const int16_t y[3],
                       const int32_t s[3], const int32_t t[3],
                       const int32_t zi[3], const uint16_t rgb[3],
                       uint32_t q29, const int32_t depth[3], const uint16_t *rgb_d) {
    draws++;
    hash_bytes(x,6); hash_bytes(y,6); hash_bytes(s,12); hash_bytes(t,12);
    memcpy(previous_rgb,last_rgb,6); previous_draw_state=last_draw_state; last_draw_state=current_state;
    memcpy(last_x,x,6); memcpy(last_y,y,6); memcpy(last_rgb,rgb,6);
    memcpy(last_depth,depth,sizeof(last_depth));
    hash_bytes(zi,12); hash_bytes(rgb,6); hash_bytes(&q29,4); hash_bytes(depth,12);
    if (rgb_d) hash_bytes(rgb_d,6);
}
#define of_gpu_clear_rect_strided record_fill
#define of_gpu_finish record_finish
#define of_gpu_flip_to record_flip
#define of_gpu_fence_reached record_fence_reached
#define of_gpu_set_tri_state record_state
#define of_gpu_load_vert_clip record_load
#define of_gpu_draw_indexed_tri record_indexed
#define of_gpu_draw_vert_tri_rgb record_rgb
#define of_gpu_draw_vert_tri record_pal
#define of_gpu_set_object_state record_object
#include "gfx/gfx_pc.c"
#include "gfx/gfx_gpu.c"

/* A deterministic timer isolates command output from the host wall clock. */
unsigned int of_time_us(void) { static unsigned ticks; return ++ticks; }
static unsigned acquires;
static int acquire_after_fence;
int of_video_acquire_next(int idx, uint32_t token) {
    acquires++;
    acquire_after_fence = !gpu_lag || (int32_t)(fence_reached_value - token) >= 0;
    return (idx+1)%3;
}
uint32_t of_video_vblank_count(void) { return 0; }
uint8_t *of_video_buffer_addr(int idx) { return (uint8_t *)(uintptr_t)(0x11000000+idx*0x40000); }
void of_video_set_display_mode(int mode) { (void)mode; }
void of_video_flip(void) {}
static struct GfxRenderingAPI api;
static unsigned char texture[2048];
static void init(void) {
    api = (struct GfxRenderingAPI){
        .lookup_shader = gpu_lookup_shader, .load_shader = gpu_load_shader,
        .unload_shader = gpu_unload_shader,
        .create_and_load_new_shader = gpu_create_and_load_new_shader,
        .shader_get_info = gpu_shader_get_info,
        .new_texture = gpu_new_texture, .select_texture = gpu_select_texture,
        .upload_texture = gpu_upload_texture, .upload_texture_rgba16 = gpu_upload_texture_rgba16,
        .set_sampler_parameters = gpu_set_sampler_parameters,
        .set_depth_test = gpu_set_depth_test, .set_depth_mask = gpu_set_depth_mask,
        .set_zmode_decal = gpu_set_zmode_decal, .set_use_alpha = gpu_set_use_alpha,
        .set_viewport = gpu_set_viewport, .set_scissor = gpu_set_scissor,
        .draw_triangles = gpu_draw_triangles,
    };
    gfx_rapi = &api;
    g_has_gpu = g_truecolor = g_clip_load = g_combine = 1;
    g_fb_bpp = 2;
    g_draw_fb = 0x11000000; g_white_addr = 0x12000000;
    g_zbuf = (void *)(uintptr_t)0x13000000;
    g_tex[0].ci8 = texture; g_tex[0].addr = 0x14000000;
    g_tex[0].w = g_tex[0].h = 32; g_tex[0].wmask = g_tex[0].hmask = 31;
    g_cur_tex[0] = &g_tex[0];
    rendering_state.textures[0] = &gfx_texture_cache.pool[0];
    rendering_state.textures[1] = &gfx_texture_cache.pool[1];
    gfx_current_dimensions.width = 320; gfx_current_dimensions.height = 240;
    gfx_current_dimensions.aspect_ratio = 4.0f/3.0f;
    rsp.texture_scaling_factor.s = rsp.texture_scaling_factor.t = 65535;
    rsp.geometry_mode = G_ZBUFFER;
    rdp.other_mode_l = Z_UPD;
    for (int i=0; i<32; i++) {
        struct LoadedVertex *v=&rsp.loaded_vertices[i];
        v->x = (i%5-2)*90.25f; v->y = (i%7-3)*61.5f;
        v->w = 100.0f + i*600.25f; v->z = v->w*0.5f; v->w_inv=1.0f/v->w;
        v->u = i*31.25f; v->v = i*15.75f;
        v->color=(struct RGBA){i*7+2,i*5+4,i*3+8,255};
    }
    gfx_dp_set_combine_mode(color_comb(0,0,0,G_CCMUX_SHADE),
                            color_comb(0,0,0,G_ACMUX_SHADE));
}
static void triangle(unsigned i) {
    gfx_push_triangle(&rsp.loaded_vertices[i%30], &rsp.loaded_vertices[(i%30)+1],
                      &rsp.loaded_vertices[(i%30)+2]);
}
static void trace(void) {
    /* Shared vertices, texture/material changes, viewport/scissor/depth,
     * and cached/legacy alternation exercise the single sticky-state owner. */
    for (unsigned batch=0; batch<1000; batch++) {
        unsigned mode=batch%6;
        gfx_dp_set_combine_mode(color_comb(0,0,0,mode&1?G_CCMUX_TEXEL0:G_CCMUX_SHADE),
                                color_comb(0,0,0,G_ACMUX_SHADE));
        rdp.texture_tile.uls = batch%8; rdp.texture_tile.ult = batch%7;
        rdp.texture_tile.cms = batch%4; rdp.texture_tile.cmt = (batch/4)%4;
        rdp.other_mode_h = (batch&1)?G_TF_POINT:G_TF_BILERP;
        rdp.other_mode_l = mode==2 ? 0 : Z_UPD;
        gpu_set_viewport(batch%4, batch%3, 320-batch%4, 240-batch%3);
        gpu_set_scissor(batch%3, batch%4, 318, 237);
        for (unsigned j=0;j<90;j++) {
            triangle(j);
            if (j%19==0) {
                struct LoadedVertex clipped[3];
                memcpy(clipped, rsp.loaded_vertices+4, sizeof(clipped));
                gfx_push_triangle(clipped,clipped+1,clipped+2);
            }
        }
        gfx_flush();
        vc_slot_stale=0xffffffffu;
    }
    printf("trace %016llx draws %u uploads %u states %u\n",
           (unsigned long long)digest,draws,uploads,states);
}

static void check_state(void) {
    for (int i=0; i<1000; i++) {
        gpu_set_depth_mask((i>>1)&1); gpu_set_depth_test(i&1);
        gpu_set_sampler_parameters(0, false, i%4, (i/4)%4);
        gpu_set_scissor(i%7, i%5, 310, 230);
        gpu_select_texture(0, 0);
        g_surf_alpha=i%256; g_subpix_tri=(i/3)&1; g_cd_active=(i/7)&1;
        emit_tri_state(i&1);
        of_gpu_tri_state_t first=current_state;
        unsigned count=states;
        emit_tri_state(i&1);
        assert(states==count);
        /* Force the original full rebuild as an oracle for the fast memo. */
        g_st_cache_valid=0;
        emit_tri_state(i&1);
        assert(!memcmp(&first,&current_state,sizeof(first)));
        g_subpix_tri^=1;
        emit_tri_state(i&1);
        assert(current_state.subpix_y==g_subpix_tri);
    }
}
static void check_colors(void) {
    rdp.other_mode_l=Z_UPD;
    gfx_dp_set_combine_mode(color_comb(0,0,0,G_CCMUX_PRIMITIVE),
                            color_comb(0,0,0,G_ACMUX_SHADE));
    gfx_dp_set_prim_color(255,0,0,255); triangle(0);
    assert(slots[0][6]==rgba_to_565(255,0,0));
    gfx_dp_set_prim_color(0,255,0,255); triangle(0);
    assert(slots[0][6]==rgba_to_565(0,255,0));
    gfx_dp_set_combine_mode(color_comb(0,0,0,G_CCMUX_ENVIRONMENT),
                            color_comb(0,0,0,G_ACMUX_SHADE));
    gfx_dp_set_env_color(0,0,255,255); triangle(0);
    assert(slots[0][6]==rgba_to_565(0,0,255));
    gfx_dp_set_env_color(255,255,0,255); triangle(0);
    assert(slots[0][6]==rgba_to_565(255,255,0));
    gfx_dp_set_combine_mode(color_comb(0,0,0,G_CCMUX_LOD_FRACTION),
                            color_comb(0,0,0,G_ACMUX_SHADE));
    triangle(5); unsigned old=slots[6][6];
    triangle(6); assert(slots[6][6]!=old);
}
static void check_textures(void) {
    struct TextureHashmapNode *node;
    rdp.loaded_texture[0].size_bytes=2048;
    rdp.texture_tile.line_size_bytes=64;
    rdp.palette=texture;
    memset(&gfx_texture_cache,0,sizeof(gfx_texture_cache));
    assert(!gfx_texture_cache_lookup(0,&node,texture,G_IM_FMT_CI,G_IM_SIZ_8b));
    assert(gfx_texture_cache_lookup(0,&node,texture,G_IM_FMT_CI,G_IM_SIZ_8b));
    rdp.palette=texture+256;
    assert(!gfx_texture_cache_lookup(0,&node,texture,G_IM_FMT_CI,G_IM_SIZ_8b));
    rdp.loaded_texture[0].size_bytes=1024;
    assert(!gfx_texture_cache_lookup(0,&node,texture,G_IM_FMT_CI,G_IM_SIZ_8b));
    rdp.texture_tile.line_size_bytes=32;
    assert(!gfx_texture_cache_lookup(0,&node,texture,G_IM_FMT_CI,G_IM_SIZ_8b));
    for(unsigned i=0;i<20000;i++) {
        const uint8_t *addr=(const uint8_t *)(uintptr_t)(0x18000000+i*32);
        assert(!gfx_texture_cache_lookup(0,&node,addr,G_IM_FMT_RGBA,G_IM_SIZ_16b));
        assert(gfx_texture_cache_lookup(0,&node,addr,G_IM_FMT_RGBA,G_IM_SIZ_16b));
    }
    for(unsigned i=0;i<256;i++) {
        gfx_lookup_or_create_color_combiner((i<<12) | (CC_SHADE<<9));
        assert(color_combiner_pool_size<=64);
    }
    struct ShaderProgram *shader[128];
    for (unsigned i=0;i<128;i++)
        shader[i]=gpu_create_and_load_new_shader(0x01001000+i);
    for (unsigned i=0;i<128;i++) {
        assert(shader[i]->shader_id==0x01001000+i);
        assert(gpu_lookup_shader(0x01001000+i)==shader[i]);
    }
    gpu_shutdown();
}
static void check_matrices(void) {
    float identity[4][4]={{1,0,0,0},{0,1,0,0},{0,0,1,0},{0,0,0,1}};
    float rotate[4][4]={{0,1,0,0},{-1,0,0,0},{0,0,1,0},{0,0,0,1}};
    float translate[4][4]; memcpy(translate,identity,sizeof(identity));
    translate[3][0]=128;
    Vtx vertex={0}; vertex.n.n[0]=127; vertex.v.ob[0]=10; vertex.v.cn[3]=255;
    rsp.modelview_matrix_stack_size=1;
    rsp.current_num_lights=2;
    rsp.current_lights[0]=(Light_t){{200,200,200},0,{200,200,200},0,{127,0,0},0};
    rsp.current_lights[1]=(Light_t){{20,20,20},0,{20,20,20},0,{0,0,0},0};
    rsp.geometry_mode=G_LIGHTING;
    gfx_sp_matrix(G_MTX_PROJECTION|G_MTX_LOAD,(int32_t*)identity);
    gfx_sp_matrix(G_MTX_MODELVIEW|G_MTX_LOAD,(int32_t*)translate);
    gfx_sp_matrix(G_MTX_MODELVIEW|G_MTX_LOAD,(int32_t*)rotate);
    gfx_sp_vertex(1,0,&vertex);
    assert(rsp.loaded_vertices[0].x==0 && rsp.loaded_vertices[0].y==10);
    unsigned shade=rsp.loaded_vertices[0].color.r;
    gfx_sp_matrix(G_MTX_MODELVIEW|G_MTX_LOAD|G_MTX_PUSH,(int32_t*)identity);
    gfx_sp_vertex(1,0,&vertex);
    assert(rsp.loaded_vertices[0].color.r>=219);
    gfx_sp_pop_matrix(1); gfx_sp_vertex(1,0,&vertex);
    assert(rsp.loaded_vertices[0].color.r==shade);
    Light_t changed={{200,200,200},0,{200,200,200},0,{0,127,0},0};
    gfx_sp_movemem(G_MV_LIGHT,48,&changed); gfx_sp_vertex(1,0,&vertex);
    assert(rsp.loaded_vertices[0].color.r!=shade);
    /* Enabling texture generation after an ordinary lit batch must compute
     * the deferred look-at vectors for the current modelview. */
    rsp.geometry_mode|=G_TEXTURE_GEN;
    gfx_sp_vertex(1,0,&vertex);
    struct LoadedVertex generated=rsp.loaded_vertices[0];
    gfx_sp_matrix(G_MTX_MODELVIEW|G_MTX_LOAD,(int32_t*)rotate);
    gfx_sp_vertex(1,0,&vertex);
    assert(generated.u==rsp.loaded_vertices[0].u && generated.v==rsp.loaded_vertices[0].v);
    gfx_sp_pop_matrix(UINT32_MAX);
    assert(rsp.modelview_matrix_stack_size==1);
    gfx_sp_vertex(1,0,&vertex);
}
static void check_render_fixes(void) {
    /* Exhaustive RGBA5551 conversion, including opaque black and the key. */
    for (unsigned c = 0; c < 65536; c++) {
        uint8_t raw[2] = {c>>8,c};
        uint8_t rgba[4] = {SCALE_5_8_GPU(c>>11), SCALE_5_8_GPU((c>>6)&31),
                           SCALE_5_8_GPU((c>>1)&31), (c&1)?255:0};
        unsigned color = tex565_from_rgba16(raw);
        assert(color == tex565_texel(rgba,0));
        assert((color != 0) == ((c&1) != 0));
    }
    const uint8_t black[2] = {0,1};
    assert(tex565_from_rgba16(black) == 1);

    struct GTex tex = {0};
    unsigned f = finishes;
    assert(gpu_tex_slot_begin(&tex,10));
    assert(((uintptr_t)tex.ci8 & 63) == 0 && tex.slotcap[tex.cur] == 64);
    tex.w=5; tex.h=1;
    uint16_t *pixels=(uint16_t *)tex.ci8;
    pixels[0]=0; pixels[1]=1; pixels[2]=0xffff; pixels[3]=0xf800; pixels[4]=0;
    assert(gpu_tex_alpha_mask(&tex));
    uint16_t *mask=(uint16_t *)tex.alpha_mask[tex.cur];
    for(int i=0;i<5;i++) {
        assert(mask[i] == (pixels[i] ? 0 : 0xffff));
        /* Exactly one pass writes each texel, including opaque black. */
        assert((mask[i]!=0)+(pixels[i]!=0)==1);
    }
    tex.slot_epoch[tex.cur]=g_tex_epoch;
    assert(gpu_tex_slot_begin(&tex,10));
    assert(finishes==f);
    assert(gpu_tex_slot_begin(&tex,10));
    assert(finishes==f+1 && tex.alpha_mask[tex.cur]==NULL);
    for(int i=0;i<2;i++) { free(tex.slot[i]); free(tex.alpha_mask[i]); }

    /* RGB shade and independent ENV alpha occupy different shader inputs. */
    gpu_set_zmode_decal(false); gpu_set_depth_mask(true);
    gfx_dp_set_env_color(0,0,0,128);
    gfx_dp_set_combine_mode(color_comb(G_CCMUX_TEXEL0,0,G_CCMUX_SHADE,0),
                            color_comb(0,0,0,G_ACMUX_ENVIRONMENT));
    triangle(0);
    assert(slots[0][6] == rgba_to_565(rsp.loaded_vertices[0].color.r,
                                    rsp.loaded_vertices[0].color.g,
                                    rsp.loaded_vertices[0].color.b));
    assert(current_state.subpix_y==1);
    assert(current_object.ycenter == (int)lrintf(g_cy*16));
    assert(current_object.yscale == (int)lrintf(g_hh*16));

    /* A material fallback carries exact XYZ/W, and both axes retain Q4. */
    struct LoadedVertex v[3]; memcpy(v,rsp.loaded_vertices,sizeof(v));
    v[0].x=17.127f; v[0].y=-34.349f; v[0].w=543.321f; v[0].w_inv=1/v[0].w;
    gfx_flush(); gpu_set_zmode_decal(true);
    gfx_push_triangle(v,v+1,v+2);
    assert(gfx_vc_true[0].valid && gfx_vc_true[0].x[0]==v[0].x);
    gfx_flush();
    int16_t x,y; gpu_project_clip(v[0].x,v[0].y,v[0].w,&x,&y);
    assert(last_x[0]==x && last_y[0]==y && (y&15)!=0);
    assert(!gfx_vc_true[0].valid);
    gpu_set_zmode_decal(false);
    GPU_TEX_FLUSH = 0;
    gfx_dp_set_combine_mode(color_comb(G_CCMUX_TEXEL0,G_CCMUX_SHADE,G_CCMUX_TEXEL0_ALPHA,G_CCMUX_SHADE),
                            color_comb(0,0,0,G_ACMUX_ENVIRONMENT));
    unsigned d=draws;
    triangle(0); gfx_flush();
    assert(draws==d+2);
    assert(last_rgb[0]==0xffff);
    assert(previous_rgb[0]==rgba_to_565(rsp.loaded_vertices[0].color.r,
                                      rsp.loaded_vertices[0].color.g,
                                      rsp.loaded_vertices[0].color.b));
    assert(previous_draw_state.tex_addr != last_draw_state.tex_addr);
    assert(previous_draw_state.flags & OF_GPU_SPAN_SKIP_ZERO);
    assert(last_draw_state.flags & OF_GPU_SPAN_SKIP_ZERO);
    assert(GPU_TEX_FLUSH==1);
    /* When output alpha is the texture alpha, its zero-alpha side stays clear. */
    gfx_dp_set_combine_mode(color_comb(G_CCMUX_TEXEL0,G_CCMUX_SHADE,G_CCMUX_TEXEL0_ALPHA,G_CCMUX_SHADE),
                            color_comb(0,0,0,G_ACMUX_TEXEL0));
    d=draws; triangle(0); gfx_flush();
    assert(draws==d+1);
    free(g_tex[0].alpha_mask[g_tex[0].cur]);
    g_tex[0].alpha_mask[g_tex[0].cur]=NULL;
    puts("renderer fixes: exact black/key conversion, disjoint alpha masks, safe texture reuse, independent color inputs and subpixel fallback passed");
}

static void check_rectangle_vertices(void) {
    gfx_flush();
    gfx_dp_set_combine_mode(color_comb(0,0,0,G_CCMUX_SHADE),
                            color_comb(0,0,0,G_ACMUX_SHADE));
    rdp.other_mode_l=0;
    rdp.other_mode_h=G_CYC_1CYCLE;
    gpu_set_zmode_decal(false);
    for(int cached=0;cached<2;cached++) {
        g_clip_load=cached;
        for(int i=MAX_VERTICES;i<MAX_VERTICES+4;i++) {
            rsp.loaded_vertices[i].w_inv=0;
            rsp.loaded_vertices[i].clip_rej=CLIP_LEFT;
            rsp.loaded_vertices[i].color=(struct RGBA){255,255,255,255};
        }
        unsigned before=draws;
        gfx_draw_rectangle(20*4,30*4,40*4,50*4); gfx_flush();
        assert(draws==before+2);
        for(int i=MAX_VERTICES;i<MAX_VERTICES+4;i++) {
            assert(rsp.loaded_vertices[i].w_inv==1.0f);
            assert(rsp.loaded_vertices[i].clip_rej==0);
        }
        for(int k=0;k<3;k++) {
            assert(last_depth[k]==(1<<30));
            assert(last_x[k]>=19*16 && last_x[k]<=41*16);
            assert(last_y[k]>=29*16 && last_y[k]<=51*16);
        }
    }
    puts("rectangle vertices: cached reciprocal and clip state initialized, both projection paths passed");
}

static void check_decal_occlusion(void) {
    gfx_flush();g_clip_load=0;
    gpu_set_viewport(0,0,320,240);gpu_set_scissor(0,0,320,240);
    rsp.geometry_mode=G_ZBUFFER;
    rdp.other_mode_l=G_RM_AA_ZB_XLU_DECAL | G_RM_AA_ZB_XLU_DECAL2;
    gfx_dp_set_combine_mode(color_comb(0,0,0,G_CCMUX_SHADE),
                            color_comb(0,0,0,G_ACMUX_SHADE));
    const float sx[3]={80,112,80},sy[3]={80,80,112},depth[3]={1000000,1000000,1320000};
    struct LoadedVertex v[3]={0};
    for(int i=0;i<3;i++) {
        v[i].w_inv=depth[i]/1073741824.f;v[i].w=1.f/v[i].w_inv;
        v[i].x=(sx[i]-160.f)/160.f*v[i].w;
        v[i].y=(120.f-sy[i])/120.f*v[i].w;
        v[i].color=(struct RGBA){255,255,255,255};
    }
    gfx_push_triangle(v,v+1,v+2);gfx_flush();
    /* At (84.5,84.5) the receiving floor is at 1,045,000. A nearer
     * character surface at 1,080,000 must occlude its ground shadow. */
    float z=last_depth[0]+(last_depth[1]-last_depth[0])*(4.5f/32.f)
                          +(last_depth[2]-last_depth[0])*(4.5f/32.f);
    assert(z>=1045000.f && z<1080000.f);
    assert(current_state.z_mode==OF_GPU_PARAM_Z_TEST_ZI);
    puts("decal occlusion: sloping ground accepts its shadow while a nearer character occludes it");
}

static void check_mixed_alpha_batch(void) {
    gfx_flush();rsp.geometry_mode=0;
    gfx_dp_set_combine_mode(color_comb(0,0,0,G_CCMUX_ENVIRONMENT),
                            color_comb(0,0,0,G_ACMUX_ENVIRONMENT));
    /* A single texture/shader batch can cross an alpha change. */
    rdp.other_mode_l=G_RM_XLU_SURF | G_RM_XLU_SURF2;
    gfx_dp_set_env_color(255,255,255,255);triangle(0);
    gfx_dp_set_env_color(255,255,255,1);triangle(0);
    assert(buf_vbo_num_tris==2);
    gfx_flush();
    assert(!(previous_draw_state.flags & OF_GPU_SPAN_BLEND));
    assert(last_draw_state.flags & OF_GPU_SPAN_BLEND);
    assert(last_draw_state.const_alpha==1);
}

static void check_menu_rendering(void) {
    gfx_flush();
    gpu_set_viewport(10,140,100,80);
    assert(g_cx==60 && g_cy==60);
    gpu_set_scissor(10,140,100,80);
    assert(g_clip_x0==10 && g_clip_x1==110 && g_clip_y0==20 && g_clip_y1==100);
    gpu_set_scissor(-5,220,20,30);
    assert(g_clip_x0==0 && g_clip_x1==15 && g_clip_y0==0 && g_clip_y1==20);
    gpu_set_viewport(0,0,320,240); gpu_set_scissor(0,0,320,240);
    const uint8_t white[4]={255,255,255,255}, red[4]={255,0,0,255},black[4]={0,0,0,255};
    unsigned f=fills;
    gpu_fill_rect(3,4,13,24,white);
    assert(fills==f+1 && fill_byte==255 && fill_addr==g_draw_fb+(4*320+3)*2);
    assert(fill_width==20 && fill_height==20 && fill_stride==640);
    gpu_fill_rect(0,0,320,240,black);assert(fill_byte==0);
    unsigned d=draws;gpu_fill_rect(3,4,13,24,red);
    assert(draws==d+2 && last_rgb[0]==0xf800 && current_state.z_mode==OF_GPU_PARAM_Z_NONE);
    assert(!(current_state.flags & OF_GPU_SPAN_BLEND));
    assert(!g_st_cache_valid);
    /* Explicit translucent alpha has continuous endpoints. Opaque render
     * modes ignore a zero vertex/material alpha, including non-Z menus. */
    rsp.geometry_mode=0;
    for (int opaque=0;opaque<2;opaque++) for(int alpha=0;alpha<256;alpha++) {
        rdp.other_mode_l=opaque ? (G_RM_AA_OPA_SURF | G_RM_AA_OPA_SURF2)
                                : (G_RM_XLU_SURF | G_RM_XLU_SURF2);
        gfx_dp_set_combine_mode(color_comb(0,0,0,G_CCMUX_ENVIRONMENT),
                                color_comb(0,0,0,G_ACMUX_ENVIRONMENT));
        gfx_dp_set_env_color(255,255,255,alpha);triangle(0);gfx_flush();
        int blend=(current_state.flags & OF_GPU_SPAN_BLEND)!=0;
        assert(blend==(!opaque && alpha<255));
        if(blend) assert(current_state.const_alpha==alpha);
    }
    check_mixed_alpha_batch();
    puts("menus: bottom-left viewport/scissor conversion, RGB565 fills, all fade alpha endpoints and mixed-alpha batches passed");
}

static void check_clip_reciprocal(void) {
    unsigned checks = 0;
    for (uint32_t den = 256; den < (1u << 24); den++) {
        assert(gpu_clip_reciprocal(den) == (uint32_t)((1ull << 32) / den));
        checks++;
    }
    uint32_t seed = 0x98a21f13u;
    for (unsigned i = 0; i < 1000000; i++) {
        seed = seed * 1664525u + 1013904223u;
        uint32_t den = seed < 256 ? 256 : seed;
        assert(gpu_clip_reciprocal(den) == (uint32_t)((1ull << 32) / den));
        checks++;
    }
    assert(gpu_clip_reciprocal(UINT32_MAX) == 1);
    puts("clip reciprocal: exhaustive near-range and randomized full-range division parity passed");
    assert(checks == (1u << 24) - 256 + 1000000);
}

/* The next frame acquires its buffer, and so may overwrite texture slots the
 * last frame sampled, only after that frame's flip fence has retired; the
 * kernel's own acquire wait gives up after ~5 ms. */
static void check_flip_fence_wait(void) {
    gpu_start_frame();
    gfx_gpu_present();
    gpu_lag = 1; fence_reached_value = flip_tokens - 1; fence_lag_polls = 5000;
    unsigned a = acquires;
    gpu_start_frame();
    assert(acquires == a + 1 && acquire_after_fence && fence_lag_polls == 0);
    gfx_gpu_present();                   /* an already retired flip needs no polling */
    fence_reached_value = flip_tokens; fence_lag_polls = 1;
    gpu_start_frame();
    assert(acquires == a + 2 && acquire_after_fence && fence_lag_polls == 1);
    gpu_lag = 0;
    puts("present: the next frame acquires only after the previous flip fence retires");
}

static void regressions(void) {
    check_clip_reciprocal();
    check_state(); check_colors(); check_render_fixes(); check_matrices(); check_textures();
    check_flip_fence_wait();
    puts("renderer: material transitions, shared colors/LOD, matrix/light changes and cache wrap passed");
}
static void bench(void) {
    recording=0;
    for (unsigned j=0;j<1000;j++) triangle(j);
    struct timespec a,b;
    clock_gettime(CLOCK_MONOTONIC,&a);
    for (unsigned j=0;j<3000000;j++) triangle(j);
    gfx_flush();
    clock_gettime(CLOCK_MONOTONIC,&b);
    printf("cached_triangles_ns %.3f\n", ((b.tv_sec-a.tv_sec)*1e9+b.tv_nsec-a.tv_nsec)/3000000.0);
}
int main(int argc,char **argv) {
    init();
    if (argc>1 && !strcmp(argv[1],"bench")) bench();
    else if (argc>1 && !strcmp(argv[1],"regressions")) regressions();
    else if (argc>1 && !strcmp(argv[1],"rectangles")) check_rectangle_vertices();
    else if (argc>1 && !strcmp(argv[1],"menus")) check_menu_rendering();
    else if (argc>1 && !strcmp(argv[1],"decals")) check_decal_occlusion();
    else if (argc>1 && !strcmp(argv[1],"alpha_batches")) check_mixed_alpha_batch();
    else {
        if (argc>1 && !strcmp(argv[1],"palettized")) { g_truecolor=0; g_fb_bpp=1; }
        trace();
    }
    return 0;
}
