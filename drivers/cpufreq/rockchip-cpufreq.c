/*
 * Copyright (C) 2013 ROCKCHIP, Inc.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */
#define pr_fmt(fmt) "cpufreq: " fmt
#include <linux/clk.h>
#include <linux/cpufreq.h>
#include <linux/err.h>
#include <linux/kernel_stat.h>
#include <linux/init.h>
#include <linux/reboot.h>
#include <linux/suspend.h>
#include <linux/tick.h>
#include <linux/workqueue.h>
#include <linux/delay.h>
#include <linux/regulator/consumer.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/string.h>
#include <linux/rockchip/cpu.h>
#include <linux/rockchip/dvfs.h>
#include <asm/smp_plat.h>
#include <asm/cpu.h>
#include <asm/unistd.h>
#include <asm/uaccess.h>
#include <asm/system_misc.h>
#include <linux/rockchip/common.h>
#include <dt-bindings/clock/rk_system_status.h>
#include "../../../drivers/clk/rockchip/clk-pd.h"

extern void dvfs_disable_temp_limit(void);

#define VERSION "1.0"

#ifdef DEBUG
#define FREQ_DBG(fmt, args...) pr_debug(fmt, ## args)
#define FREQ_LOG(fmt, args...) pr_debug(fmt, ## args)
#else
#define FREQ_DBG(fmt, args...) do {} while(0)
#define FREQ_LOG(fmt, args...) do {} while(0)
#endif
#define FREQ_ERR(fmt, args...) pr_err(fmt, ## args)

/* Frequency table index must be sequential starting at 0 */
static struct cpufreq_frequency_table default_freq_table[] = {
	{.frequency = 312 * 1000,       .index = 875 * 1000},
	{.frequency = 504 * 1000,       .index = 925 * 1000},
	{.frequency = 816 * 1000,       .index = 975 * 1000},
	{.frequency = 1008 * 1000,      .index = 1075 * 1000},
	{.frequency = 1200 * 1000,      .index = 1150 * 1000},
	{.frequency = 1416 * 1000,      .index = 1250 * 1000},
	{.frequency = 1608 * 1000,      .index = 1350 * 1000},
	{.frequency = CPUFREQ_TABLE_END},
};
static struct cpufreq_frequency_table *freq_table = default_freq_table;
/*********************************************************/
/* additional symantics for "relation" in cpufreq with pm */
#define DISABLE_FURTHER_CPUFREQ         0x10
#define ENABLE_FURTHER_CPUFREQ          0x20
#define MASK_FURTHER_CPUFREQ            0x30
/* With 0x00(NOCHANGE), it depends on the previous "further" status */
#define CPUFREQ_PRIVATE                 0x100
static unsigned int no_cpufreq_access = 0;
static unsigned int suspend_freq = 816 * 1000;
static unsigned int suspend_volt = 1100000;
static unsigned int low_battery_freq = 600 * 1000;
static unsigned int low_battery_capacity = 5; // 5%
static bool is_booting = true;
static DEFINE_MUTEX(cpufreq_mutex);
static bool gpu_is_mali400;
struct dvfs_node *clk_cpu_dvfs_node = NULL;
struct dvfs_node *clk_gpu_dvfs_node = NULL;
struct dvfs_node *aclk_vio1_dvfs_node = NULL;
struct dvfs_node *clk_ddr_dvfs_node = NULL;
/*******************************************************/
static unsigned int cpufreq_get_rate(unsigned int cpu)
{
	if (clk_cpu_dvfs_node)
		return clk_get_rate(clk_cpu_dvfs_node->clk) / 1000;

	return 0;
}

static bool cpufreq_is_ondemand(struct cpufreq_policy *policy)
{
	char c = 0;
	if (policy && policy->governor)
		c = policy->governor->name[0];
	return (c == 'o' || c == 'i' || c == 'c' || c == 'h');
}

