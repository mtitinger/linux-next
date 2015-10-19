/*
 * CPU Generic PM Domain.
 *
 * Copyright (C) 2015 Linaro Ltd.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#define DEBUG

#include <linux/kernel.h>
#include <linux/export.h>
#include <linux/cpu.h>
#include <linux/cpu_pm.h>
#include <linux/cpu-pd.h>
#include <linux/device.h>
#include <linux/pm_runtime.h>
#include <linux/platform_device.h>
#include <linux/rculist.h>
#include <linux/slab.h>

#define NAME_MAX 36

/* List of CPU PM domains we care about */
static LIST_HEAD(of_cpu_pd_list);
static DEFINE_SPINLOCK(cpu_pd_list_lock);

static inline
struct cpu_pm_domain *to_cpu_pd(struct generic_pm_domain *d)
{
	struct cpu_pm_domain *pd = NULL;

	rcu_read_lock();
	list_for_each_entry_rcu(pd, &of_cpu_pd_list, link)
		if (pd->genpd == d)
			break;
	rcu_read_unlock();

	return pd;
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

static int of_pm_domain_attach_cpus(void)
{
	int cpuid, ret;

	/* Find any CPU nodes with a phandle to this power domain */
	for_each_possible_cpu(cpuid) {
		struct device *cpu_dev;

		cpu_dev = get_cpu_device(cpuid);
		if (!cpu_dev) {
			pr_warn("%s: Unable to get device for CPU%d\n",
					__func__, cpuid);
			return -ENODEV;
		}

		if (cpu_online(cpuid)) {
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
			smp_call_function_single(cpuid, run_cpu, NULL, true);
		} else {
			pm_runtime_set_suspended(cpu_dev);
		}
		pm_runtime_enable(cpu_dev);

		/*
		 * We attempt to attach the device to genpd again. We would
		 * have failed in our earlier attempt to attach to the domain
		 * provider as the CPU device would not have been IRQ safe,
		 * while the domain is defined as IRQ safe. IRQ safe domains
		 * can only have IRQ safe devices.
		 */
		ret = genpd_dev_pm_attach(cpu_dev);
		if (ret) {
			dev_warn(cpu_dev,
				"%s: Unable to attach to power-domain: %d\n",
				__func__, ret);
			pm_runtime_disable(cpu_dev);
		} else
			dev_dbg(cpu_dev, "Attached to PM domain\n");
	}

	return 0;
}

static int cpu_hotplug(struct notifier_block *nb,
			unsigned long action, void *data)
{
	/* Execute CPU runtime PM on that CPU */
	switch (action) {
	case CPU_DYING:
	case CPU_DYING_FROZEN:
		cpu_pm_runtime_suspend();
		break;
	case CPU_STARTING:
	case CPU_STARTING_FROZEN:
		cpu_pm_runtime_resume();
		break;
	default:
		break;
	}

	return NOTIFY_OK;
}

/**
 * of_register_cpu_pm_domain() - Register the CPU PM domain with GenPD
 * framework
 * @dn: PM domain provider device node
 * @pd: The CPU PM domain that has been initialized
 *
 * This can be used by the platform code to setup the ->genpd of the @pd
 * The platform can also set up its callbacks in the ->plat_ops.
 */
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
	pm_genpd_init_simple(pd->genpd, &simple_qos_governor, false);
	of_genpd_add_provider_simple(dn, pd->genpd);

	/* Attach the CPUs to the CPU PM domain */
	ret = of_pm_domain_attach_cpus();
	if (ret) {
		of_genpd_del_provider(dn);
		return ret;
	}

	hotcpu_notifier(cpu_hotplug, 0)
	return 0;
}
EXPORT_SYMBOL(of_register_cpu_pm_domain);

/**
 * of_init_cpu_pm_domain() - Initialize a CPU PM domain using the CPU pd
 * provided
 * @dn: PM domain provider device node
 * @ops: CPU PM domain platform specific ops for callback
 *
 * This is a single step initialize the CPU PM domain with defaults,
 * also register the genpd and attach CPUs to the genpd.
 */
int of_init_cpu_pm_domain(struct device_node *dn, struct cpu_pm_ops *ops)
{
	struct cpu_pm_domain *pd;
	int ret;

	if (!of_device_is_available(dn))
		return -ENODEV;

	pd = kzalloc(sizeof(*pd), GFP_KERNEL);
	if (!pd)
		return -ENOMEM;

	pd->genpd = kzalloc(sizeof(*(pd->genpd)), GFP_KERNEL);
	if (!pd->genpd) {
		kfree(pd);
		return -ENOMEM;
	}

	if (ops) {
		pd->plat_ops.power_off = ops->power_off;
		pd->plat_ops.power_on = ops->power_on;
	}

	pd->genpd->name = kstrndup(dn->full_name, NAME_MAX, GFP_KERNEL);

	ret = of_register_cpu_pm_domain(dn, pd);
	if (ret) {
		kfree(pd->genpd);
		kfree(pd);
		kfree(pd->genpd->name);
	}

	return ret;
}
EXPORT_SYMBOL(of_init_cpu_pm_domain);


static int __init of_cpu_pd_init(void)
{
	struct device_node *dn;
	int ret;

	for_each_compatible_node(dn, NULL, "cpu,pd") {

		if (!of_device_is_available(dn))
			continue;

		ret = of_init_cpu_pm_domain(dn, NULL);
		if (!ret)
			return ret;

	}
	return 0;
}
device_initcall(of_cpu_pd_init);
