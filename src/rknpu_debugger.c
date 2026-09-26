// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) Rockchip Electronics Co., Ltd.
 * Author: Felix Zeng <felix.zeng@rock-chips.com>
 * Author: lurenJBD <31967654+lurenJBD@users.noreply.github.com>
 */

#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/syscalls.h>
#include <linux/debugfs.h>
#include <linux/proc_fs.h>
#include <linux/devfreq.h>
#include <linux/clk.h>
#include <asm/div64.h>

#include "rknpu_drv.h"
#include "rknpu_core.h"
#include "rknpu_mm.h"
#include "rknpu_reset.h"
#include "rknpu_debugger.h"
#include "rknpu_devfreq.h"
#include "rknpu_gem.h"

#define RKNPU_DEBUGGER_ROOT_NAME "rknpu"
static int rknpu_version_show(struct seq_file *m, void *data)
{
	seq_printf(m, "%s: v%d.%d.%d\n", DRIVER_DESC, DRIVER_MAJOR,
		   DRIVER_MINOR, DRIVER_PATCHLEVEL);

	return 0;
}

static int rknpu_load_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node = m->private;
	struct rknpu_debugger *debugger = node->debugger;
	struct rknpu_device *rknpu_dev =
		container_of(debugger, struct rknpu_device, debugger);
	struct rknpu_subcore_data *subcore_data = NULL;
	unsigned long flags;
	int i;
	int load;
	uint64_t total_busy_time, div_value;

	seq_puts(m, "NPU load: ");
	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		subcore_data = &rknpu_dev->subcore_datas[i];

		if (rknpu_dev->config->num_irqs > 1)
			seq_printf(m, " Core%d: ", i);

		spin_lock_irqsave(&rknpu_dev->irq_lock, flags);

		total_busy_time = subcore_data->timer.total_busy_time;

		spin_unlock_irqrestore(&rknpu_dev->irq_lock, flags);

		div_value = (RKNPU_LOAD_INTERVAL / 100);
		do_div(total_busy_time, div_value);
		load = total_busy_time > 100 ? 100 : total_busy_time;

		if (rknpu_dev->config->num_irqs > 1)
			seq_printf(m, "%2.d%%,", load);
		else
			seq_printf(m, "%2.d%%", load);
	}
	seq_puts(m, "\n");

	return 0;
}

static int rknpu_power_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node = m->private;
	struct rknpu_debugger *debugger = node->debugger;
	struct rknpu_device *rknpu_dev =
		container_of(debugger, struct rknpu_device, debugger);

	if (atomic_read(&rknpu_dev->power_refcount) > 0)
		seq_puts(m, "on\n");
	else
		seq_puts(m, "off\n");

	return 0;
}

/*
 * Read-only dump of the PC-domain registers the submit path programs.
 * Only readable when the NPU power domain is active.
 */