static unsigned int get_freq_from_table(unsigned int max_freq)
{
	unsigned int i;
	unsigned int target_freq = 0;
	for (i = 0; freq_table[i].frequency != CPUFREQ_TABLE_END; i++) {
		unsigned int freq = freq_table[i].frequency;
		if (freq <= max_freq && target_freq < freq) {
			target_freq = freq;
		}
	}
	if (!target_freq)
		target_freq = max_freq;
	return target_freq;
}

static int cpufreq_notifier_policy(struct notifier_block *nb, unsigned long val, void *data)
{
	static unsigned int min_rate=0, max_rate=-1;
	struct cpufreq_policy *policy = data;

	if (val != CPUFREQ_ADJUST)
		return 0;

	if (cpufreq_is_ondemand(policy)) {
		FREQ_DBG("queue work\n");
		dvfs_clk_enable_limit(clk_cpu_dvfs_node, min_rate, max_rate);
	} else {
		FREQ_DBG("cancel work\n");
		dvfs_clk_get_limit(clk_cpu_dvfs_node, &min_rate, &max_rate);
	}

	return 0;
}

static struct notifier_block notifier_policy_block = {
	.notifier_call = cpufreq_notifier_policy
};

static int cpufreq_verify(struct cpufreq_policy *policy)
{
	if (!freq_table)
		return -EINVAL;
	return cpufreq_frequency_table_verify(policy, freq_table);
}

static int cpufreq_scale_rate_for_dvfs(struct clk *clk, unsigned long rate)
{
	int ret;
	struct cpufreq_freqs freqs;
	struct cpufreq_policy *policy;
	
	freqs.new = rate / 1000;
	freqs.old = clk_get_rate(clk) / 1000;
	
	for_each_online_cpu(freqs.cpu) {
		policy = cpufreq_cpu_get(freqs.cpu);
		cpufreq_notify_transition(policy, &freqs, CPUFREQ_PRECHANGE);
		cpufreq_cpu_put(policy);
	}
	
	FREQ_DBG("cpufreq_scale_rate_for_dvfs(%lu)\n", rate);
	
	ret = clk_set_rate(clk, rate);

	freqs.new = clk_get_rate(clk) / 1000;
	/* notifiers */
	for_each_online_cpu(freqs.cpu) {
		policy = cpufreq_cpu_get(freqs.cpu);
		cpufreq_notify_transition(policy, &freqs, CPUFREQ_POSTCHANGE);
		cpufreq_cpu_put(policy);
	}

	return ret;
	
}

static int cpufreq_init_cpu0(struct cpufreq_policy *policy)
{
	unsigned int i;
	int ret;
	struct regulator *vdd_gpu_regulator;

	gpu_is_mali400 = cpu_is_rk3188();

	clk_gpu_dvfs_node = clk_get_dvfs_node("clk_gpu");
	if (clk_gpu_dvfs_node){
		clk_enable_dvfs(clk_gpu_dvfs_node);
		vdd_gpu_regulator = dvfs_get_regulator("vdd_gpu");
		if (!IS_ERR_OR_NULL(vdd_gpu_regulator)) {
			if (!regulator_is_enabled(vdd_gpu_regulator)) {
				ret = regulator_enable(vdd_gpu_regulator);
				arm_pm_restart('h', NULL);
			}
			/* make sure vdd_gpu_regulator is in use,
			so it will not be disable by regulator_init_complete*/
			ret = regulator_enable(vdd_gpu_regulator);
			if (ret != 0)
				arm_pm_restart('h', NULL);
		}
		if (gpu_is_mali400)
			dvfs_clk_enable_limit(clk_gpu_dvfs_node, 133000000, 600000000);	
	}

	clk_ddr_dvfs_node = clk_get_dvfs_node("clk_ddr");
	if (clk_ddr_dvfs_node){
		clk_enable_dvfs(clk_ddr_dvfs_node);
	}

	clk_cpu_dvfs_node = clk_get_dvfs_node("clk_core");
	if (!clk_cpu_dvfs_node){
		return -EINVAL;
	}
	dvfs_clk_register_set_rate_callback(clk_cpu_dvfs_node, cpufreq_scale_rate_for_dvfs);
	freq_table = dvfs_get_freq_volt_table(clk_cpu_dvfs_node);
	if (freq_table == NULL) {
		freq_table = default_freq_table;
	} else {
		int v = INT_MAX;
		for (i = 0; freq_table[i].frequency != CPUFREQ_TABLE_END; i++) {
			if (freq_table[i].index >= suspend_volt && v > freq_table[i].index) {
				suspend_freq = freq_table[i].frequency;
				v = freq_table[i].index;
			}
		}
	}
	low_battery_freq = get_freq_from_table(low_battery_freq);
	clk_enable_dvfs(clk_cpu_dvfs_node);

	cpufreq_register_notifier(&notifier_policy_block, CPUFREQ_POLICY_NOTIFIER);

	printk("cpufreq version " VERSION ", suspend freq %d MHz\n", suspend_freq / 1000);
	return 0;
}

