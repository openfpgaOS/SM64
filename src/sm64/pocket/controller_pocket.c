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

/* Analog dead zone in of_input's int16 stick range. */
#define DEADZONE 2400

static void controller_of_init(void) {
}

static void controller_of_read(OSContPad *pad) {
    of_input_state_t st;
    of_input_poll();
    of_input_state(0, &st);

    uint32_t b = st.buttons;

    /* DSi-style mapping: D-pad drives Mario's analog movement; the face
     * buttons + shoulders are the four C-buttons (camera). */
    if (b & OF_BTN_A)      pad->button |= A_BUTTON;     /* jump */
    if (b & OF_BTN_B)      pad->button |= B_BUTTON;     /* punch / dive / grab */

    /* X / Y / L / R -> C-buttons (camera). */
    if (b & OF_BTN_Y)      pad->button |= U_CBUTTONS;
    if (b & OF_BTN_X)      pad->button |= D_CBUTTONS;
    if (b & OF_BTN_L1)     pad->button |= L_CBUTTONS;
    if (b & OF_BTN_R1)     pad->button |= R_CBUTTONS;

    /* Z (crouch / ground-pound / long-jump / backflip) — essential to SM64 and
     * not in the chosen face-button scheme, so it lives on the free buttons. */
    if (b & OF_BTN_SELECT) pad->button |= Z_TRIG;
    if (b & OF_BTN_L2)     pad->button |= Z_TRIG;
    if (b & OF_BTN_R2)     pad->button |= Z_TRIG;

    if (b & OF_BTN_START)  pad->button |= START_BUTTON;

    /* D-pad -> analog stick (full deflection = run).  If a real analog stick is
     * present (external pad), it takes over when no D-pad direction is held. */
    int sx = 0, sy = 0;
    if (b & OF_BTN_LEFT)   sx = -80;
    if (b & OF_BTN_RIGHT)  sx =  80;
    if (b & OF_BTN_DOWN)   sy = -80;
    if (b & OF_BTN_UP)     sy =  80;

    if (sx == 0 && sy == 0) {
        int lx = st.joy_lx;
        int ly = st.joy_ly;
        if (lx > -DEADZONE && lx < DEADZONE) lx = 0;
        if (ly > -DEADZONE && ly < DEADZONE) ly = 0;
        sx = lx * 80 / 32768;
        sy = ly * 80 / 32768;
        if (sx >  80) sx =  80; else if (sx < -80) sx = -80;
        if (sy >  80) sy =  80; else if (sy < -80) sy = -80;
    }

    pad->stick_x = (s8)sx;
    pad->stick_y = (s8)sy;
}

struct ControllerAPI controller_pocket = {
    controller_of_init,
    controller_of_read,
};

#endif /* TARGET_OPENFPGA */