static int rknpu_regs_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node = m->private;
	struct rknpu_debugger *debugger = node->debugger;
	struct rknpu_device *rknpu_dev =
		container_of(debugger, struct rknpu_device, debugger);
	unsigned int i;

	if (atomic_read(&rknpu_dev->power_refcount) <= 0) {
		seq_puts(m,
			 "N/A: NPU is powered off (power_refcount == 0).\n"
			 "Write 'on' to /sys/kernel/debug/rknpu/power first, and\n"
			 "only while no job is in flight.\n");
		return 0;
	}

	for (i = 0; i < rknpu_dev->config->num_irqs; i++) {
		if (!rknpu_dev->base[i]) {
			seq_printf(m, "core %u: not mapped\n", i);
			continue;
		}

		seq_printf(m,
			   "core %u: ver=%#x ver_num=%#x\n"
			   "  op_en=%#x data_addr=%#x data_amount=%#x\n"
			   "  int_mask=%#x int_clear=%#x int_status=%#x int_raw_status=%#x\n"
			   "  task_control=%#x dma_base=%#x task_status=%#x\n",
			   i,
			   rknpu_core_read(rknpu_dev, i,
					   RKNPU_CORE_REG_VERSION),
			   rknpu_core_read(rknpu_dev, i,
					   RKNPU_CORE_REG_VERSION_NUM),
			   rknpu_core_read(rknpu_dev, i,
					   RKNPU_CORE_REG_PC_OP_EN),
			   rknpu_core_read(rknpu_dev, i,
					   RKNPU_CORE_REG_PC_DATA_ADDR),
			   rknpu_core_read(rknpu_dev, i,
					   RKNPU_CORE_REG_PC_DATA_AMOUNT),
			   rknpu_core_read(rknpu_dev, i,
					   RKNPU_CORE_REG_INT_MASK),
			   rknpu_core_read(rknpu_dev, i,
					   RKNPU_CORE_REG_INT_CLEAR),
			   rknpu_core_read(rknpu_dev, i,
					   RKNPU_CORE_REG_INT_STATUS),
			   rknpu_core_read(rknpu_dev, i,
					   RKNPU_CORE_REG_INT_RAW_STATUS),
			   rknpu_core_read(rknpu_dev, i,
					   RKNPU_CORE_REG_PC_TASK_CONTROL),
			   rknpu_core_read(rknpu_dev, i,
					   RKNPU_CORE_REG_PC_DMA_BASE_ADDR),
			   rknpu_core_read_dynamic(
				   rknpu_dev, i,
				   rknpu_dev->config->pc_task_status_offset));

		/* Read block-level execution status and operation enable registers. */
		seq_printf(m,
			   "  CNA s_status=%#x s_pointer=%#x op_en=%#x\n"
			   "  CORE s_status=%#x s_pointer=%#x op_en=%#x\n"
			   "  DPU s_status=%#x s_pointer=%#x op_en=%#x\n"
			   "  DPU_RDMA s_status=%#x s_pointer=%#x op_en=%#x\n"
			   "  PPU s_status=%#x s_pointer=%#x op_en=%#x\n"
			   "  PPU_RDMA s_status=%#x s_pointer=%#x op_en=%#x\n"
			   "  GLOBAL op_en=%#x\n",
			   rknpu_core_read_block(
				   rknpu_dev, i,
				   RKNPU_CORE_BLOCK_CNA_S_STATUS),
			   rknpu_core_read_block(
				   rknpu_dev, i,
				   RKNPU_CORE_BLOCK_CNA_S_POINTER),
			   rknpu_core_read_block(
				   rknpu_dev, i, RKNPU_CORE_BLOCK_CNA_OP_EN),
			   rknpu_core_read_block(
				   rknpu_dev, i,
				   RKNPU_CORE_BLOCK_CORE_S_STATUS),
			   rknpu_core_read_block(
				   rknpu_dev, i,
				   RKNPU_CORE_BLOCK_CORE_S_POINTER),
			   rknpu_core_read_block(
				   rknpu_dev, i,
				   RKNPU_CORE_BLOCK_CORE_OP_EN),
			   rknpu_core_read_block(
				   rknpu_dev, i,
				   RKNPU_CORE_BLOCK_DPU_S_STATUS),
			   rknpu_core_read_block(
				   rknpu_dev, i,
				   RKNPU_CORE_BLOCK_DPU_S_POINTER),
			   rknpu_core_read_block(
				   rknpu_dev, i, RKNPU_CORE_BLOCK_DPU_OP_EN),
			   rknpu_core_read_block(
				   rknpu_dev, i,
				   RKNPU_CORE_BLOCK_DPU_RDMA_S_STATUS),
			   rknpu_core_read_block(
				   rknpu_dev, i,
				   RKNPU_CORE_BLOCK_DPU_RDMA_S_POINTER),
			   rknpu_core_read_block(
				   rknpu_dev, i,
				   RKNPU_CORE_BLOCK_DPU_RDMA_OP_EN),
			   rknpu_core_read_block(
				   rknpu_dev, i,
				   RKNPU_CORE_BLOCK_PPU_S_STATUS),
			   rknpu_core_read_block(
				   rknpu_dev, i,
				   RKNPU_CORE_BLOCK_PPU_S_POINTER),
			   rknpu_core_read_block(
				   rknpu_dev, i, RKNPU_CORE_BLOCK_PPU_OP_EN),
			   rknpu_core_read_block(
				   rknpu_dev, i,
				   RKNPU_CORE_BLOCK_PPU_RDMA_S_STATUS),
			   rknpu_core_read_block(
				   rknpu_dev, i,
				   RKNPU_CORE_BLOCK_PPU_RDMA_S_POINTER),
			   rknpu_core_read_block(
				   rknpu_dev, i,
				   RKNPU_CORE_BLOCK_PPU_RDMA_OP_EN),
			   rknpu_core_read_block(
				   rknpu_dev, i,
				   RKNPU_CORE_BLOCK_GLOBAL_OP_EN));
	}

	return 0;
}

