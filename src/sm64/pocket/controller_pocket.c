//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * controller_pocket.c -- openfpgaOS controller backend for SM64.
 *
 * Implements ControllerAPI by reading the openfpgaOS input state via
 * of_input.h and mapping it to N64 controller inputs.  (Filename kept for
 * minimal churn; the symbol `controller_pocket` is referenced by
 * controller_entry_point.c.)
 */

#ifdef TARGET_OPENFPGA

#include <stdint.h>

#include <ultra64.h>
#include "controller/controller_api.h"

#include "of.h"
#include "of_input.h"

/* Analog dead zone in of_input's int16 stick range (~7.3%). */
#define DEADZONE 2400

/* Nonzero while the stick vector this frame came from the D-pad rather than a
 * real analog stick.  update_mario_joystick_inputs() reads it to decide whether
 * to apply the digital->analog magnitude ramp. */
s32 gPocketDpadStick = 0;

/* Nonzero while the run modifier is held.  Not an N64 button: it picks the
 * virtual stick's magnitude ceiling (walk vs run) in dpad_ramp_level(). */
s32 gPocketRunHeld = 0;

/* --- Stick shaping -------------------------------------------------------
 *
 * SM64's adjust_analog_stick() (game_init.c) does two things to the raw s8
 * axes before the game sees them:
 *
 *   1. per-axis dead zone -- |raw| < 8 reads as 0, otherwise 6 is subtracted
 *      from the magnitude of that axis;
 *   2. the resulting vector's magnitude is clamped DOWN to 64.  It is never
 *      scaled up, so a short vector stays short.
 *
 * So to request an exact stick magnitude we pre-compensate the 6 on each axis
 * and normalize diagonals ourselves.  (Full-deflection diagonals happened to
 * come out right before only because step 2's clamp caught them; anything
 * short of full deflection would have come out sqrt(2) too fast diagonally.)
 */
#define STICK_DZ    6     /* per-axis offset adjust_analog_stick() removes */
#define STICK_FULL  64    /* post-deadzone magnitude for full deflection */
#define SQRT1_2_Q8  181   /* 1/sqrt(2) in Q8 */

/* Post-deadzone magnitude `mag` along 8-way direction (dx,dy) -> raw s8 axis.
 * Diagonals round up, so a full-deflection diagonal lands just over 64 and the
 * game's clamp brings it back to exactly 64. */
static s8 stick_axis(int dir, int dx, int dy, int mag) {
    if (dir == 0)
        return 0;
    if (dx && dy)
        mag = (mag * SQRT1_2_Q8 + 255) >> 8;
    mag += STICK_DZ;
    return (s8)(dir > 0 ? mag : -mag);
}

static uint32_t isqrt32(uint32_t n) {
    uint32_t x = 0, bit = 1u << 30;
    while (bit > n)
        bit >>= 2;
    while (bit) {
        if (n >= x + bit) {
            n -= x + bit;
            x = (x >> 1) + bit;
        } else {
            x >>= 1;
        }
        bit >>= 2;
    }
    return x;
}

static void controller_of_init(void) {
}

static void controller_of_read(OSContPad *pad) {
    of_input_state_t st;
    of_input_poll();
    of_input_state(0, &st);

    uint32_t b = st.buttons;

    /* Face buttons, on the Pocket's SNES-style diamond (X top, Y left,
     * A right, B bottom):
     *
     *     X = jump          Y = punch / dive / grab
     *     A = run (hold)    B = crouch / ground-pound / long jump
     *
     * The D-pad still walks by default and A raises the ceiling to a run; see
     * dpad_ramp_level() in mario.c.  The shoulders carry C-left/C-right because
     * the camera has to be rotatable and there is no touchscreen to do it on.
     * Nine physical buttons cannot cover the N64's nine functions plus a run
     * modifier, so D_CBUTTONS (the Lakitu/Mario zoom toggle, the least
     * load-bearing C-button) is dropped.  R_TRIG stays unmapped, as before.
     *
     * Note Z+A (long jump) is B+X here -- opposite corners of the diamond,
     * rather than the adjacent pair an X-mounted Z gave.  Docked pads get Z on
     * the triggers as well, which sidesteps it. */
    if (b & OF_BTN_X)      pad->button |= A_BUTTON;     /* jump */
    if (b & OF_BTN_Y)      pad->button |= B_BUTTON;     /* punch / dive / grab */

    if (b & OF_BTN_B)      pad->button |= Z_TRIG;       /* crouch / GP / long jump */
    if (b & OF_BTN_L2)     pad->button |= Z_TRIG;       /* docked pads: trigger Z */
    if (b & OF_BTN_R2)     pad->button |= Z_TRIG;

    if (b & OF_BTN_L1)     pad->button |= L_CBUTTONS;   /* rotate camera left */
    if (b & OF_BTN_R1)     pad->button |= R_CBUTTONS;   /* rotate camera right */
    if (b & OF_BTN_SELECT) pad->button |= U_CBUTTONS;   /* first-person look */

    if (b & OF_BTN_START)  pad->button |= START_BUTTON;

    /* A gates magnitude rather than pressing an N64 button. */
    gPocketRunHeld = (b & OF_BTN_A) ? TRUE : FALSE;

    /* D-pad -> virtual analog stick.  The raw axes are always full deflection;
     * dpad_ramp_level() scales the magnitude down to a walk unless Y is held.
     * A real analog stick (docked pad) takes over when no direction is held,
     * and is never ramped -- it already has a magnitude of its own. */
    int dx = ((b & OF_BTN_RIGHT) ? 1 : 0) - ((b & OF_BTN_LEFT) ? 1 : 0);
    int dy = ((b & OF_BTN_UP)    ? 1 : 0) - ((b & OF_BTN_DOWN) ? 1 : 0);

    if (dx || dy) {
        pad->stick_x = stick_axis(dx, dx, dy, STICK_FULL);
        pad->stick_y = stick_axis(dy, dx, dy, STICK_FULL);
        gPocketDpadStick = TRUE;
        return;
    }
    gPocketDpadStick = FALSE;

    /* Real analog stick: radial dead zone, rescaled so the live range runs the
     * whole way from 0 to full deflection, then pre-compensated per axis the
     * same way the D-pad path is. */
    int lx = st.joy_lx;
    int ly = st.joy_ly;
    uint32_t m = isqrt32((uint32_t)(lx * lx) + (uint32_t)(ly * ly));

    if (m <= DEADZONE) {
        pad->stick_x = 0;
        pad->stick_y = 0;
        return;
    }

    uint32_t mag = (m - DEADZONE) * STICK_FULL / (32767u - DEADZONE);
    if (mag > STICK_FULL)
        mag = STICK_FULL;

    int ax = lx * (int)mag / (int)m;
    int ay = ly * (int)mag / (int)m;
    if (ax > 0) ax += STICK_DZ; else if (ax < 0) ax -= STICK_DZ;
    if (ay > 0) ay += STICK_DZ; else if (ay < 0) ay -= STICK_DZ;

    pad->stick_x = (s8)ax;
    pad->stick_y = (s8)ay;
}

struct ControllerAPI controller_pocket = {
    controller_of_init,
    controller_of_read,
};

#endif /* TARGET_OPENFPGA */
