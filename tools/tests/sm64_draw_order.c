/* Synthetic state-sharing display lists; no game assets required. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "game/rendering_graph_node.c"

Gfx *gDisplayListHead;
static struct DisplayListNode allocated[5];
static unsigned allocation_count;

void *alloc_only_pool_alloc(struct AllocOnlyPool *pool, s32 size) {
    (void)pool;
    assert(size == sizeof(struct DisplayListNode));
    assert(allocation_count < 5);
    return &allocated[allocation_count++];
}

void guLookAtReflect(Mtx *m, LookAt *l, float ex, float ey, float ez,
                    float ax, float ay, float az, float ux, float uy, float uz) {
    (void)m; (void)l; (void)ex; (void)ey; (void)ez;
    (void)ax; (void)ay; (void)az; (void)ux; (void)uy; (void)uz;
}

/* Separate nodes establish state and consume it, as articulated models do.
 * The triangle's first index identifies its expected material independently
 * of the node order chosen by the scene-graph renderer. */
static const Gfx blue[] = {
    gsDPSetPrimColor(0, 0, 0, 0, 255, 255), gsSPEndDisplayList(),
};
static const Gfx inherit_blue[] = {
    gsSP1Triangle(0, 4, 5, 0), gsSPEndDisplayList(),
};
static const Gfx red[] = {
    gsDPSetPrimColor(0, 0, 255, 0, 0, 255),
    gsSP1Triangle(1, 4, 5, 0), gsSPEndDisplayList(),
};
static const Gfx inherit_red[] = {
    gsSP1Triangle(2, 4, 5, 0), gsSPEndDisplayList(),
};
static const Gfx white[] = {
    gsDPSetPrimColor(0, 0, 255, 255, 255, 255),
    gsSP1Triangle(3, 4, 5, 0), gsSPEndDisplayList(),
};

static void check_pose(unsigned pose, int layer, int zbuffer) {
    struct GraphNodeMasterList master = {0};
    const Gfx *lists[] = {blue, inherit_blue, red, inherit_red, white};
    const uint32_t expected[] = {0x0000ffff, 0xff0000ff, 0xff0000ff, 0xffffffff};
    Gfx commands[64];
    Mtx matrix = {0};
    master.node.flags = zbuffer ? GRAPH_RENDER_Z_BUFFER : 0;
    allocation_count = 0;
    gDisplayListHead = commands;
    gCurGraphNodeMasterList = &master;
    gMatStackIndex = 0;
    gMatStackFixed[0] = &matrix;
    for (unsigned i = 0; i < 5; i++) {
        /* All 5! relative depths, including parts crossing during animation. */
        unsigned digits = pose;
        unsigned available[] = {0, 1, 2, 3, 4};
        unsigned depth = 0;
        for (unsigned j = 0; j <= i; j++) {
            unsigned pick = digits % (5-j);
            digits /= 5-j;
            depth = available[pick];
            for (unsigned k = pick; k+1 < 5-j; k++) available[k] = available[k+1];
        }
        gMatStack[0][3][2] = -100.0f - depth * 20.0f;
        geo_append_display_list((void *)lists[i], layer);
    }
    geo_process_master_list_sub(&master);
    gCurGraphNodeMasterList = NULL;

    uint32_t color = 0;
    unsigned seen = 0;
    for (const Gfx *cmd = commands; cmd < gDisplayListHead; cmd++) {
        if ((cmd->words.w0 >> 24) != G_DL) continue;
        const Gfx *child = (const Gfx *)(uintptr_t)cmd->words.w1;
        while ((child->words.w0 >> 24) != (uint8_t)G_ENDDL) {
            if ((child->words.w0 >> 24) == G_SETPRIMCOLOR) color = child->words.w1;
            if ((child->words.w0 >> 24) == (uint8_t)G_TRI1) {
                unsigned id = ((child->words.w0 >> 16) & 255) / 2;
                assert(id < 4);
                assert(color == expected[id]);
                seen |= 1u << id;
            }
            child++;
        }
    }
    assert(seen == 15);
}

int main(void) {
    const int layers[] = {LAYER_OPAQUE, LAYER_ALPHA, LAYER_TRANSPARENT};
    for (unsigned pose = 0; pose < 120; pose++)
        for (unsigned layer = 0; layer < 3; layer++)
            for (int z = 0; z < 2; z++) check_pose(pose, layers[layer], z);
    puts("draw order: inherited materials survive 120 depth permutations in opaque, cutout and translucent layers, with and without Z");
    return 0;
}