static int cpufreq_init(struct cpufreq_policy *policy)
{
	static int cpu0_err;
	
	if (policy->cpu == 0) {
		cpu0_err = cpufreq_init_cpu0(policy);
	}
	
	if (cpu0_err)
		return cpu0_err;
	
	//set freq min max
	cpufreq_frequency_table_cpuinfo(policy, freq_table);
	//sys nod
	cpufreq_frequency_table_get_attr(freq_table, policy->cpu);


	policy->cur = clk_get_rate(clk_cpu_dvfs_node->clk) / 1000;

	policy->cpuinfo.transition_latency = 40 * NSEC_PER_USEC;	// make ondemand default sampling_rate to 40000

	/*
	 * On SMP configuartion, both processors share the voltage
	 * and clock. So both CPUs needs to be scaled together and hence
	 * needs software co-ordination. Use cpufreq affected_cpus
	 * interface to handle this scenario. Additional is_smp() check
	 * is to keep SMP_ON_UP build working.
	 */
	if (is_smp())
		cpumask_setall(policy->cpus);

	return 0;

}

static int cpufreq_exit(struct cpufreq_policy *policy)
{
	if (policy->cpu != 0)
		return 0;

	cpufreq_frequency_table_cpuinfo(policy, freq_table);
	clk_put_dvfs_node(clk_cpu_dvfs_node);
	cpufreq_unregister_notifier(&notifier_policy_block, CPUFREQ_POLICY_NOTIFIER);

	return 0;
}

static struct freq_attr *cpufreq_attr[] = {
	&cpufreq_freq_attr_scaling_available_freqs,
	NULL,
};

#ifdef CONFIG_CHARGER_DISPLAY
extern int rk_get_system_battery_capacity(void);
#else
static int rk_get_system_battery_capacity(void) { return 100; }
#endif

static unsigned int cpufreq_scale_limit(unsigned int target_freq, struct cpufreq_policy *policy, bool is_private)
{
	bool is_ondemand = cpufreq_is_ondemand(policy);

	if (!is_ondemand)
		return target_freq;

	if (is_booting) {
		s64 boottime_ms = ktime_to_ms(ktime_get_boottime());
		if (boottime_ms > 60 * MSEC_PER_SEC) {
			is_booting = false;
		} else if (target_freq > low_battery_freq &&
			   rk_get_system_battery_capacity() <= low_battery_capacity) {
			target_freq = low_battery_freq;
		}
	}

	return target_freq;
}

