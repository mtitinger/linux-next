/*
 * CPU Generic PM Domain.
 *
 * Copyright (C) 2015 Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/cpu-pd.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>
#include <linux/rculist.h>
#include <linux/slab.h>

#define CPU_PD_NAME_MAX 36

/* List of CPU PM domains we care about */
static LIST_HEAD(of_cpu_pd_list);
static DEFINE_SPINLOCK(cpu_pd_list_lock);

static inline
struct cpu_pm_domain *to_cpu_pd(struct generic_pm_domain *d)
{
	struct cpu_pm_domain *pd;
	struct cpu_pm_domain *res = NULL;

	rcu_read_lock();
	list_for_each_entry_rcu(pd, &of_cpu_pd_list, link)
		if (pd->genpd == d) {
			res = pd;
			break;
		}
	rcu_read_unlock();

	return res;
}

static int cpu_pd_power_off(struct generic_pm_domain *genpd)
{
	struct cpu_pm_domain *pd = to_cpu_pd(genpd);

	if (pd->plat_ops.power_off)
		pd->plat_ops.power_off(genpd);

	/*
	 * Notify CPU PM domain power down
	 * TODO: Call the notificated directly from here.
	 */
	cpu_cluster_pm_enter();

	return 0;
}

static int cpu_pd_power_on(struct generic_pm_domain *genpd)
{
	struct cpu_pm_domain *pd = to_cpu_pd(genpd);

	if (pd->plat_ops.power_on)
		pd->plat_ops.power_on(genpd);

	/* Notify CPU PM domain power up */
	cpu_cluster_pm_exit();

	return 0;
}

static void run_cpu(void *unused)
{
	struct device *cpu_dev = get_cpu_device(smp_processor_id());

	/* We are running, increment the usage count */
	pm_runtime_get_noresume(cpu_dev);
}

static int of_pm_domain_attach_cpu(int cpu)
{
	int ret;
	struct device *cpu_dev;

	cpu_dev = get_cpu_device(cpu);
	if (!cpu_dev) {
		pr_warn("%s: Unable to get device for CPU%d\n",
				__func__, cpu);
		return -ENODEV;
	}

	if (cpu_online(cpu)) {
		pm_runtime_set_active(cpu_dev);
		/*
		 * Execute the below on that 'cpu' to ensure that the
		 * reference counting is correct. It's possible that
		 * while this code is executing, the 'cpu' may be
		 * powered down, but we may incorrectly increment the
		 * usage. By executing the get_cpu on the 'cpu',
		 * we can ensure that the 'cpu' and its usage count are
		 * matched.
		 */
		smp_call_function_single(cpu, run_cpu, NULL, true);
	} else {
		pm_runtime_set_suspended(cpu_dev);
	}

	ret = genpd_dev_pm_attach(cpu_dev);
	if (ret) {
		dev_warn(cpu_dev,
			"%s: Unable to attach to power-domain: %d\n",
			__func__, ret);
	} else {
		pm_runtime_enable(cpu_dev);
		dev_dbg(cpu_dev, "Attached CPU%d to domain\n", cpu);
	}

	return ret;
}

