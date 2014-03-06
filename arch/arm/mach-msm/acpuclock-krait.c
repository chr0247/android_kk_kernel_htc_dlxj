/*
 * Copyright (c) 2011-2012, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/mutex.h>
#include <linux/err.h>
#include <linux/errno.h>
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/regulator/consumer.h>

#include <asm/mach-types.h>
#include <asm/cpu.h>

#include <mach/board.h>
#include <mach/msm_iomap.h>
#include <mach/socinfo.h>
#include <mach/msm-krait-l2-accessors.h>
#include <mach/rpm-regulator.h>
#include <mach/rpm-regulator-smd.h>
#include <mach/msm_bus.h>

#include "acpuclock.h"
#include "acpuclock-krait.h"
#include "avs.h"

#ifdef CONFIG_CPU_OVERCLOCK
#define OVERCLOCK_EXTRA_FREQS	7
#else
#define OVERCLOCK_EXTRA_FREQS	0
#endif

#ifdef CONFIG_LOW_CPUCLOCKS
#define FREQ_TABLE_SIZE		(40 + OVERCLOCK_EXTRA_FREQS)
#else
#define FREQ_TABLE_SIZE		(35 + OVERCLOCK_EXTRA_FREQS)
#endif

#define CPU_FOOT_PRINT_MAGIC				0xACBDFE00
static void set_acpuclk_foot_print(unsigned cpu, unsigned state)
{
	unsigned *status = (unsigned *)(CPU_FOOT_PRINT_BASE + 0x6C) + cpu;
	*status = (CPU_FOOT_PRINT_MAGIC | state);
	mb();
}

static void set_acpuclk_cpu_freq_foot_print(unsigned cpu, unsigned khz)
{
	unsigned *status = (unsigned *)(CPU_FOOT_PRINT_BASE + 0x58) + cpu;
	*status = khz;
	mb();
}

static void set_acpuclk_L2_freq_foot_print(unsigned khz)
{
	unsigned *status = (unsigned *)(CPU_FOOT_PRINT_BASE + 0x68);
	*status = khz;
	mb();
}

#ifdef CONFIG_ACPU_CUSTOM_FREQ_SUPPORT
static unsigned long acpu_max_freq = CONFIG_ACPU_MAX_FREQ;
#endif

#define PRI_SRC_SEL_SEC_SRC	0
#define PRI_SRC_SEL_HFPLL	1
#define PRI_SRC_SEL_HFPLL_DIV2	2

#define SECCLKAGD		BIT(4)

static DEFINE_MUTEX(driver_lock);
static DEFINE_SPINLOCK(l2_lock);

static struct drv_data drv;

static unsigned long acpuclk_krait_get_rate(int cpu)
{
	return drv.scalable[cpu].cur_speed->khz;
}

static void set_pri_clk_src(struct scalable *sc, u32 pri_src_sel)
{
	u32 regval;

	regval = get_l2_indirect_reg(sc->l2cpmr_iaddr);
	regval &= ~0x3;
	regval |= (pri_src_sel & 0x3);
	set_l2_indirect_reg(sc->l2cpmr_iaddr, regval);
	
	mb();
	udelay(1);
}

static void set_sec_clk_src(struct scalable *sc, u32 sec_src_sel)
{
	u32 regval;

	
	regval = get_l2_indirect_reg(sc->l2cpmr_iaddr);
	regval |= SECCLKAGD;
	set_l2_indirect_reg(sc->l2cpmr_iaddr, regval);

	
	regval &= ~(0x3 << 2);
	regval |= ((sec_src_sel & 0x3) << 2);
	set_l2_indirect_reg(sc->l2cpmr_iaddr, regval);

	
	regval &= ~SECCLKAGD;
	set_l2_indirect_reg(sc->l2cpmr_iaddr, regval);

	
	mb();
	udelay(1);
}

static int enable_rpm_vreg(struct vreg *vreg)
{
	int ret = 0;

	if (vreg->rpm_reg) {
		ret = rpm_regulator_enable(vreg->rpm_reg);
		if (ret)
			dev_err(drv.dev, "%s regulator enable failed (%d)\n",
				vreg->name, ret);
	}

	return ret;
}

static void disable_rpm_vreg(struct vreg *vreg)
{
	int rc;

	if (vreg->rpm_reg) {
		rc = rpm_regulator_disable(vreg->rpm_reg);
		if (rc)
			dev_err(drv.dev, "%s regulator disable failed (%d)\n",
				vreg->name, rc);
	}
}

static void hfpll_enable(struct scalable *sc, bool skip_regulators)
{
	if (!skip_regulators) {
		
		enable_rpm_vreg(&sc->vreg[VREG_HFPLL_A]);
		enable_rpm_vreg(&sc->vreg[VREG_HFPLL_B]);
	}

	
	writel_relaxed(0x2, sc->hfpll_base + drv.hfpll_data->mode_offset);

	mb();
	udelay(10);

	
	writel_relaxed(0x6, sc->hfpll_base + drv.hfpll_data->mode_offset);

	
	mb();
	udelay(60);

	
	writel_relaxed(0x7, sc->hfpll_base + drv.hfpll_data->mode_offset);
}

static void hfpll_disable(struct scalable *sc, bool skip_regulators)
{
	writel_relaxed(0, sc->hfpll_base + drv.hfpll_data->mode_offset);

	if (!skip_regulators) {
		
		disable_rpm_vreg(&sc->vreg[VREG_HFPLL_B]);
		disable_rpm_vreg(&sc->vreg[VREG_HFPLL_A]);
	}
}

static void hfpll_set_rate(struct scalable *sc, const struct core_speed *tgt_s)
{
	writel_relaxed(tgt_s->pll_l_val,
		sc->hfpll_base + drv.hfpll_data->l_offset);
}

static unsigned int compute_l2_level(struct scalable *sc, unsigned int vote_l)
{
	unsigned int new_l = 0;
	int cpu;

	
	sc->l2_vote = vote_l;
	for_each_present_cpu(cpu)
		new_l = max(new_l, drv.scalable[cpu].l2_vote);

	return new_l;
}

static void set_bus_bw(unsigned int bw)
{
	int ret;

	
	ret = msm_bus_scale_client_update_request(drv.bus_perf_client, bw);
	if (ret)
		dev_err(drv.dev, "bandwidth request failed (%d)\n", ret);
}

static void set_speed(struct scalable *sc, const struct core_speed *tgt_s,
	bool skip_regulators)
{
	const struct core_speed *strt_s = sc->cur_speed;

	if (strt_s->src == HFPLL && tgt_s->src == HFPLL) {
		set_pri_clk_src(sc, PRI_SRC_SEL_SEC_SRC);

		
		hfpll_disable(sc, true);
		hfpll_set_rate(sc, tgt_s);
		hfpll_enable(sc, true);

		
		set_pri_clk_src(sc, tgt_s->pri_src_sel);
	} else if (strt_s->src == HFPLL && tgt_s->src != HFPLL) {
		set_pri_clk_src(sc, tgt_s->pri_src_sel);
		hfpll_disable(sc, skip_regulators);
	} else if (strt_s->src != HFPLL && tgt_s->src == HFPLL) {
		hfpll_set_rate(sc, tgt_s);
		hfpll_enable(sc, skip_regulators);
		set_pri_clk_src(sc, tgt_s->pri_src_sel);
	}

	sc->cur_speed = tgt_s;
}

struct vdd_data {
	int vdd_mem;
	int vdd_dig;
	int vdd_core;
	int ua_core;
};

static int increase_vdd(int cpu, struct vdd_data *data,
			enum setrate_reason reason)
{
	struct scalable *sc = &drv.scalable[cpu];
	int rc;

	if (data->vdd_mem > sc->vreg[VREG_MEM].cur_vdd) {
		rc = rpm_regulator_set_voltage(sc->vreg[VREG_MEM].rpm_reg,
				data->vdd_mem, sc->vreg[VREG_MEM].max_vdd);
		if (rc) {
			dev_err(drv.dev,
				"vdd_mem (cpu%d) increase failed (%d)\n",
				cpu, rc);
			return rc;
		}
		 sc->vreg[VREG_MEM].cur_vdd = data->vdd_mem;
	}

	
	if (data->vdd_dig > sc->vreg[VREG_DIG].cur_vdd) {
		rc = rpm_regulator_set_voltage(sc->vreg[VREG_DIG].rpm_reg,
				data->vdd_dig, sc->vreg[VREG_DIG].max_vdd);
		if (rc) {
			dev_err(drv.dev,
				"vdd_dig (cpu%d) increase failed (%d)\n",
				cpu, rc);
			return rc;
		}
		sc->vreg[VREG_DIG].cur_vdd = data->vdd_dig;
	}

	
	if (data->ua_core > sc->vreg[VREG_CORE].cur_ua) {
		rc = regulator_set_optimum_mode(sc->vreg[VREG_CORE].reg,
						data->ua_core);
		if (rc < 0) {
			dev_err(drv.dev, "regulator_set_optimum_mode(%s) failed (%d)\n",
				sc->vreg[VREG_CORE].name, rc);
			return rc;
		}
		sc->vreg[VREG_CORE].cur_ua = data->ua_core;
	}

	if (data->vdd_core > sc->vreg[VREG_CORE].cur_vdd
			&& reason != SETRATE_HOTPLUG) {
		rc = regulator_set_voltage(sc->vreg[VREG_CORE].reg,
				data->vdd_core, sc->vreg[VREG_CORE].max_vdd);
		if (rc) {
			dev_err(drv.dev,
				"vdd_core (cpu%d) increase failed (%d)\n",
				cpu, rc);
			return rc;
		}
		sc->vreg[VREG_CORE].cur_vdd = data->vdd_core;
	}

	return 0;
}

static void decrease_vdd(int cpu, struct vdd_data *data,
			 enum setrate_reason reason)
{
	struct scalable *sc = &drv.scalable[cpu];
	int ret;

	if (data->vdd_core < sc->vreg[VREG_CORE].cur_vdd
			&& reason != SETRATE_HOTPLUG) {
		ret = regulator_set_voltage(sc->vreg[VREG_CORE].reg,
				data->vdd_core, sc->vreg[VREG_CORE].max_vdd);
		if (ret) {
			dev_err(drv.dev,
				"vdd_core (cpu%d) decrease failed (%d)\n",
				cpu, ret);
			return;
		}
		sc->vreg[VREG_CORE].cur_vdd = data->vdd_core;
	}

	
	if (data->ua_core < sc->vreg[VREG_CORE].cur_ua) {
		ret = regulator_set_optimum_mode(sc->vreg[VREG_CORE].reg,
						data->ua_core);
		if (ret < 0) {
			dev_err(drv.dev, "regulator_set_optimum_mode(%s) failed (%d)\n",
				sc->vreg[VREG_CORE].name, ret);
			return;
		}
		sc->vreg[VREG_CORE].cur_ua = data->ua_core;
	}

	
	if (data->vdd_dig < sc->vreg[VREG_DIG].cur_vdd) {
		ret = rpm_regulator_set_voltage(sc->vreg[VREG_DIG].rpm_reg,
				data->vdd_dig, sc->vreg[VREG_DIG].max_vdd);
		if (ret) {
			dev_err(drv.dev,
				"vdd_dig (cpu%d) decrease failed (%d)\n",
				cpu, ret);
			return;
		}
		sc->vreg[VREG_DIG].cur_vdd = data->vdd_dig;
	}

	if (data->vdd_mem < sc->vreg[VREG_MEM].cur_vdd) {
		ret = rpm_regulator_set_voltage(sc->vreg[VREG_MEM].rpm_reg,
				data->vdd_mem, sc->vreg[VREG_MEM].max_vdd);
		if (ret) {
			dev_err(drv.dev,
				"vdd_mem (cpu%d) decrease failed (%d)\n",
				cpu, ret);
			return;
		}
		sc->vreg[VREG_MEM].cur_vdd = data->vdd_mem;
	}
}

static int calculate_vdd_mem(const struct acpu_level *tgt)
{
	return drv.l2_freq_tbl[tgt->l2_level].vdd_mem;
}

static int get_src_dig(const struct core_speed *s)
{
	const int *hfpll_vdd = drv.hfpll_data->vdd;
	const u32 low_vdd_l_max = drv.hfpll_data->low_vdd_l_max;
	const u32 nom_vdd_l_max = drv.hfpll_data->nom_vdd_l_max;

	if (s->src != HFPLL)
		return hfpll_vdd[HFPLL_VDD_NONE];
	else if (s->pll_l_val > nom_vdd_l_max)
		return hfpll_vdd[HFPLL_VDD_HIGH];
	else if (s->pll_l_val > low_vdd_l_max)
		return hfpll_vdd[HFPLL_VDD_NOM];
	else
		return hfpll_vdd[HFPLL_VDD_LOW];
}

static int calculate_vdd_dig(const struct acpu_level *tgt)
{
	int l2_pll_vdd_dig, cpu_pll_vdd_dig;

	l2_pll_vdd_dig = get_src_dig(&drv.l2_freq_tbl[tgt->l2_level].speed);
	cpu_pll_vdd_dig = get_src_dig(&tgt->speed);

	return max(drv.l2_freq_tbl[tgt->l2_level].vdd_dig,
		   max(l2_pll_vdd_dig, cpu_pll_vdd_dig));
}

static bool enable_boost = true;
module_param_named(boost, enable_boost, bool, S_IRUGO | S_IWUSR);

static int calculate_vdd_core(const struct acpu_level *tgt)
{
	return tgt->vdd_core + (enable_boost ? drv.boost_uv : 0);
}

static DEFINE_MUTEX(l2_regulator_lock);
static int l2_vreg_count;

static int enable_l2_regulators(void)
{
	int ret = 0;

	mutex_lock(&l2_regulator_lock);
	if (l2_vreg_count == 0) {
		ret = enable_rpm_vreg(&drv.scalable[L2].vreg[VREG_HFPLL_A]);
		if (ret)
			goto out;
		ret = enable_rpm_vreg(&drv.scalable[L2].vreg[VREG_HFPLL_B]);
		if (ret) {
			disable_rpm_vreg(&drv.scalable[L2].vreg[VREG_HFPLL_A]);
			goto out;
		}
	}
	l2_vreg_count++;
out:
	mutex_unlock(&l2_regulator_lock);

	return ret;
}

static void disable_l2_regulators(void)
{
	mutex_lock(&l2_regulator_lock);

	if (WARN(!l2_vreg_count, "L2 regulator votes are unbalanced!"))
		goto out;

	if (l2_vreg_count == 1) {
		disable_rpm_vreg(&drv.scalable[L2].vreg[VREG_HFPLL_B]);
		disable_rpm_vreg(&drv.scalable[L2].vreg[VREG_HFPLL_A]);
	}
	l2_vreg_count--;
out:
	mutex_unlock(&l2_regulator_lock);
}

static int acpuclk_krait_set_rate(int cpu, unsigned long rate,
				  enum setrate_reason reason)
{
	const struct core_speed *strt_acpu_s, *tgt_acpu_s;
	const struct acpu_level *tgt;
	int tgt_l2_l;
	enum src_id prev_l2_src = NUM_SRC_ID;
	struct vdd_data vdd_data;
	bool skip_regulators;
	int rc = 0;

	set_acpuclk_foot_print(cpu, 0x1);

	if (cpu > num_possible_cpus())
		return -EINVAL;

	if (reason == SETRATE_CPUFREQ || reason == SETRATE_HOTPLUG)
		mutex_lock(&driver_lock);

	set_acpuclk_foot_print(cpu, 0x2);

	strt_acpu_s = drv.scalable[cpu].cur_speed;

	
	if (rate == strt_acpu_s->khz)
		goto out;

	
	for (tgt = drv.acpu_freq_tbl; tgt->speed.khz != 0; tgt++) {
		if (tgt->speed.khz == rate) {
			tgt_acpu_s = &tgt->speed;
			break;
		}
	}
	if (tgt->speed.khz == 0) {
		rc = -EINVAL;
		goto out;
	}

	
	vdd_data.vdd_mem  = calculate_vdd_mem(tgt);
	vdd_data.vdd_dig  = calculate_vdd_dig(tgt);
	vdd_data.vdd_core = calculate_vdd_core(tgt);
	vdd_data.ua_core = tgt->ua_core;

	
	if (reason == SETRATE_CPUFREQ && drv.scalable[cpu].avs_enabled) {
		AVS_DISABLE(cpu);
		drv.scalable[cpu].avs_enabled = false;
	}

	
	if (reason == SETRATE_CPUFREQ || reason == SETRATE_HOTPLUG) {
		rc = increase_vdd(cpu, &vdd_data, reason);
		udelay(60);
		set_acpuclk_foot_print(cpu, 0x3);

		if (rc)
			goto out;

		prev_l2_src =
			drv.l2_freq_tbl[drv.scalable[cpu].l2_vote].speed.src;
		
		if (drv.l2_freq_tbl[tgt->l2_level].speed.src == HFPLL) {
			rc = enable_l2_regulators();

			set_acpuclk_foot_print(cpu, 0x4);

			if (rc)
				goto out;
		}
	}

	dev_dbg(drv.dev, "Switching from ACPU%d rate %lu KHz -> %lu KHz\n",
		cpu, strt_acpu_s->khz, tgt_acpu_s->khz);

	skip_regulators = (reason == SETRATE_PC);

	
	set_speed(&drv.scalable[cpu], tgt_acpu_s, skip_regulators);

	set_acpuclk_cpu_freq_foot_print(cpu, tgt_acpu_s->khz);
	set_acpuclk_foot_print(cpu, 0x5);


	spin_lock(&l2_lock);
	tgt_l2_l = compute_l2_level(&drv.scalable[cpu], tgt->l2_level);
	set_speed(&drv.scalable[L2],
			&drv.l2_freq_tbl[tgt_l2_l].speed, true);

	set_acpuclk_L2_freq_foot_print(drv.l2_freq_tbl[tgt_l2_l].speed.khz);
	set_acpuclk_foot_print(cpu, 0x6);

	spin_unlock(&l2_lock);

	
	if (reason == SETRATE_PC || reason == SETRATE_SWFI)
		goto out;

	if (prev_l2_src == HFPLL)
		disable_l2_regulators();

	set_acpuclk_foot_print(cpu, 0x7);

	
	set_bus_bw(drv.l2_freq_tbl[tgt_l2_l].bw_level);

	set_acpuclk_foot_print(cpu, 0x8);

	
	decrease_vdd(cpu, &vdd_data, reason);

	set_acpuclk_foot_print(cpu, 0x9);

	
	if (reason == SETRATE_CPUFREQ && tgt->avsdscr_setting) {
		AVS_ENABLE(cpu, tgt->avsdscr_setting);
		drv.scalable[cpu].avs_enabled = true;
	}

	dev_dbg(drv.dev, "ACPU%d speed change complete\n", cpu);

out:
	if (reason == SETRATE_CPUFREQ || reason == SETRATE_HOTPLUG)
		mutex_unlock(&driver_lock);

	set_acpuclk_foot_print(cpu, 0xA);

	return rc;
}

static struct acpuclk_data acpuclk_krait_data = {
	.set_rate = acpuclk_krait_set_rate,
	.get_rate = acpuclk_krait_get_rate,
};

#ifdef CONFIG_APQ8064_ONLY 
unsigned long acpuclk_krait_power_collapse(void)
{
	unsigned long rate = acpuclk_get_rate(smp_processor_id());
	acpuclk_krait_set_rate(smp_processor_id(), 384000, SETRATE_PC);
	return rate;
}
#endif

static void __init hfpll_init(struct scalable *sc,
			      const struct core_speed *tgt_s)
{
	dev_dbg(drv.dev, "Initializing HFPLL%d\n", sc - drv.scalable);

	
	hfpll_disable(sc, true);

	
	writel_relaxed(drv.hfpll_data->config_val,
		       sc->hfpll_base + drv.hfpll_data->config_offset);
	writel_relaxed(0, sc->hfpll_base + drv.hfpll_data->m_offset);
	writel_relaxed(1, sc->hfpll_base + drv.hfpll_data->n_offset);

	
	if (drv.hfpll_data->has_droop_ctl)
		writel_relaxed(drv.hfpll_data->droop_val,
			       sc->hfpll_base + drv.hfpll_data->droop_offset);

	
	hfpll_set_rate(sc, tgt_s);
	hfpll_enable(sc, false);
}

static int rpm_regulator_init(struct scalable *sc, enum vregs vreg,
					 int vdd, bool enable)
{
	int ret;

	if (!sc->vreg[vreg].name)
		return 0;

	sc->vreg[vreg].rpm_reg = rpm_regulator_get(drv.dev,
						   sc->vreg[vreg].name);

	if (IS_ERR(sc->vreg[vreg].rpm_reg)) {
		ret = PTR_ERR(sc->vreg[vreg].rpm_reg);
		dev_err(drv.dev, "rpm_regulator_get(%s) failed (%d)\n",
			sc->vreg[vreg].name, ret);
		goto err_get;
	}

	ret = rpm_regulator_set_voltage(sc->vreg[vreg].rpm_reg, vdd,
					sc->vreg[vreg].max_vdd);
	if (ret) {
		dev_err(drv.dev, "%s initialization failed (%d)\n",
			sc->vreg[vreg].name, ret);
		goto err_conf;
	}
	sc->vreg[vreg].cur_vdd = vdd;

	if (enable) {
		ret = enable_rpm_vreg(&sc->vreg[vreg]);
		if (ret)
			goto err_conf;
	}

	return 0;

err_conf:
	rpm_regulator_put(sc->vreg[vreg].rpm_reg);
err_get:
	return ret;
}

static void rpm_regulator_cleanup(struct scalable *sc,
						enum vregs vreg)
{
	if (!sc->vreg[vreg].rpm_reg)
		return;

	disable_rpm_vreg(&sc->vreg[vreg]);
	rpm_regulator_put(sc->vreg[vreg].rpm_reg);
}

static int regulator_init(struct scalable *sc,
				const struct acpu_level *acpu_level)
{
	int ret, vdd_mem, vdd_dig, vdd_core;

	vdd_mem = calculate_vdd_mem(acpu_level);
	ret = rpm_regulator_init(sc, VREG_MEM, vdd_mem, true);
	if (ret)
		goto err_mem;

	vdd_dig = calculate_vdd_dig(acpu_level);
	ret = rpm_regulator_init(sc, VREG_DIG, vdd_dig, true);
	if (ret)
		goto err_dig;

	ret = rpm_regulator_init(sc, VREG_HFPLL_A,
			   sc->vreg[VREG_HFPLL_A].max_vdd, false);
	if (ret)
		goto err_hfpll_a;
	ret = rpm_regulator_init(sc, VREG_HFPLL_B,
			   sc->vreg[VREG_HFPLL_B].max_vdd, false);
	if (ret)
		goto err_hfpll_b;

	
	sc->vreg[VREG_CORE].reg = regulator_get(drv.dev,
				  sc->vreg[VREG_CORE].name);
	if (IS_ERR(sc->vreg[VREG_CORE].reg)) {
		ret = PTR_ERR(sc->vreg[VREG_CORE].reg);
		dev_err(drv.dev, "regulator_get(%s) failed (%d)\n",
			sc->vreg[VREG_CORE].name, ret);
		goto err_core_get;
	}
	ret = regulator_set_optimum_mode(sc->vreg[VREG_CORE].reg,
					 acpu_level->ua_core);
	if (ret < 0) {
		dev_err(drv.dev, "regulator_set_optimum_mode(%s) failed (%d)\n",
			sc->vreg[VREG_CORE].name, ret);
		goto err_core_conf;
	}
	sc->vreg[VREG_CORE].cur_ua = acpu_level->ua_core;
	vdd_core = calculate_vdd_core(acpu_level);
	ret = regulator_set_voltage(sc->vreg[VREG_CORE].reg, vdd_core,
				    sc->vreg[VREG_CORE].max_vdd);
	if (ret) {
		dev_err(drv.dev, "regulator_set_voltage(%s) (%d)\n",
			sc->vreg[VREG_CORE].name, ret);
		goto err_core_conf;
	}
	sc->vreg[VREG_CORE].cur_vdd = vdd_core;
	ret = regulator_enable(sc->vreg[VREG_CORE].reg);
	if (ret) {
		dev_err(drv.dev, "regulator_enable(%s) failed (%d)\n",
			sc->vreg[VREG_CORE].name, ret);
		goto err_core_conf;
	}

	if (drv.l2_freq_tbl[acpu_level->l2_level].speed.src == HFPLL)
		l2_vreg_count++;

	return 0;

err_core_conf:
	regulator_put(sc->vreg[VREG_CORE].reg);
err_core_get:
	rpm_regulator_cleanup(sc, VREG_HFPLL_B);
err_hfpll_b:
	rpm_regulator_cleanup(sc, VREG_HFPLL_A);
err_hfpll_a:
	rpm_regulator_cleanup(sc, VREG_DIG);
err_dig:
	rpm_regulator_cleanup(sc, VREG_MEM);
err_mem:
	return ret;
}

static void regulator_cleanup(struct scalable *sc)
{
	regulator_disable(sc->vreg[VREG_CORE].reg);
	regulator_put(sc->vreg[VREG_CORE].reg);
	rpm_regulator_cleanup(sc, VREG_HFPLL_B);
	rpm_regulator_cleanup(sc, VREG_HFPLL_A);
	rpm_regulator_cleanup(sc, VREG_DIG);
	rpm_regulator_cleanup(sc, VREG_MEM);
}

static int init_clock_sources(struct scalable *sc,
					 const struct core_speed *tgt_s)
{
	u32 regval;
	void __iomem *aux_reg;

	
	if (sc->aux_clk_sel_phys) {
		aux_reg = ioremap(sc->aux_clk_sel_phys, 4);
		if (!aux_reg)
			return -ENOMEM;
		writel_relaxed(sc->aux_clk_sel, aux_reg);
		iounmap(aux_reg);
	}

	
	set_sec_clk_src(sc, sc->sec_clk_sel);
	set_pri_clk_src(sc, PRI_SRC_SEL_SEC_SRC);
	hfpll_init(sc, tgt_s);

	
	regval = get_l2_indirect_reg(sc->l2cpmr_iaddr);
	regval &= ~(0x3 << 6);
	set_l2_indirect_reg(sc->l2cpmr_iaddr, regval);

	
	set_pri_clk_src(sc, tgt_s->pri_src_sel);
	sc->cur_speed = tgt_s;

	return 0;
}

static void fill_cur_core_speed(struct core_speed *s,
					  struct scalable *sc)
{
	s->pri_src_sel = get_l2_indirect_reg(sc->l2cpmr_iaddr) & 0x3;
	s->pll_l_val = readl_relaxed(sc->hfpll_base + drv.hfpll_data->l_offset);
}

static bool speed_equal(const struct core_speed *s1,
				  const struct core_speed *s2)
{
	return (s1->pri_src_sel == s2->pri_src_sel &&
		s1->pll_l_val == s2->pll_l_val);
}

static const struct acpu_level *find_cur_acpu_level(int cpu)
{
	struct scalable *sc = &drv.scalable[cpu];
	const struct acpu_level *l;
	struct core_speed cur_speed;

	fill_cur_core_speed(&cur_speed, sc);
	for (l = drv.acpu_freq_tbl; l->speed.khz != 0; l++)
		if (speed_equal(&l->speed, &cur_speed))
			return l;
	return NULL;
}

static const struct l2_level __init *find_cur_l2_level(void)
{
	struct scalable *sc = &drv.scalable[L2];
	const struct l2_level *l;
	struct core_speed cur_speed;

	fill_cur_core_speed(&cur_speed, sc);
	for (l = drv.l2_freq_tbl; l->speed.khz != 0; l++)
		if (speed_equal(&l->speed, &cur_speed))
			return l;
	return NULL;
}

static const struct acpu_level *find_min_acpu_level(void)
{
	struct acpu_level *l;

	for (l = drv.acpu_freq_tbl; l->speed.khz != 0; l++)
		if (l->use_for_scaling)
			return l;

	return NULL;
}

static int per_cpu_init(int cpu)
{
	struct scalable *sc = &drv.scalable[cpu];
	const struct acpu_level *acpu_level;
	int ret;

	sc->hfpll_base = ioremap(sc->hfpll_phys_base, SZ_32);
	if (!sc->hfpll_base) {
		ret = -ENOMEM;
		goto err_ioremap;
	}

	acpu_level = find_cur_acpu_level(cpu);
	if (!acpu_level) {
		acpu_level = find_min_acpu_level();
		if (!acpu_level) {
			ret = -ENODEV;
			goto err_table;
		}
		dev_dbg(drv.dev, "CPU%d is running at an unknown rate. Defaulting to %lu KHz.\n",
			cpu, acpu_level->speed.khz);
	} else {
		dev_dbg(drv.dev, "CPU%d is running at %lu KHz\n", cpu,
			acpu_level->speed.khz);
	}

	ret = regulator_init(sc, acpu_level);
	if (ret)
		goto err_regulators;

	ret = init_clock_sources(sc, &acpu_level->speed);
	if (ret)
		goto err_clocks;

	sc->l2_vote = acpu_level->l2_level;
	sc->initialized = true;

	return 0;

err_clocks:
	regulator_cleanup(sc);
err_regulators:
err_table:
	iounmap(sc->hfpll_base);
err_ioremap:
	return ret;
}

static void __init bus_init(const struct l2_level *l2_level)
{
	int ret;

	drv.bus_perf_client = msm_bus_scale_register_client(drv.bus_scale);
	if (!drv.bus_perf_client) {
		dev_err(drv.dev, "unable to register bus client\n");
		BUG();
	}

	ret = msm_bus_scale_client_update_request(drv.bus_perf_client,
			l2_level->bw_level);
	if (ret)
		dev_err(drv.dev, "initial bandwidth req failed (%d)\n", ret);
}

#ifdef CONFIG_CPU_VOLTAGE_TABLE

#define HFPLL_MIN_VDD		 600000
#define HFPLL_MAX_VDD		1450000

ssize_t acpuclk_get_vdd_levels_str(char *buf) {

	int i, len = 0;

	if (buf) {
		mutex_lock(&driver_lock);

		for (i = 0; drv.acpu_freq_tbl[i].speed.khz; i++) {
			/* updated to use uv required by 8x60 architecture - faux123 */
			len += sprintf(buf + len, "%8lu: %8d\n", drv.acpu_freq_tbl[i].speed.khz,
				drv.acpu_freq_tbl[i].vdd_core );
		}

		mutex_unlock(&driver_lock);
	}
	return len;
}