static int cpufreq_target(struct cpufreq_policy *policy, unsigned int target_freq, unsigned int relation)
{
	unsigned int i, new_freq = target_freq, new_rate, cur_rate;
	int ret = 0;
	bool is_private;

	if (!freq_table) {
		FREQ_ERR("no freq table!\n");
		return -EINVAL;
	}

	mutex_lock(&cpufreq_mutex);

	is_private = relation & CPUFREQ_PRIVATE;
	relation &= ~CPUFREQ_PRIVATE;

	if ((relation & ENABLE_FURTHER_CPUFREQ) && no_cpufreq_access)
		no_cpufreq_access--;
	if (no_cpufreq_access) {
		FREQ_LOG("denied access to %s as it is disabled temporarily\n", __func__);
		ret = -EINVAL;
		goto out;
	}
	if (relation & DISABLE_FURTHER_CPUFREQ)
		no_cpufreq_access++;
	relation &= ~MASK_FURTHER_CPUFREQ;

	ret = cpufreq_frequency_table_target(policy, freq_table, target_freq, relation, &i);
	if (ret) {
		FREQ_ERR("no freq match for %d(ret=%d)\n", target_freq, ret);
		goto out;
	}
	new_freq = freq_table[i].frequency;
	if (!no_cpufreq_access)
		new_freq = cpufreq_scale_limit(new_freq, policy, is_private);

	new_rate = new_freq * 1000;
	cur_rate = dvfs_clk_get_rate(clk_cpu_dvfs_node);
	FREQ_LOG("req = %7u new = %7u (was = %7u)\n", target_freq, new_freq, cur_rate / 1000);
	if (new_rate == cur_rate)
		goto out;
	ret = dvfs_clk_set_rate(clk_cpu_dvfs_node, new_rate);

out:
	FREQ_DBG("set freq (%7u) end, ret %d\n", new_freq, ret);
	mutex_unlock(&cpufreq_mutex);
	return ret;

}

static int cpufreq_pm_notifier_event(struct notifier_block *this, unsigned long event, void *ptr)
{
	int ret = NOTIFY_DONE;
	struct cpufreq_policy *policy = cpufreq_cpu_get(0);

	if (!policy)
		return ret;

	if (!cpufreq_is_ondemand(policy))
		goto out;

	switch (event) {
	case PM_SUSPEND_PREPARE:
		policy->cur++;
		ret = cpufreq_driver_target(policy, suspend_freq, DISABLE_FURTHER_CPUFREQ | CPUFREQ_RELATION_H);
		if (ret < 0) {
			ret = NOTIFY_BAD;
			goto out;
		}
		ret = NOTIFY_OK;
		break;
	case PM_POST_RESTORE:
	case PM_POST_SUSPEND:
		//if (target_freq == policy->cur) then cpufreq_driver_target
		//will return, and our target will not be called, it casue
		//ENABLE_FURTHER_CPUFREQ flag invalid, avoid that.
		policy->cur++;
		cpufreq_driver_target(policy, suspend_freq, ENABLE_FURTHER_CPUFREQ | CPUFREQ_RELATION_H);
		ret = NOTIFY_OK;
		break;
	}
out:
	cpufreq_cpu_put(policy);
	return ret;
}

static struct notifier_block cpufreq_pm_notifier = {
	.notifier_call = cpufreq_pm_notifier_event,
};

int rockchip_cpufreq_reboot_limit_freq(void)
{
	struct regulator *regulator;
	int volt = 0;
	u32 rate;

	dvfs_disable_temp_limit();
	dvfs_clk_enable_limit(clk_cpu_dvfs_node, 1000*suspend_freq, 1000*suspend_freq);

	rate = dvfs_clk_get_rate(clk_cpu_dvfs_node);
	regulator = dvfs_get_regulator("vdd_arm");
	if (regulator)
		volt = regulator_get_voltage(regulator);
	else
		pr_info("cpufreq: get arm regulator failed\n");
	pr_info("cpufreq: reboot set core rate=%lu, volt=%d\n",
		dvfs_clk_get_rate(clk_cpu_dvfs_node), volt);

	return 0;
}

static int cpufreq_reboot_notifier_event(struct notifier_block *this,
					 unsigned long event, void *ptr)
{
	rockchip_set_system_status(SYS_STATUS_REBOOT);
	rockchip_cpufreq_reboot_limit_freq();

	return NOTIFY_OK;
}

static struct notifier_block cpufreq_reboot_notifier = {
	.notifier_call = cpufreq_reboot_notifier_event,
};

