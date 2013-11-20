/*
 * Jailhouse, a Linux-based partitioning hypervisor
 *
 * Copyright (c) Siemens AG, 2013
 *
 * Authors:
 *  Jan Kiszka <jan.kiszka@siemens.com>
 *
 * This work is licensed under the terms of the GNU GPL, version 2.  See
 * the COPYING file in the top-level directory.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/firmware.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/uaccess.h>
#include <asm/smp.h>
#include <asm/cacheflush.h>

#include "jailhouse.h"
#include <jailhouse/header.h>
#include <jailhouse/hypercall.h>

#define JAILHOUSE_FW_NAME	"jailhouse.bin"

MODULE_DESCRIPTION("Loader for Jailhouse partitioning hypervisor");
MODULE_LICENSE("GPL");
MODULE_FIRMWARE(JAILHOUSE_FW_NAME);

static struct device *jailhouse_dev;
static DEFINE_MUTEX(lock);
static bool enabled;
static void *hypervisor_mem;
static cpumask_t offlined_cpus;
static atomic_t call_done;
static int error_code;

#ifdef CONFIG_X86

static void *jailhouse_ioremap(phys_addr_t start, unsigned long size)
{
	void *addr;

	addr = (__force void *)ioremap_cache(start, size);
	if (addr)
		set_memory_x((unsigned long)addr, size / PAGE_SIZE);
	return addr;
}

#elif defined(CONFIG_ARM)

#include <asm/mach/map.h>

static void *jailhouse_ioremap(phys_addr_t start, unsigned long size)
{
	return (__force void *)__arm_ioremap(start, size, MT_MEMORY);
}

#else
#error Unsupported architecture
#endif

static void enter_hypervisor(void *info)
{
	struct jailhouse_header *header = info;
	entry_func entry;
	int err;

	entry = (entry_func)(hypervisor_mem + header->entry);

	/* either returns 0 or the same error code across all CPUs */
	err = entry(smp_processor_id());
	if (err)
		error_code = err;

	atomic_inc(&call_done);
}

static int jailhouse_enable(struct jailhouse_system __user *arg)
{
	unsigned long hv_core_size, percpu_size, config_size;
	const struct firmware *hypervisor;
	struct jailhouse_system config_header;
	struct jailhouse_memory *hv_mem = &config_header.hypervisor_memory;
	struct jailhouse_header *header;
	int err;

	if (copy_from_user(&config_header, arg, sizeof(config_header)))
		return -EFAULT;

	if (mutex_lock_interruptible(&lock) != 0)
		return -EINTR;

	err = -EBUSY;
	if (enabled || !try_module_get(THIS_MODULE))
		goto error_unlock;

	err = request_firmware(&hypervisor, JAILHOUSE_FW_NAME, jailhouse_dev);
	if (err)
		goto error_put_module;

	header = (struct jailhouse_header *)hypervisor->data;

	err = -EINVAL;
	if (memcmp(header->signature, JAILHOUSE_SIGNATURE,
		   sizeof(header->signature)) != 0)
		goto error_release_fw;

	hv_core_size = PAGE_ALIGN(header->bss_end);
	percpu_size = num_possible_cpus() * header->percpu_size;
	config_size = jailhouse_system_config_size(&config_header);
	if (hv_mem->size <= hv_core_size + percpu_size + config_size)
		goto error_release_fw;

	/* CMA would be better... */
	hypervisor_mem = jailhouse_ioremap(hv_mem->phys_start, hv_mem->size);
	if (!hypervisor_mem)
		goto error_release_fw;

	memcpy(hypervisor_mem, hypervisor->data, hypervisor->size);
	memset(hypervisor_mem + hypervisor->size, 0,
	       hv_mem->size - hypervisor->size);

	header = (struct jailhouse_header *)hypervisor_mem;
	header->size = hv_mem->size;
	header->page_offset =
		(unsigned long)hypervisor_mem - hv_mem->phys_start;
	header->possible_cpus = num_possible_cpus();

	if (copy_from_user(hypervisor_mem + hv_core_size + percpu_size, arg,
			   config_size)) {
		err = -EFAULT;
		goto error_unmap;
	}

	error_code = 0;

	preempt_disable();

	header->online_cpus = num_online_cpus();

	atomic_set(&call_done, 0);
	on_each_cpu(enter_hypervisor, header, 0);
	while (atomic_read(&call_done) != num_online_cpus())
		cpu_relax();

	preempt_enable();

	if (error_code) {
		err = error_code;
		goto error_unmap;
	}

	release_firmware(hypervisor);

	enabled = true;

	mutex_unlock(&lock);

	printk("The Jailhouse is opening.\n");

	return 0;

error_unmap:
	iounmap((__force void __iomem *)hypervisor_mem);

error_release_fw:
	release_firmware(hypervisor);

error_put_module:
	module_put(THIS_MODULE);

error_unlock:
	mutex_unlock(&lock);
	return err;
}

static void leave_hypervisor(void *info)
{
	int err;

	/* either returns 0 or the same error code across all CPUs */
	err = jailhouse_call0(JAILHOUSE_HC_DISABLE);
	if (err)
		error_code = err;

	atomic_inc(&call_done);
}

