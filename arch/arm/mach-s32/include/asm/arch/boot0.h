/* SPDX-License-Identifier:     GPL-2.0+ */
/*
 * Copyright 2023 NXP
 */

#ifndef __BOOT0_H
#define __BOOT0_H

	ldr r0, =__bss_start
	ldr r1, =__bss_end
	ldr r2, =0x0
clear_loop:
	str r2, [r0], #4
	cmp r0, r1
	blo    clear_loop

	b reset

#endif /* __BOOT0_H */
