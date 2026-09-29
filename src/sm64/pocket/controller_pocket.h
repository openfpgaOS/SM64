//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

#ifndef CONTROLLER_POCKET_H
#define CONTROLLER_POCKET_H

#include "controller/controller_api.h"

extern struct ControllerAPI controller_pocket;

/* TRUE while the current frame's stick vector was synthesized from the D-pad
 * (as opposed to read from a real analog stick on a docked / SNAC pad). */
extern s32 gPocketDpadStick;

/* TRUE while the run modifier is held (see controller_pocket.c for which
 * physical button that is).  Selects the virtual stick's magnitude ceiling:
 * walk when clear, full deflection when set. */
extern s32 gPocketRunHeld;

#endif
