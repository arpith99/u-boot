// SPDX-License-Identifier: GPL-2.0+
/*
 * Copyright 2022 NXP
 */
#include <common.h>
#include <debug_uart.h>
#include <asm/global_data.h>
#include <asm/sections.h>
#include <asm/armv8/mmu.h>

DECLARE_GLOBAL_DATA_PTR;

#ifndef CONFIG_SYS_DCACHE_OFF
#define PERIPH_BASE      0x40000000
#define PERIPH_SIZE      0x20000000
#endif

#ifndef CONFIG_SYS_DCACHE_OFF
static struct mm_region s32_mem_map[] = {
	{
		PHYS_SDRAM_1, PHYS_SDRAM_1, PHYS_SDRAM_1_SIZE,
		PTE_BLOCK_MEMTYPE(MT_NORMAL) | PTE_BLOCK_OUTER_SHARE |
		    PTE_BLOCK_NS
	},
#ifdef PHYS_SDRAM_2
	{
		PHYS_SDRAM_2, PHYS_SDRAM_2, PHYS_SDRAM_2_SIZE,
		PTE_BLOCK_MEMTYPE(MT_NORMAL) | PTE_BLOCK_OUTER_SHARE |
		    PTE_BLOCK_NS
	},
#endif
	{
		PERIPH_BASE, PERIPH_BASE, PERIPH_SIZE,
		PTE_BLOCK_MEMTYPE(MT_DEVICE_NGNRNE) | PTE_BLOCK_NON_SHARE |
		    PTE_BLOCK_PXN | PTE_BLOCK_UXN
	},
	{
		CONFIG_SYS_FLASH_BASE, CONFIG_SYS_FLASH_BASE,
		CONFIG_SYS_FLASH_SIZE,
		PTE_BLOCK_MEMTYPE(MT_NORMAL) | PTE_BLOCK_OUTER_SHARE
	},
	/* list terminator */
	{},
};

struct mm_region *mem_map = s32_mem_map;

static void disable_qspi_mmu_entry(void)
{
	struct mm_region *region;
	int offset;
	size_t i;

	offset = fdt_node_offset_by_compatible(gd->fdt_blob, -1,
					       "nxp,s32cc-qspi");
	if (offset > 0) {
		if (fdtdec_get_is_enabled(gd->fdt_blob, offset))
			return;
	}

	/* Skip AHB mapping by setting its size to 0 */
	for (i = 0; i < ARRAY_SIZE(s32_mem_map); i++) {
		region = &s32_mem_map[i];
		if (region->phys == CONFIG_SYS_FLASH_BASE) {
			region->size = 0U;
			break;
		}
	}
}
#else /* CONFIG_SYS_DCACHE_OFF */
static void disable_qspi_mmu_entry(void)
{
}
#endif

int arch_cpu_init(void)
{
	/* Clear the BSS. */
	memset(__bss_start, 0, __bss_end - __bss_start);

	disable_qspi_mmu_entry();

	gd->flags |= GD_FLG_SKIP_RELOC;

	if (IS_ENABLED(CONFIG_DEBUG_UART))
		debug_uart_init();

	return 0;
}

