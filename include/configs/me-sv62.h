/*
 * Copyright 2024 Mobileye
 */
#ifndef __ME_SV62__
#define __ME_SV62__

#include <configs/s32g3.h>

#ifdef CONFIG_EXTRA_ENV_SETTINGS
#undef CONFIG_EXTRA_ENV_SETTINGS
#endif
#define CONFIG_EXTRA_ENV_SETTINGS \
	S32CC_ENV_SETTINGS \
	"boot_sh=echo Booting SH from eMMC...; " \
		"run load_sh; " \
		"dcache flush; " \
		"startm7 0x34003000; \0" \
	"init_sram=mw 0x4019C004 0; " \
		"mw 0x4019C008 0x1ffff; " \
		"mw 0x4019C000 0x00000001; \0" \
	"load_sh=echo Loading SH00.bin from eMMC...; " \
		"run init_sram;" \
		"fatload mmc 0:1 0x80080000 SH00.bin;" \
		"cp 80080000 34003000 1FD000;\0" \

#ifdef CONFIG_BOOTCOMMAND
#undef CONFIG_BOOTCOMMAND
#endif
#define CONFIG_BOOTCOMMAND \
	"run boot_sh; " \

#define EXTRA_BOOT_ARGS			""
#define FDT_FILE			"s32g3xxa-evb3.dtb"
#endif