static int jailhouse_disable(void)
{
	unsigned int cpu;
	int err;

	if (mutex_lock_interruptible(&lock) != 0)
		return -EINTR;

	if (!enabled) {
		mutex_unlock(&lock);
		return -EINVAL;
	}

	error_code = 0;

	preempt_disable();

	atomic_set(&call_done, 0);
	on_each_cpu(leave_hypervisor, NULL, 0);
	while (atomic_read(&call_done) != num_online_cpus())
		cpu_relax();

	preempt_enable();

	err = error_code;
	if (err)
		goto unlock_out;

	iounmap((__force void __iomem *)hypervisor_mem);

	for_each_cpu_mask(cpu, offlined_cpus)
		if (cpu_up(cpu) != 0)
			printk("Jailhouse: failed to bring CPU %d back "
			       "online\n", cpu);

	enabled = false;
	module_put(THIS_MODULE);

	printk("The Jailhouse was closed.\n");

unlock_out:
	mutex_unlock(&lock);

	return err;
}

static int jailhouse_cell_create(struct jailhouse_new_cell __user *arg)
{
	struct {
		struct jailhouse_new_cell cell;
		struct jailhouse_preload_image image;
	} cell_buffer;
	struct jailhouse_new_cell *cell = &cell_buffer.cell;
	struct jailhouse_preload_image *image = &cell->image[0];
	unsigned int mask_pos, bit_pos, cpu;
	struct jailhouse_cell_desc *config;
	struct jailhouse_memory *ram;
	void *cell_mem;
	u8 *cpu_mask;
	int err;

	if (copy_from_user(cell, arg, sizeof(*cell)))
		return -EFAULT;

	if (cell->num_preload_images != 1)
		return -EINVAL;

	if (copy_from_user(cell->image, arg->image,
			   sizeof(*cell->image) * cell->num_preload_images))
		return -EFAULT;

	config = kmalloc(cell->config_size, GFP_KERNEL | GFP_DMA);
	if (!config)
		return -ENOMEM;

	if (copy_from_user(config, (void *)(unsigned long)cell->config_address,
			   cell->config_size)) {
		err = -EFAULT;
		goto kfree_config_out;
	}
	config->name[JAILHOUSE_CELL_NAME_MAXLEN] = 0;

	cpu_mask = ((void *)config) + sizeof(struct jailhouse_cell_desc);
	for (mask_pos = 0; mask_pos < config->cpu_set_size; mask_pos++)
		for (bit_pos = 0; bit_pos < 8; bit_pos++) {
			if (!(cpu_mask[mask_pos] & (1 << bit_pos)))
				continue;
			cpu = mask_pos * 8 + bit_pos;
			if (cpu_online(cpu)) {
				err = cpu_down(cpu);
				if (err)
					goto kfree_config_out;
				cpu_set(cpu, offlined_cpus);
			}
		}

	ram = ((void *)config) + sizeof(struct jailhouse_cell_desc) +
		config->cpu_set_size;
	if (config->num_memory_regions < 1 || ram->size < 1024 * 1024 ||
	    image->target_address + image->size > ram->size) {
		err = -EINVAL;
		goto kfree_config_out;
	}

	cell_mem = jailhouse_ioremap(ram->phys_start, ram->size);
	if (!cell_mem) {
		err = -EBUSY;
		goto kfree_config_out;
	}
	memset(cell_mem, 0, ram->size);

	if (copy_from_user(cell_mem + image->target_address,
			   (void *)(unsigned long)image->source_address,
			   image->size)) {
		err = -EFAULT;
		goto iounmap_out;
	}

	if (mutex_lock_interruptible(&lock) != 0) {
		err = -EINTR;
		goto kfree_config_out;
	}

	if (!enabled) {
		err = -EINVAL;
		goto unlock_out;
	}

	err = jailhouse_call1(JAILHOUSE_HC_CELL_CREATE, __pa(config));
	if (err)
		goto unlock_out;

	printk("Created Jailhouse cell \"%s\"\n", config->name);

unlock_out:
	mutex_unlock(&lock);

iounmap_out:
	iounmap((__force void __iomem *)cell_mem);
kfree_config_out:
	kfree(config);

	return err;
}

static long jailhouse_ioctl(struct file *file, unsigned int ioctl,
			    unsigned long arg)
{
	long err;

	switch (ioctl) {
	case JAILHOUSE_ENABLE:
		err = jailhouse_enable(
			(struct jailhouse_system __user *)arg);
		break;
	case JAILHOUSE_DISABLE:
		err = jailhouse_disable();
		break;
	case JAILHOUSE_CELL_CREATE:
		err = jailhouse_cell_create(
			(struct jailhouse_new_cell __user *)arg);
		break;
	case JAILHOUSE_CELL_DESTROY:
		err = -ENOSYS;
		break;
	default:
		err = -EINVAL;
		break;
	}

	return err;
}

static const struct file_operations jailhouse_fops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = jailhouse_ioctl,
	.llseek = noop_llseek,
};

static struct miscdevice jailhouse_misc_dev = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "jailhouse",
	.fops = &jailhouse_fops,
};

static int __init jailhouse_init(void)
{
	jailhouse_dev = root_device_register("jailhouse");
	if (IS_ERR(jailhouse_dev))
		return PTR_ERR(jailhouse_dev);
	return misc_register(&jailhouse_misc_dev);
}

static void __exit jailhouse_exit(void)
{
	misc_deregister(&jailhouse_misc_dev);
	root_device_unregister(jailhouse_dev);
}

module_init(jailhouse_init);
module_exit(jailhouse_exit);