static int clk_pd_vio_notifier_call(struct notifier_block *nb, unsigned long event, void *ptr)
{
	switch (event) {
	case RK_CLK_PD_PREPARE:
		if (aclk_vio1_dvfs_node)
			clk_enable_dvfs(aclk_vio1_dvfs_node);
		break;
	case RK_CLK_PD_UNPREPARE:
		if (aclk_vio1_dvfs_node)
			clk_disable_dvfs(aclk_vio1_dvfs_node);
		break;
	}
	return NOTIFY_OK;
}

static struct notifier_block clk_pd_vio_notifier = {
	.notifier_call = clk_pd_vio_notifier_call,
};

#ifdef CONFIG_PM_WARP

#include <linux/rockchip/iomap.h>

static u32 cru_apll_con[3], cru_dpll_con[3], cru_cpll_con[3], cru_gpll_con[3];
static u32 cru_mode_con, cru_clksel_con[35], cru_clkgate_con[11];
static u32 cru_glb_srst_fst_value, cru_glb_srst_snd_value, cru_softrst_con[9];
static u32 cru_misc_con, cru_glb_cnt_th, cru_glb_rst_st, cru_sdmmc_con[2];
static u32 cru_sdio_con[2], cru_emmc_con[2], cru_pll_prg_en;

static int cpufreq_rk_suspend (struct cpufreq_policy *policy)
{
        if (pm_device_down) {
                int i;

		if (clk_cpu_dvfs_node)
			clk_disable_dvfs(clk_cpu_dvfs_node);

		for (i = 0; i < 3; i++) {
			cru_apll_con[i] = readl_relaxed(RK_CRU_VIRT
							+ 0x0000 + (i * 4));
			if (i != 2)
				cru_apll_con[i] |= 0xffff0000;
		}
#if 0
#if 0
		for (i = 0; i < 3; i++) {
			cru_dpll_con[i] = readl_relaxed(RK_CRU_VIRT
							+ 0x0010 + (i * 4));
			if (i != 2)
				cru_dpll_con[i] |= 0xffff0000;
		}
#endif
		for (i = 0; i < 3; i++) {
			cru_cpll_con[i] = readl_relaxed(RK_CRU_VIRT
							+ 0x0020 + (i * 4));
			if (i != 2)
				cru_cpll_con[i] |= 0xffff0000;
		}
		for (i = 0; i < 3; i++) {
			cru_gpll_con[i] = readl_relaxed(RK_CRU_VIRT
							+ 0x0030 + (i * 4));
			if (i != 2)
				cru_gpll_con[i] |= 0xffff0000;
		}
		cru_mode_con = 0x11110000 | readl_relaxed(RK_CRU_VIRT + 0x0040);
		for (i = 0; i < 35; i++) {
			if ((i == 16) || (i == 21) || (i == 22) || (i == 33))
				continue;
			cru_clksel_con[i] = readl_relaxed(RK_CRU_VIRT
							  + 0x0044 + (i * 4));
			if ((i >= 0 && i <= 6) || (i >= 9 && i <= 15)
			    || (i >= 23 && i <= 34))
				cru_clksel_con[i] |= 0xffff0000;
		}
		for (i = 0; i < 11; i++)
			cru_clkgate_con[i] = 0xffff0000
					     | readl_relaxed(RK_CRU_VIRT
							     + 0x00d0
							     + (i * 4));
		cru_glb_srst_fst_value = readl_relaxed(RK_CRU_VIRT + 0x0100);
		cru_glb_srst_snd_value = readl_relaxed(RK_CRU_VIRT + 0x0104);
#if 0
		for (i = 0; i < 9; i++)
			cru_softrst_con[i] = 0xffff0000
					     | readl_relaxed(RK_CRU_VIRT
							     + 0x0110
							     + (i * 4));
#endif
		cru_misc_con = 0xffff0000 | readl_relaxed(RK_CRU_VIRT + 0x0134);
		cru_glb_cnt_th = readl_relaxed(RK_CRU_VIRT + 0x0140);
		cru_glb_rst_st = readl_relaxed(RK_CRU_VIRT + 0x0150);
#if 0
		cru_sdmmc_con[0] = 0xffff0000
				   | readl_relaxed(RK_CRU_VIRT + 0x01c0);
		cru_sdmmc_con[1] = 0xffff0000
				   | readl_relaxed(RK_CRU_VIRT + 0x01c4);
		cru_sdio_con[0] = 0xffff0000
				   | readl_relaxed(RK_CRU_VIRT + 0x01c8);
		cru_sdio_con[1] = 0xffff0000
				   | readl_relaxed(RK_CRU_VIRT + 0x01cc);
		cru_emmc_con[0] = 0xffff0000
				   | readl_relaxed(RK_CRU_VIRT + 0x01d8);
		cru_emmc_con[1] = 0xffff0000
				   | readl_relaxed(RK_CRU_VIRT + 0x01dc);
		cru_pll_prg_en = 0xffff0000
				   | readl_relaxed(RK_CRU_VIRT + 0x01f0);
#endif
#endif
        }
	return 0;
}