static ssize_t rknpu_power_set(struct file *file, const char __user *ubuf,
			       size_t len, loff_t *offp)
{
	struct seq_file *priv = file->private_data;
	struct rknpu_debugger_node *node = priv->private;
	struct rknpu_debugger *debugger = node->debugger;
	struct rknpu_device *rknpu_dev =
		container_of(debugger, struct rknpu_device, debugger);
	char buf[8];

	if (len > sizeof(buf) - 1)
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len - 1] = '\0';

	if (strcmp(buf, "on") == 0) {
		atomic_inc(&rknpu_dev->cmdline_power_refcount);
		rknpu_power_get(rknpu_dev);
		LOG_INFO("rknpu power is on!");
	} else if (strcmp(buf, "off") == 0) {
		if (atomic_read(&rknpu_dev->power_refcount) > 0 &&
		    atomic_dec_if_positive(
			    &rknpu_dev->cmdline_power_refcount) >= 0) {
			atomic_sub(
				atomic_read(&rknpu_dev->cmdline_power_refcount),
				&rknpu_dev->power_refcount);
			atomic_set(&rknpu_dev->cmdline_power_refcount, 0);
			rknpu_power_put(rknpu_dev);
		}
		if (atomic_read(&rknpu_dev->power_refcount) <= 0)
			LOG_INFO("rknpu power is off!");
	} else {
		LOG_ERROR("rknpu power node params is invalid!");
	}

	return len;
}

static int rknpu_power_put_delay_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node = m->private;
	struct rknpu_debugger *debugger = node->debugger;
	struct rknpu_device *rknpu_dev =
		container_of(debugger, struct rknpu_device, debugger);

	seq_printf(m, "%lu\n", rknpu_dev->power_put_delay);

	return 0;
}

static ssize_t rknpu_power_put_delay_set(struct file *file,
					 const char __user *ubuf, size_t len,
					 loff_t *offp)
{
	struct seq_file *priv = file->private_data;
	struct rknpu_debugger_node *node = priv->private;
	struct rknpu_debugger *debugger = node->debugger;
	struct rknpu_device *rknpu_dev =
		container_of(debugger, struct rknpu_device, debugger);
	char buf[16];
	unsigned long power_put_delay = 0;
	int ret = 0;

	if (len > sizeof(buf) - 1)
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len - 1] = '\0';

	ret = kstrtoul(buf, 10, &power_put_delay);
	if (ret) {
		LOG_ERROR("failed to parse power put delay string: %s\n", buf);
		return -EFAULT;
	}

	rknpu_dev->power_put_delay = power_put_delay;

	LOG_INFO("set rknpu power put delay time %lums\n",
		 rknpu_dev->power_put_delay);

	return len;
}

static int rknpu_freq_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node = m->private;
	struct rknpu_debugger *debugger = node->debugger;
	struct rknpu_device *rknpu_dev =
		container_of(debugger, struct rknpu_device, debugger);
	struct clk *npu_clk;
	unsigned long current_freq = 0;

	npu_clk = rknpu_get_npu_clk(rknpu_dev);
	if (!npu_clk) {
		seq_puts(m, "unavailable\n");
		return 0;
	}

	/*
	 * The power implementation is reference-counted. A read must not
	 * force a full off transition when the NPU was already powered by
	 * another user; it only drops the temporary reference acquired here.
	 */
	if (rknpu_power_get(rknpu_dev))
		return 0;

	current_freq = clk_get_rate(npu_clk);

	rknpu_power_put(rknpu_dev);

	seq_printf(m, "%lu\n", current_freq);

	return 0;
}