/* updated to use uv required by 8x60 architecture - faux123 */
void acpuclk_set_vdd(unsigned int khz, int vdd_uv) {

	int i;
	unsigned int new_vdd_uv;

	mutex_lock(&driver_lock);

	for (i = 0; drv.acpu_freq_tbl[i].speed.khz; i++) {
		if (khz == 0)
			new_vdd_uv = min(max((unsigned int)(drv.acpu_freq_tbl[i].vdd_core + vdd_uv),
				(unsigned int)HFPLL_MIN_VDD), (unsigned int)HFPLL_MAX_VDD);
		else if ( drv.acpu_freq_tbl[i].speed.khz == khz)
			new_vdd_uv = min(max((unsigned int)vdd_uv,
				(unsigned int)HFPLL_MIN_VDD), (unsigned int)HFPLL_MAX_VDD);
		else 
			continue;

		drv.acpu_freq_tbl[i].vdd_core = new_vdd_uv;
	}
	pr_warn("faux123: user voltage table modified!\n");
	mutex_unlock(&driver_lock);
}
#endif	/* CONFIG_CPU_VOTALGE_TABLE */

#ifdef CONFIG_CPU_FREQ_MSM
static struct cpufreq_frequency_table freq_table[NR_CPUS][FREQ_TABLE_SIZE];

