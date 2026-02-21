/*
 * controller_pocket.c -- Analogue Pocket controller backend for SM64
 *
 * Implements ControllerAPI, reading the Pocket's buttons and analog stick
 * via MMIO registers and mapping them to N64 controller inputs.
 */

#ifdef TARGET_POCKET

#include <stdint.h>

#include <ultra64.h>
#include "controller/controller_api.h"

/* Controller MMIO registers */
#define CONT1_KEY   (*(volatile uint32_t *)0x40000050)
#define CONT1_JOY   (*(volatile uint32_t *)0x40000054)
#define CONT1_TRIG  (*(volatile uint32_t *)0x40000058)

/* Pocket key bitmap bits */
#define KEY_DPAD_UP     (1 << 0)
#define KEY_DPAD_DOWN   (1 << 1)
#define KEY_DPAD_LEFT   (1 << 2)
#define KEY_DPAD_RIGHT  (1 << 3)
#define KEY_FACE_A      (1 << 4)
#define KEY_FACE_B      (1 << 5)
#define KEY_FACE_X      (1 << 6)
#define KEY_FACE_Y      (1 << 7)
#define KEY_TRIG_L1     (1 << 8)
#define KEY_TRIG_R1     (1 << 9)
#define KEY_TRIG_L2     (1 << 10)
#define KEY_TRIG_R2     (1 << 11)
#define KEY_SELECT      (1 << 14)
#define KEY_START       (1 << 15)

/* Analog stick dead zone */
#define DEADZONE 10

static void controller_pocket_init(void) {
    /* Nothing to initialize */
}

static void controller_pocket_read(OSContPad *pad) {
    uint32_t keys = CONT1_KEY;
    uint32_t joy = CONT1_JOY;

    /* Map Pocket buttons to N64 buttons.
     * Pocket layout:       N64 mapping:
     *   A (right face)  -> A_BUTTON  (jump)
     *   B (bottom face) -> B_BUTTON  (attack/dive)
     *   X (top face)    -> B_BUTTON  (alternate attack)
     *   Y (left face)   -> A_BUTTON  (alternate jump)
     *   L1              -> Z_TRIG    (crouch/ground pound)
     *   R1              -> R_TRIG    (camera)
     *   L2              -> L_TRIG    (unused in SM64, but mapped)
     *   R2              -> Z_TRIG    (alternate crouch)
     *   Start           -> START
     *   Select          -> R_TRIG    (camera toggle)
     *   D-pad           -> C buttons (camera control)
     */

    if (keys & KEY_FACE_A)      pad->button |= A_BUTTON;
    if (keys & KEY_FACE_B)      pad->button |= B_BUTTON;
    if (keys & KEY_FACE_X)      pad->button |= B_BUTTON;
    if (keys & KEY_FACE_Y)      pad->button |= A_BUTTON;
    if (keys & KEY_TRIG_L1)     pad->button |= Z_TRIG;
    if (keys & KEY_TRIG_R1)     pad->button |= R_TRIG;
    if (keys & KEY_TRIG_L2)     pad->button |= L_TRIG;
    if (keys & KEY_TRIG_R2)     pad->button |= Z_TRIG;
    if (keys & KEY_START)       pad->button |= START_BUTTON;
    if (keys & KEY_SELECT)      pad->button |= R_TRIG;

    /* D-pad -> C buttons for camera control */
    if (keys & KEY_DPAD_UP)     pad->button |= U_CBUTTONS;
    if (keys & KEY_DPAD_DOWN)   pad->button |= D_CBUTTONS;
    if (keys & KEY_DPAD_LEFT)   pad->button |= L_CBUTTONS;
    if (keys & KEY_DPAD_RIGHT)  pad->button |= R_CBUTTONS;

    /* Analog stick: CONT1_JOY provides {x[15:0], y[15:0]} as signed 16-bit.
     * The Pocket analog range is roughly -128..+127.
     * N64 stick range is -80..+80. Scale and apply dead zone. */
    int16_t raw_x = (int16_t)(joy & 0xFFFF);
    int16_t raw_y = (int16_t)(joy >> 16);

    /* Apply dead zone */
    if (raw_x > -DEADZONE && raw_x < DEADZONE) raw_x = 0;
    if (raw_y > -DEADZONE && raw_y < DEADZONE) raw_y = 0;

    /* Scale from Pocket range (~-128..127) to N64 range (-80..80) */
    int sx = (raw_x * 80) / 128;
    int sy = (raw_y * 80) / 128;

    /* Clamp */
    if (sx > 80) sx = 80;
    if (sx < -80) sx = -80;
    if (sy > 80) sy = 80;
    if (sy < -80) sy = -80;

    pad->stick_x = (s8)sx;
    pad->stick_y = (s8)sy;
}

struct ControllerAPI controller_pocket = {
    controller_pocket_init,
    controller_pocket_read,
};

#endif /* TARGET_POCKET */