#ifdef CONFIG_PM_DEVFREQ
static ssize_t rknpu_freq_set(struct file *file, const char __user *ubuf,
			      size_t len, loff_t *offp)
{
	struct seq_file *priv = file->private_data;
	struct rknpu_debugger_node *node = priv->private;
	struct rknpu_debugger *debugger = node->debugger;
	struct rknpu_device *rknpu_dev =
		container_of(debugger, struct rknpu_device, debugger);
	char buf[16];
	unsigned long freq = 0;
	int ret = 0;

	if (len > sizeof(buf) - 1)
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len - 1] = '\0';

	ret = kstrtoul(buf, 10, &freq);
	if (ret) {
		LOG_ERROR("failed to parse freq string: %s\n", buf);
		return -EFAULT;
	}

	if (rknpu_power_get(rknpu_dev))
		return -ENODEV;

	ret = rknpu_opp_set_freq(rknpu_dev, freq);
	if (!ret)
		rknpu_dev->ondemand_freq = rknpu_dev->current_freq;

	rknpu_power_put(rknpu_dev);

	if (ret)
		return ret;

	return len;
}
#else
static ssize_t rknpu_freq_set(struct file *file, const char __user *ubuf,
			      size_t len, loff_t *offp)
{
	return -EFAULT;
}
#endif

static int rknpu_volt_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node = m->private;
	struct rknpu_debugger *debugger = node->debugger;
	struct rknpu_device *rknpu_dev =
		container_of(debugger, struct rknpu_device, debugger);
	struct rknpu_core *core;
	unsigned int i;
	unsigned int j;

	/*
	 * The mainline DT exposes supplies on each core node. There is no
	 * single aggregate vdd, so report each core's supplies explicitly.
	 */
	for (i = 0; i < RKNPU_MAX_CORES; i++) {
		core = &rknpu_dev->cores[i];
		if (!core->registered)
			continue;

		for (j = 0; j < core->num_regulators; j++) {
			int voltage;

			if (!core->regulators[j].consumer)
				continue;

			voltage = regulator_get_voltage(
				core->regulators[j].consumer);
			if (voltage < 0)
				continue;

			seq_printf(m, "core%u %s %d\n", i,
				   core->regulators[j].supply, voltage);
		}
	}

	return 0;
}

static int rknpu_reset_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node = m->private;
	struct rknpu_debugger *debugger = node->debugger;
	struct rknpu_device *rknpu_dev =
		container_of(debugger, struct rknpu_device, debugger);

	if (!rknpu_dev->bypass_soft_reset)
		seq_puts(m, "on\n");
	else
		seq_puts(m, "off\n");

	return 0;
}

static ssize_t rknpu_reset_set(struct file *file, const char __user *ubuf,
			       size_t len, loff_t *offp)
{
	struct seq_file *priv = file->private_data;
	struct rknpu_debugger_node *node = priv->private;
	struct rknpu_debugger *debugger = node->debugger;
	struct rknpu_device *rknpu_dev =
		container_of(debugger, struct rknpu_device, debugger);
	char buf[8];

	if (len > sizeof(buf) - 1)
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len - 1] = '\0';

	if (strcmp(buf, "1") == 0 &&
	    atomic_read(&rknpu_dev->power_refcount) > 0)
		rknpu_soft_reset(rknpu_dev);
	else if (strcmp(buf, "on") == 0)
		rknpu_dev->bypass_soft_reset = 0;
	else if (strcmp(buf, "off") == 0)
		rknpu_dev->bypass_soft_reset = 1;

	return len;
}

static int rknpu_mem_stats_show(struct seq_file *m, void *data)
{
	struct rknpu_debugger_node *node = m->private;
	struct rknpu_device *dev = container_of(node->debugger,
						struct rknpu_device, debugger);

	return rknpu_gem_mem_stats_show(m, dev);
}

static struct rknpu_debugger_list rknpu_debugger_root_list[] = {
	{ "mem_stats", rknpu_mem_stats_show, NULL, NULL },
	{ "version", rknpu_version_show, NULL, NULL },
	{ "load", rknpu_load_show, NULL, NULL },
	{ "power", rknpu_power_show, rknpu_power_set, NULL },
	{ "regs", rknpu_regs_show, NULL, NULL },
	{ "freq", rknpu_freq_show, rknpu_freq_set, NULL },
	{ "volt", rknpu_volt_show, NULL, NULL },
	{ "delayms", rknpu_power_put_delay_show, rknpu_power_put_delay_set,
	  NULL },
	{ "reset", rknpu_reset_show, rknpu_reset_set, NULL },
};

static ssize_t rknpu_debugger_write(struct file *file, const char __user *ubuf,
				    size_t len, loff_t *offp)
{
	struct seq_file *priv = file->private_data;
	struct rknpu_debugger_node *node = priv->private;

	if (node->info_ent->write)
		return node->info_ent->write(file, ubuf, len, offp);
	else
		return len;
}

