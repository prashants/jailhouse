/*
 * Jailhouse, a Linux-based partitioning hypervisor
 *
 * Minimal configuration for demo inmates, 1 CPU, 1 MB RAM, 1 serial port
 *
 * Copyright (c) Siemens AG, 2013
 *
 * Authors:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <linux/types.h>
#include <jailhouse/cell-config.h>

#define ALIGN __attribute__((aligned(1)))
#define ARRAY_SIZE(a) sizeof(a) / sizeof(a[0])

struct {
	struct jailhouse_cell_desc ALIGN cell;
	__u64 ALIGN cpus[1];
	struct jailhouse_memory ALIGN mem_regions[1];
	__u8 ALIGN pio_bitmap[0x2000];
} ALIGN config = {
	.cell = {
		.name = "Minimal",

		.cpu_set_size = sizeof(config.cpus),
		.num_memory_regions = ARRAY_SIZE(config.mem_regions),
		.num_irq_lines = 0,
		.pio_bitmap_size = ARRAY_SIZE(config.pio_bitmap),

		.num_pci_devices = 0,
	},

	.cpus = {
		0x8,
	},

	.mem_regions = {
		/* RAM */ {
			.phys_start = 0x3bf00000,
			.virt_start = 0,
			.size = 0x00100000,
			.access_flags = JAILHOUSE_MEM_READ |
				JAILHOUSE_MEM_WRITE | JAILHOUSE_MEM_EXECUTE,
		},
	},

	.pio_bitmap = {
		[     0/8 ...  0x3f7/8] = -1,
		[ 0x3f8/8 ...  0x3ff/8] = 0, /* serial1 */
		[ 0x400/8 ...  0x407/8] = -1,
		[ 0x408/8 ...  0x40f/8] = 0xf0, /* PM-timer H700 */
		[ 0x410/8 ... 0x1807/8] = -1,
		[0x1808/8 ... 0x180f/8] = 0xf0, /* PM-timer H87I-PLUS */
		[0x1810/8 ... 0xb007/8] = -1,
		[0xb008/8 ... 0xb00f/8] = 0xf0, /* PM-timer QEMU */
		[0xb010/8 ... 0xe00f/8] = -1,
		[0xe010/8 ... 0xe017/8] = 0, /* OXPCIe952 serial1 */
		[0xe018/8 ... 0xffff/8] = -1,
	},
};
