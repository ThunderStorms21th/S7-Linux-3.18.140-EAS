/*
 * Scheduler Energy Driver
 *
 * Scheduler Energy Driver updates capacities in the scheduler group energy array.
 * The array contains power cost at each CPU operating points so energy aware
 * scheduler (EAS) can utilize it for task placement.
 *
 * Required properties:
 * - compatible:	Must be "sched-energy"
 *
 * Example:
 *	energy-costs {
 *		compatible = "sched-energy";
 *	}
 */

#define pr_fmt(fmt) "sched-energy: " fmt

#include <linux/gfp.h>
#include <linux/of.h>
#include <linux/printk.h>
#include <linux/sched.h>
#include <linux/sched_energy.h>
#include <linux/stddef.h>
#include <linux/cpu.h>
#include <linux/cpuset.h>
#include <linux/pm_opp.h>
#include <linux/platform_device.h>

static int update_topology;

/*
 * Ideally this should be arch specific implementation,
 * let's define here to help rebuild sched_domain with new capacities.
 */
int arch_update_cpu_topology(void)
{
	return update_topology;
}

static int sched_energy_probe(struct platform_device *pdev)
{
	unsigned long max_freq = 0;
	int max_efficiency = INT_MIN;
	int cpu;
	unsigned long *max_frequencies = NULL;
	int ret;

	max_frequencies = kmalloc_array(nr_cpu_ids, sizeof(unsigned long),
					GFP_KERNEL);
	if (!max_frequencies) {
		ret = -ENOMEM;
		goto exit;
	}

	/*
	 * Find system max possible frequency and max frequencies for each
	 * CPUs.
	 */
	for_each_possible_cpu(cpu) {
		struct device *cpu_dev;
		struct dev_pm_opp *opp;
		int efficiency = arch_get_cpu_efficiency(cpu);

		max_efficiency = max(efficiency, max_efficiency);

		cpu_dev = get_cpu_device(cpu);
		if (IS_ERR_OR_NULL(cpu_dev)) {
			if (!cpu_dev)
				ret = -EINVAL;
			else
				ret = PTR_ERR(cpu_dev);
			goto exit;
		}

		max_frequencies[cpu] = ULONG_MAX;

		rcu_read_lock();
		opp = dev_pm_opp_find_freq_floor(cpu_dev,
						 &max_frequencies[cpu]);
		if (IS_ERR_OR_NULL(opp)) {
			if (!opp || PTR_ERR(opp) == -ENODEV)
				ret = -EPROBE_DEFER;
			else
				ret = PTR_ERR(opp);
			goto exit_rcu_unlock;
		}
		rcu_read_unlock();

		/* Convert HZ to KHZ */
		max_frequencies[cpu] /= 1000;
		max_freq = max(max_freq, max_frequencies[cpu]);
	}

	/* update capacity in energy model */
	for_each_possible_cpu(cpu) {
		unsigned long cpu_max_cap;
		struct sched_group_energy *sge_l0, *sge;
		int efficiency = arch_get_cpu_efficiency(cpu);

		cpu_max_cap = DIV_ROUND_UP(SCHED_CAPACITY_SCALE *
					   max_frequencies[cpu], max_freq);
		cpu_max_cap = DIV_ROUND_UP(cpu_max_cap * efficiency,
					   max_efficiency);

		/*
		 * All the cap_states have same frequency table so use
		 * SD_LEVEL0's.
		 */
		sge_l0 = sge_array[cpu][SD_LEVEL0];
		if (sge_l0 && sge_l0->nr_cap_states > 0) {
			int i;
			int ncapstates = sge_l0->nr_cap_states;

			for (i = 0; i < ncapstates; i++) {
				int sd_level;
				unsigned long freq, cap;

				/*
				 * Energy model can contain more frequency
				 * steps than actual for multiple speedbin
				 * support. Ceil the max capacity with actual
				 * one.
				 */
				freq = min(sge_l0->cap_states[i].frequency,
					   max_frequencies[cpu]);
				cap = DIV_ROUND_UP(cpu_max_cap * freq,
						   max_frequencies[cpu]);

				for_each_possible_sd_level(sd_level) {
					sge = sge_array[cpu][sd_level];
					if (!sge)
						break;
					sge->cap_states[i].cap = cap;
				}

				dev_dbg(&pdev->dev,
					"cpu=%d freq=%ld cap=%ld power_d0=%ld\n",
					cpu, freq, sge_l0->cap_states[i].cap,
					sge_l0->cap_states[i].power);
			}

			dev_info(&pdev->dev,
				"cpu=%d eff=%d [freq=%ld cap=%ld power_d0=%ld] -> [freq=%ld cap=%ld power_d0=%ld]\n",
				cpu, efficiency,
				sge_l0->cap_states[0].frequency,
				sge_l0->cap_states[0].cap,
				sge_l0->cap_states[0].power,
				sge_l0->cap_states[ncapstates - 1].frequency,
				sge_l0->cap_states[ncapstates - 1].cap,
				sge_l0->cap_states[ncapstates - 1].power
				);
		}

		dev_dbg(&pdev->dev,
			"cpu=%d efficiency=%d max_frequency=%ld max_efficiency=%d cpu_max_capacity=%ld\n",
			cpu, efficiency, max_frequencies[cpu], max_efficiency,
			cpu_max_cap);

		arch_update_cpu_capacity(cpu);
	}

	kfree(max_frequencies);

	if (is_sge_valid) {
		/*
		 * Sched_domains might have built with default cpu capacity
		 * values on bootup.
		 *
		 * Let's rebuild them again with actual cpu capacities.
		 * And partition_sched_domain() expects update in cpu topology
		 * to rebuild the domains, so make it satisfied..
		 */
		update_topology = 1;
		rebuild_sched_domains();
		update_topology = 0;
	}
	
	dev_info(&pdev->dev, "Sched-energy-costs capacity updated\n");
	return 0;

exit_rcu_unlock:
	rcu_read_unlock();

exit:
	if (ret != -EPROBE_DEFER)
		dev_err(&pdev->dev, "error=%d\n", ret);

	kfree(max_frequencies);
	return ret;
}

static const struct of_device_id of_sched_energy_dt[] = {
	{
		.compatible = "sched-energy",
	},
	{ }
};

static struct platform_driver energy_driver = {
	.driver = {
		.name = "sched-energy",
		.of_match_table = of_sched_energy_dt,
	},
	.probe = sched_energy_probe,
};

static int __init sched_energy_init(void)
{
	return platform_driver_register(&energy_driver);
}
subsys_initcall(sched_energy_init);