static int rknpu_debugfs_open(struct inode *inode, struct file *file)
{
	struct rknpu_debugger_node *node = inode->i_private;

	return single_open(file, node->info_ent->show, node);
}

static const struct file_operations rknpu_debugfs_fops = {
	.owner = THIS_MODULE,
	.open = rknpu_debugfs_open,
	.read = seq_read,
	.llseek = seq_lseek,
	.release = single_release,
	.write = rknpu_debugger_write,
};


static int rknpu_debugfs_remove_files(struct rknpu_debugger *debugger)
{
	struct rknpu_debugger_node *pos, *q;
	struct list_head *entry_list;

	mutex_lock(&debugger->debugfs_lock);

	/* Delete debugfs entry list */
	entry_list = &debugger->debugfs_entry_list;
	list_for_each_entry_safe(pos, q, entry_list, list) {
		if (pos->dent == NULL)
			continue;
		list_del(&pos->list);
		kfree(pos);
		pos = NULL;
	}

	/* Delete all debugfs node in this directory */
	debugfs_remove_recursive(debugger->debugfs_dir);
	debugger->debugfs_dir = NULL;

	mutex_unlock(&debugger->debugfs_lock);

	return 0;
}

static int rknpu_debugfs_create_files(const struct rknpu_debugger_list *files,
				      int count, struct dentry *root,
				      struct rknpu_debugger *debugger)
{
	int i;
	struct dentry *ent;
	struct rknpu_debugger_node *tmp;

	for (i = 0; i < count; i++) {
		tmp = kmalloc(sizeof(struct rknpu_debugger_node), GFP_KERNEL);
		if (tmp == NULL) {
			LOG_ERROR(
				"Cannot alloc node path /sys/kernel/debug/%pd/%s\n",
				root, files[i].name);
			goto MALLOC_FAIL;
		}

		tmp->info_ent = &files[i];
		tmp->debugger = debugger;

		ent = debugfs_create_file(files[i].name, S_IFREG | S_IRUGO,
					  root, tmp, &rknpu_debugfs_fops);
		if (!ent) {
			LOG_ERROR("Cannot create /sys/kernel/debug/%pd/%s\n",
				  root, files[i].name);
			goto CREATE_FAIL;
		}

		tmp->dent = ent;

		mutex_lock(&debugger->debugfs_lock);
		list_add_tail(&tmp->list, &debugger->debugfs_entry_list);
		mutex_unlock(&debugger->debugfs_lock);
	}

	return 0;

CREATE_FAIL:
	kfree(tmp);
MALLOC_FAIL:
	rknpu_debugfs_remove_files(debugger);

	return -1;
}

static int rknpu_debugfs_remove(struct rknpu_debugger *debugger)
{
	rknpu_debugfs_remove_files(debugger);

	return 0;
}

static int rknpu_debugfs_init(struct rknpu_debugger *debugger)
{
	int ret;

	debugger->debugfs_dir =
		debugfs_create_dir(RKNPU_DEBUGGER_ROOT_NAME, NULL);
	if (IS_ERR_OR_NULL(debugger->debugfs_dir)) {
		LOG_ERROR("failed on mkdir /sys/kernel/debug/%s\n",
			  RKNPU_DEBUGGER_ROOT_NAME);
		debugger->debugfs_dir = NULL;
		return -EIO;
	}

	ret = rknpu_debugfs_create_files(rknpu_debugger_root_list,
					 ARRAY_SIZE(rknpu_debugger_root_list),
					 debugger->debugfs_dir, debugger);
	if (ret) {
		LOG_ERROR(
			"Could not install rknpu_debugger_root_list debugfs\n");
		goto CREATE_FAIL;
	}

	return 0;

CREATE_FAIL:
	rknpu_debugfs_remove(debugger);

	return ret;
}


#ifdef CONFIG_ROCKCHIP_RKNPU_PROC_FS
static int rknpu_procfs_open(struct inode *inode, struct file *file)
{
	struct rknpu_debugger_node *node = pde_data(inode);

	return single_open(file, node->info_ent->show, node);
}

static const struct proc_ops rknpu_procfs_fops = {
	.proc_open = rknpu_procfs_open,
	.proc_read = seq_read,
	.proc_lseek = seq_lseek,
	.proc_release = single_release,
	.proc_write = rknpu_debugger_write,
};