static int cpufreq_rk_resume (struct cpufreq_policy *policy)
{
	if (pm_device_down) {
		int i;
		u32 cru_apll_con0_tmp, cru_apll_con1_tmp;
		unsigned long rate_old, rate_new;

		/* ARM PLL */
		cru_apll_con0_tmp = readl_relaxed(RK_CRU_VIRT + 0x0000);
		cru_apll_con1_tmp = readl_relaxed(RK_CRU_VIRT + 0x0004);
		rate_old = 24 / (cru_apll_con1_tmp & 0x3f)
			   * (cru_apll_con0_tmp & 0xfff)
			   / ((cru_apll_con0_tmp >> 12) & 0x7)
			   / ((cru_apll_con1_tmp >> 6) & 0x7);
		rate_new = 24 / (cru_apll_con[1] & 0x3f)
			   * (cru_apll_con[0] & 0xfff)
			   / ((cru_apll_con[0] >> 12) & 0x7)
			   / ((cru_apll_con[1] >> 6) & 0x7);
		if (rate_old <= rate_new) {
			writel_relaxed(cru_clksel_con[0], RK_CRU_VIRT + 0x0044);
			writel_relaxed(cru_clksel_con[1], RK_CRU_VIRT + 0x0048);
		}
		/* select GPLL div2 */
		writel_relaxed(0x00800080, RK_CRU_VIRT + 0x0044);
		for (i = 0; i < 3; i++)
			writel_relaxed(cru_apll_con[i],
				       RK_CRU_VIRT + 0x0000 + (i * 4));
		/* CRU_APLL_CON1 wait pll lock */
		i = 24000000;
		while (i-- > 0) {
			if (readl_relaxed(RK_CRU_VIRT + 0x0004) & (1 << 10))
				break;
		}
		/* select APLL */
		writel_relaxed(0x00800000, RK_CRU_VIRT + 0x0044);
		if (rate_old > rate_new) {
			writel_relaxed(cru_clksel_con[0], RK_CRU_VIRT + 0x0044);
			writel_relaxed(cru_clksel_con[1], RK_CRU_VIRT + 0x0048);
		}
#if 0   /* DDR PLL */
		writel_relaxed(0x00100000, RK_CRU_VIRT + 0x0040);
		for (i = 0; i < 3; i++)
			writel_relaxed(cru_dpll_con[i],
				       RK_CRU_VIRT + 0x0010 + (i * 4));
		i = 24000000;
		while (i-- > 0) {
			if (readl_relaxed(RK_CRU_VIRT + 0x0014) & (1 << 10))
				break;
		}
		writel_relaxed(0x00100010, RK_CRU_VIRT + 0x0040);
		writel_relaxed(0x01000000, RK_CRU_VIRT + 0x0040);
		for (i = 0; i < 3; i++)
			writel_relaxed(cru_cpll_con[i],
				       RK_CRU_VIRT + 0x0020 + (i * 4));
		i = 24000000;
		while (i-- > 0) {
			if (readl_relaxed(RK_CRU_VIRT + 0x0024) & (1 << 10))
				break;
		}
		writel_relaxed(0x01000100, RK_CRU_VIRT + 0x0040);
		writel_relaxed(0x10000000, RK_CRU_VIRT + 0x0040);
		for (i = 0; i < 3; i++)
			writel_relaxed(cru_gpll_con[i],
				       RK_CRU_VIRT + 0x0030 + (i * 4));
		i = 24000000;
		while (i-- > 0) {
			if (readl_relaxed(RK_CRU_VIRT + 0x0034) & (1 << 10))
				break;
		}
		writel_relaxed(0x10001000, RK_CRU_VIRT + 0x0040);
#if 0
		writel_relaxed(cru_mode_con, RK_CRU_VIRT + 0x0040);
#endif
		for (i = 2; i < 35; i++) {
			if ((i == 16) || (i == 21) || (i == 22) || (i == 33))
				continue;
			writel_relaxed(cru_clksel_con[i],
				       RK_CRU_VIRT + 0x0044 + (i * 4));
		}
		for (i = 0; i < 11; i++)
			writel_relaxed(cru_clkgate_con[i],
				       RK_CRU_VIRT + 0x00d0 + (i * 4));
		writel_relaxed(cru_glb_srst_fst_value, RK_CRU_VIRT + 0x0100);
		writel_relaxed(cru_glb_srst_snd_value, RK_CRU_VIRT + 0x0104);
#if 0
		for (i = 0; i < 9; i++)
			writel_relaxed(cru_softrst_con[i],
				       RK_CRU_VIRT + 0x0110 + (i * 4));
#endif
		writel_relaxed(cru_misc_con, RK_CRU_VIRT + 0x0134);
		writel_relaxed(cru_glb_cnt_th, RK_CRU_VIRT + 0x0140);
#if 0
		writel_relaxed(cru_glb_rst_st, RK_CRU_VIRT + 0x0150);
		writel_relaxed(cru_sdmmc_con[0], RK_CRU_VIRT + 0x01c0);
		writel_relaxed(cru_sdmmc_con[1], RK_CRU_VIRT + 0x01c4);
		writel_relaxed(cru_sdio_con[0], RK_CRU_VIRT + 0x01c8);
		writel_relaxed(cru_sdio_con[1], RK_CRU_VIRT + 0x01cc);
		writel_relaxed(cru_emmc_con[0], RK_CRU_VIRT + 0x01d8);
		writel_relaxed(cru_emmc_con[1], RK_CRU_VIRT + 0x01dc);
		writel_relaxed(0xffff5a5a, RK_CRU_VIRT + 0x01f0);
#endif
#endif

		if (clk_cpu_dvfs_node)
			clk_enable_dvfs(clk_cpu_dvfs_node);
        }
	return 0;
}
#endif	/* CONFIG_PM_WARP */

static struct cpufreq_driver cpufreq_driver = {
	.flags = CPUFREQ_CONST_LOOPS,
	.verify = cpufreq_verify,
	.target = cpufreq_target,
	.get = cpufreq_get_rate,
	.init = cpufreq_init,
	.exit = cpufreq_exit,
	.name = "rockchip",
	.attr = cpufreq_attr,
#ifdef CONFIG_PM_WARP
	.suspend = cpufreq_rk_suspend,
	.resume = cpufreq_rk_resume,
#endif
};

static int __init cpufreq_driver_init(void)
{
	struct clk *clk;

	clk = clk_get(NULL, "pd_vio");
	if (clk) {
		rk_clk_pd_notifier_register(clk, &clk_pd_vio_notifier);
		aclk_vio1_dvfs_node = clk_get_dvfs_node("aclk_vio1");
		if (aclk_vio1_dvfs_node && __clk_is_enabled(clk)){
			clk_enable_dvfs(aclk_vio1_dvfs_node);
		}
	}
	register_reboot_notifier(&cpufreq_reboot_notifier);
	register_pm_notifier(&cpufreq_pm_notifier);
	return cpufreq_register_driver(&cpufreq_driver);
}

device_initcall(cpufreq_driver_init);
