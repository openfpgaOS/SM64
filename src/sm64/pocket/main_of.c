//------------------------------------------------------------------------------
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileType: SOURCE
// SPDX-FileCopyrightText: (c) 2026, ThinkElastic <Think@Elastic.com>
//------------------------------------------------------------------------------

/*
 * main_of.c -- openfpgaOS entry point for SM64.
 *
 * The openfpgaOS ELF loader sets up the standard SysV stack and calls
 * main() via musl's crt1.o.  We just hand control to the SM64 PC-port
 * driver (main_func in pc_main.c), which selects the openfpgaOS
 * rendering / window-manager / audio backends and runs the game loop.
 */

extern void main_func(void);

int main(void) {
    main_func();
    return 0;
}