static int rknpu_procfs_remove_files(struct rknpu_debugger *debugger)
{
	struct rknpu_debugger_node *pos, *q;
	struct list_head *entry_list;

	mutex_lock(&debugger->procfs_lock);

	/* Delete procfs entry list */
	entry_list = &debugger->procfs_entry_list;
	list_for_each_entry_safe(pos, q, entry_list, list) {
		if (pos->pent == NULL)
			continue;
		list_del(&pos->list);
		kfree(pos);
		pos = NULL;
	}

	/* Delete all procfs node in this directory */
	proc_remove(debugger->procfs_dir);
	debugger->procfs_dir = NULL;

	mutex_unlock(&debugger->procfs_lock);

	return 0;
}

static int rknpu_procfs_create_files(const struct rknpu_debugger_list *files,
				     int count, struct proc_dir_entry *root,
				     struct rknpu_debugger *debugger)
{
	int i;
	struct proc_dir_entry *ent;
	struct rknpu_debugger_node *tmp;

	for (i = 0; i < count; i++) {
		tmp = kmalloc(sizeof(struct rknpu_debugger_node), GFP_KERNEL);
		if (tmp == NULL) {
			LOG_ERROR("Cannot alloc node path for /proc/%s/%s\n",
				  RKNPU_DEBUGGER_ROOT_NAME, files[i].name);
			goto MALLOC_FAIL;
		}

		tmp->info_ent = &files[i];
		tmp->debugger = debugger;

		ent = proc_create_data(files[i].name, S_IFREG | S_IRUGO, root,
				       &rknpu_procfs_fops, tmp);
		if (!ent) {
			LOG_ERROR("Cannot create /proc/%s/%s\n",
				  RKNPU_DEBUGGER_ROOT_NAME, files[i].name);
			goto CREATE_FAIL;
		}

		tmp->pent = ent;

		mutex_lock(&debugger->procfs_lock);
		list_add_tail(&tmp->list, &debugger->procfs_entry_list);
		mutex_unlock(&debugger->procfs_lock);
	}

	return 0;

CREATE_FAIL:
	kfree(tmp);
MALLOC_FAIL:
	rknpu_procfs_remove_files(debugger);
	return -1;
}

static int rknpu_procfs_remove(struct rknpu_debugger *debugger)
{
	rknpu_procfs_remove_files(debugger);

	return 0;
}

static int rknpu_procfs_init(struct rknpu_debugger *debugger)
{
	int ret;

	debugger->procfs_dir = proc_mkdir(RKNPU_DEBUGGER_ROOT_NAME, NULL);
	if (IS_ERR_OR_NULL(debugger->procfs_dir)) {
		pr_err("failed on mkdir /proc/%s\n", RKNPU_DEBUGGER_ROOT_NAME);
		debugger->procfs_dir = NULL;
		return -EIO;
	}

	ret = rknpu_procfs_create_files(rknpu_debugger_root_list,
					ARRAY_SIZE(rknpu_debugger_root_list),
					debugger->procfs_dir, debugger);
	if (ret) {
		pr_err("Could not install rknpu_debugger_root_list procfs\n");
		goto CREATE_FAIL;
	}

	return 0;

CREATE_FAIL:
	rknpu_procfs_remove(debugger);

	return ret;
}
#endif /* #ifdef CONFIG_ROCKCHIP_RKNPU_PROC_FS */

int rknpu_debugger_init(struct rknpu_device *rknpu_dev)
{
	mutex_init(&rknpu_dev->debugger.debugfs_lock);
	INIT_LIST_HEAD(&rknpu_dev->debugger.debugfs_entry_list);
	rknpu_debugfs_init(&rknpu_dev->debugger);
#ifdef CONFIG_ROCKCHIP_RKNPU_PROC_FS
	mutex_init(&rknpu_dev->debugger.procfs_lock);
	INIT_LIST_HEAD(&rknpu_dev->debugger.procfs_entry_list);
	rknpu_procfs_init(&rknpu_dev->debugger);
#endif
	return 0;
}

int rknpu_debugger_remove(struct rknpu_device *rknpu_dev)
{
	rknpu_debugfs_remove(&rknpu_dev->debugger);
#ifdef CONFIG_ROCKCHIP_RKNPU_PROC_FS
	rknpu_procfs_remove(&rknpu_dev->debugger);
#endif
	return 0;
}