static int cpu_hotplug(struct notifier_block *nb,
			unsigned long action, void *data)
{
	struct device *dev = get_cpu_device(smp_processor_id());

	/* Execute CPU runtime PM on that CPU */
	switch (action) {
	case CPU_DYING:
	case CPU_DYING_FROZEN:
		pm_runtime_put_sync_suspend(dev);
		pm_runtime_disable(dev);
		break;
	case CPU_STARTING:
	case CPU_STARTING_FROZEN:
		pm_runtime_enable(dev);
		pm_runtime_get_sync(dev);
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

int of_register_cpu_pm_domain(struct device_node *dn,
		struct cpu_pm_domain *pd)
{
	int ret;

	if (!pd || !pd->genpd)
		return -EINVAL;

	/*
	 * The platform should not set up the genpd callbacks.
	 * They should setup the pd->plat_ops instead.
	 */
	WARN_ON(pd->genpd->power_off);
	WARN_ON(pd->genpd->power_on);

	pd->genpd->power_off = cpu_pd_power_off;
	pd->genpd->power_on = cpu_pd_power_on;
	pd->genpd->flags |= GENPD_FLAG_IRQ_SAFE;

	INIT_LIST_HEAD_RCU(&pd->link);
	spin_lock(&cpu_pd_list_lock);
	list_add_rcu(&pd->link, &of_cpu_pd_list);
	spin_unlock(&cpu_pd_list_lock);
	pd->dn = dn;

	/* Register the CPU genpd */
	pr_debug("adding %s as CPU PM domain.\n", pd->genpd->name);
	ret = pm_genpd_init(pd->genpd, &simple_qos_governor, false);
	if (ret) {
		pr_err("Unable to initialize domain %s\n", dn->full_name);
		return ret;
	}

	ret = of_genpd_add_provider_simple(dn, pd->genpd);
	if (ret)
		pr_warn("Unable to add genpd %s as provider\n",
				pd->genpd->name);

	return ret;
}

/**
 * of_init_cpu_pm_domain() - Initialize a CPU PM domain using the CPU pd
 * provided
 * @dn: PM domain provider device node
 * @ops: CPU PM domain platform specific ops for callback
 */
static struct generic_pm_domain *of_init_cpu_pm_domain(struct device_node *dn,
				const struct cpu_pd_ops *ops)
{
	struct cpu_pm_domain *pd;
	struct generic_pm_domain *genpd;
	int ret = -ENOMEM;

	if (!of_device_is_available(dn))
		return ERR_PTR(-ENODEV);

	genpd = kzalloc(sizeof(*(genpd)), GFP_KERNEL);
	if (!genpd)
		goto no_mem;

	/* use name, not full_name for find-by-name to work. */
	genpd->name = kstrndup(dn->name, CPU_PD_NAME_MAX, GFP_KERNEL);
	if (!genpd->name)
		goto no_mem;

	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (!pd)
		goto no_mem;

	pd->genpd = genpd;

	if (ops) {
		pd->plat_ops.power_off = ops->power_off;
		pd->plat_ops.power_on = ops->power_on;
	}

	ret = of_register_cpu_pm_domain(dn, pd);
	if (ret)
		goto no_mem;

	return pd->genpd;
no_mem:

	kfree(pd->genpd->name);
	kfree(pd->genpd);
	kfree(pd);
	return ERR_PTR(ret);
}

static struct generic_pm_domain *of_get_cpu_domain(struct device_node *dn,
		const struct cpu_pd_ops *ops, int cpu)
{
	struct of_phandle_args args;
	struct generic_pm_domain *genpd, *parent;
	int ret;

	/* Do we have this domain? If not, create the domain */
	args.np = dn;
	args.args_count = 0;

	genpd = of_genpd_get_from_provider(&args);
	if (!IS_ERR(genpd))
		goto skip_parent;

	genpd = of_init_cpu_pm_domain(dn, ops);
	if (IS_ERR(genpd))
		return genpd;

	/* Is there a domain provider for this domain? */
	ret = of_parse_phandle_with_args(dn, "power-domains",
			"#power-domain-cells", 0, &args);
	of_node_put(dn);
	if (ret < 0)
		goto skip_parent;

	/* Find its parent and attach this domain to it, recursively */
	parent = of_get_cpu_domain(args.np, ops, cpu);
	if (IS_ERR(parent)) {
		struct cpu_pm_domain *cpu_pd, *parent_cpu_pd;

		ret = pm_genpd_add_subdomain(genpd, parent);
		if (ret) {
			pr_err("%s: Unable to add sub-domain (%s) to parent (%s)\n err: %d",
					__func__, genpd->name, parent->name,
					ret);
			return ERR_PTR(ret);
		}

		/*
		 * Reference parent domain for easy access.
		 * Note: We could be attached to a domain that is not a CPU PM domain,
		 * in that case dont reference the parent.
		 */
		cpu_pd = to_cpu_pd(genpd);
		parent_cpu_pd = to_cpu_pd(parent);

		if (cpu_pd && parent_cpu_pd)
			cpu_pd->parent = parent_cpu_pd;
	}

skip_parent:
	return genpd;
}

/**
 * of_setup_cpu_pm_domains() - Setup the CPU domains for all CPUs
 *
 * @ops: The PM domain suspend/resume ops for all the domains in the topology
 */
int of_setup_cpu_pm_domains(const struct cpu_pd_ops *ops)
{
	struct device_node *dn;
	struct generic_pm_domain *genpd;
	struct cpu_pm_domain *cpu_pd;
	int cpu, cpus_attached = 0;
	int ret = 0;

	for_each_possible_cpu(cpu) {
		dn = of_get_cpu_node(cpu, NULL);
		if (!dn)
			continue;

		dn = of_parse_phandle(dn, "power-domains", 0);
		if (!dn)
			continue;
		of_node_put(dn);

		genpd = of_get_cpu_domain(dn, ops, cpu);
		if (IS_ERR(genpd))
			return PTR_ERR(genpd);

		/*
		 * Sanity check: Ensure that genpd was not created by this
		 * code. If not, then the power_on and power_off callbacks
		 * would already be setup and we will not get callbacks.
		 */
		cpu_pd = to_cpu_pd(genpd);
		if (!cpu_pd) {
			pr_err("%s: Genpd was created outside CPU PM domains\n",
					__func__);
			return -ENOENT;
		}

		ret = of_pm_domain_attach_cpu(cpu);
		if (ret)
			return ret;

		cpus_attached++;
	}

	if (cpus_attached > 0)
		hotcpu_notifier(cpu_hotplug, 0);

	return ret;
}
EXPORT_SYMBOL(of_setup_cpu_pm_domains);