static void __init cpufreq_table_init(void)
{
	int cpu;

	for_each_possible_cpu(cpu) {
		int i, freq_cnt = 0;
		
		for (i = 0; drv.acpu_freq_tbl[i].speed.khz != 0
				&& freq_cnt < ARRAY_SIZE(*freq_table); i++) {
			if (drv.acpu_freq_tbl[i].use_for_scaling) {
				freq_table[cpu][freq_cnt].index = freq_cnt;
				freq_table[cpu][freq_cnt].frequency
					= drv.acpu_freq_tbl[i].speed.khz;
				freq_cnt++;
			}
		}
		
		BUG_ON(drv.acpu_freq_tbl[i].speed.khz != 0);

		freq_table[cpu][freq_cnt].index = freq_cnt;
		freq_table[cpu][freq_cnt].frequency = CPUFREQ_TABLE_END;

		dev_info(drv.dev, "CPU%d: %d frequencies supported\n",
			cpu, freq_cnt);

		
		cpufreq_frequency_table_get_attr(freq_table[cpu], cpu);
	}
}
#else
static void __init cpufreq_table_init(void) {}
#endif

static int acpuclk_cpu_callback(struct notifier_block *nfb,
					    unsigned long action, void *hcpu)
{
	static int prev_khz[NR_CPUS];
	int rc, cpu = (int)hcpu;
	struct scalable *sc = &drv.scalable[cpu];
	unsigned long hot_unplug_khz = acpuclk_krait_data.power_collapse_khz;

	switch (action & ~CPU_TASKS_FROZEN) {
	case CPU_DEAD:
		prev_khz[cpu] = acpuclk_krait_get_rate(cpu);
		
	case CPU_UP_CANCELED:
		acpuclk_krait_set_rate(cpu, hot_unplug_khz, SETRATE_HOTPLUG);
		regulator_set_optimum_mode(sc->vreg[VREG_CORE].reg, 0);
		break;
	case CPU_UP_PREPARE:
		if (!sc->initialized) {
			rc = per_cpu_init(cpu);
			if (rc)
				return NOTIFY_BAD;
			break;
		}
		if (WARN_ON(!prev_khz[cpu]))
			return NOTIFY_BAD;
		rc = regulator_set_optimum_mode(sc->vreg[VREG_CORE].reg,
						sc->vreg[VREG_CORE].cur_ua);
		if (rc < 0)
			return NOTIFY_BAD;
		acpuclk_krait_set_rate(cpu, prev_khz[cpu], SETRATE_HOTPLUG);
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

static struct notifier_block __cpuinitdata acpuclk_cpu_notifier = {
	.notifier_call = acpuclk_cpu_callback,
};

static const int krait_needs_vmin(void)
{
	switch (read_cpuid_id()) {
	case 0x511F04D0: 
	case 0x511F04D1: 
	case 0x510F06F0: 
		return 1;
	default:
		return 0;
	};
}

static void krait_apply_vmin(struct acpu_level *tbl)
{
	for (; tbl->speed.khz != 0; tbl++) {
		if (tbl->vdd_core < 1150000)
			tbl->vdd_core = 1150000;
		tbl->avsdscr_setting = 0;
	}
}

uint32_t global_speed_bin;
int __init get_speed_bin(u32 pte_efuse)
{
	uint32_t speed_bin;

	speed_bin = pte_efuse & 0xF;
	if (speed_bin == 0xF)
		speed_bin = (pte_efuse >> 4) & 0xF;

	if (speed_bin == 0xF) {
		speed_bin = 0;
		dev_warn(drv.dev, "SPEED BIN: Defaulting to %d\n", speed_bin);
	} else {
		dev_info(drv.dev, "SPEED BIN: %d\n", speed_bin);
	}

	global_speed_bin = speed_bin;
	return speed_bin;
}

int msm_get_cpu_speed_bin(void)
{
	return global_speed_bin;
}

static int __init get_pvs_bin(u32 pte_efuse)
{
	uint32_t pvs_bin;

	pvs_bin = (pte_efuse >> 10) & 0x7;
	if (pvs_bin == 0x7)
		pvs_bin = (pte_efuse >> 13) & 0x7;

	if (pvs_bin == 0x7) {
		pvs_bin = 0;
		dev_warn(drv.dev, "ACPU PVS: Defaulting to %d\n", pvs_bin);
	} else {
		dev_info(drv.dev, "ACPU PVS: %d\n", pvs_bin);
	}

	return pvs_bin;
}

static int speed_bin_filter = 0;
static unsigned long speed_bin_freq = 0;
void set_acpu_speedbin_filter_freq(int bin, unsigned long freq)
{
	speed_bin_filter = bin;
	speed_bin_freq = freq;
}

static struct pvs_table * __init select_freq_plan(u32 pte_efuse_phys,
			struct pvs_table (*pvs_tables)[NUM_PVS])
{
	void __iomem *pte_efuse;
	u32 pte_efuse_val;
#ifdef CONFIG_ACPU_CUSTOM_FREQ_SUPPORT
	struct pvs_table *pvs;
	struct acpu_level *l;
#endif

	pte_efuse = ioremap(pte_efuse_phys, 4);
	if (!pte_efuse) {
		dev_err(drv.dev, "Unable to map QFPROM base\n");
		return NULL;
	}

	pte_efuse_val = readl_relaxed(pte_efuse);
	iounmap(pte_efuse);

	
	drv.speed_bin = get_speed_bin(pte_efuse_val);
	drv.pvs_bin = get_pvs_bin(pte_efuse_val);

#ifdef CONFIG_ACPU_SPEED_BIN_FREQ_SUPPORT
	if ((speed_bin_freq!=0) && (drv.speed_bin == speed_bin_filter))
		acpu_max_freq = speed_bin_freq;
#endif

#ifdef CONFIG_ACPU_CUSTOM_FREQ_SUPPORT
	pvs = &pvs_tables[drv.speed_bin][drv.pvs_bin];
	BUG_ON(!pvs->table);

	
	if (acpu_max_freq) {
		for (l = pvs->table; l->speed.khz != 0; l++) {
			if (l->speed.khz >= acpu_max_freq) {
				if(l->speed.khz == acpu_max_freq)
					l++;
				for (; l->speed.khz != 0; l++)
					l->use_for_scaling = 0;
				break;
			}
		}
	}

	return pvs;
#else
	return &pvs_tables[drv.speed_bin][drv.pvs_bin];
#endif
}

static void __init drv_data_init(struct device *dev,
				 const struct acpuclk_krait_params *params)
{
	struct pvs_table *pvs;

	drv.dev = dev;
	drv.scalable = kmemdup(params->scalable, params->scalable_size,
				GFP_KERNEL);
	BUG_ON(!drv.scalable);

	drv.hfpll_data = kmemdup(params->hfpll_data, sizeof(*drv.hfpll_data),
				GFP_KERNEL);
	BUG_ON(!drv.hfpll_data);

	drv.l2_freq_tbl = kmemdup(params->l2_freq_tbl, params->l2_freq_tbl_size,
				GFP_KERNEL);
	BUG_ON(!drv.l2_freq_tbl);

	drv.bus_scale = kmemdup(params->bus_scale, sizeof(*drv.bus_scale),
				GFP_KERNEL);
	BUG_ON(!drv.bus_scale);
	drv.bus_scale->usecase = kmemdup(drv.bus_scale->usecase,
		drv.bus_scale->num_usecases * sizeof(*drv.bus_scale->usecase),
		GFP_KERNEL);
	BUG_ON(!drv.bus_scale->usecase);

	pvs = select_freq_plan(params->pte_efuse_phys, params->pvs_tables);
	BUG_ON(!pvs->table);

	drv.acpu_freq_tbl = kmemdup(pvs->table, pvs->size, GFP_KERNEL);
	BUG_ON(!drv.acpu_freq_tbl);
	drv.boost_uv = pvs->boost_uv;

	acpuclk_krait_data.power_collapse_khz = params->stby_khz;
	acpuclk_krait_data.wait_for_irq_khz = params->stby_khz;
}

static void __init hw_init(void)
{
	struct scalable *l2 = &drv.scalable[L2];
	const struct l2_level *l2_level;
	int cpu, rc;

	if (krait_needs_vmin())
		krait_apply_vmin(drv.acpu_freq_tbl);

	l2->hfpll_base = ioremap(l2->hfpll_phys_base, SZ_32);
	BUG_ON(!l2->hfpll_base);

	rc = rpm_regulator_init(l2, VREG_HFPLL_A,
				l2->vreg[VREG_HFPLL_A].max_vdd, false);
	BUG_ON(rc);
	rc = rpm_regulator_init(l2, VREG_HFPLL_B,
				l2->vreg[VREG_HFPLL_B].max_vdd, false);
	BUG_ON(rc);

	l2_level = find_cur_l2_level();
	if (!l2_level) {
		l2_level = drv.l2_freq_tbl;
		dev_dbg(drv.dev, "L2 is running at an unknown rate. Defaulting to %lu KHz.\n",
			l2_level->speed.khz);
	} else {
		dev_dbg(drv.dev, "L2 is running at %lu KHz\n",
			l2_level->speed.khz);
	}

	rc = init_clock_sources(l2, &l2_level->speed);
	BUG_ON(rc);

	for_each_online_cpu(cpu) {
		rc = per_cpu_init(cpu);
		BUG_ON(rc);
	}

	bus_init(l2_level);
}

int __init acpuclk_krait_init(struct device *dev,
			      const struct acpuclk_krait_params *params)
{
	drv_data_init(dev, params);
	hw_init();

	cpufreq_table_init();
	acpuclk_register(&acpuclk_krait_data);
	register_hotcpu_notifier(&acpuclk_cpu_notifier);

	acpuclk_krait_debug_init(&drv);

	return 0;
}
