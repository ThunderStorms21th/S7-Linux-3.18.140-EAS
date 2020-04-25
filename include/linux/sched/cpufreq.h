/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_CPUFREQ_H
#define _LINUX_SCHED_CPUFREQ_H

#include <linux/types.h>

/*
 * Interface between cpufreq drivers and the scheduler:
 */

#define SCHED_CPUFREQ_IOWAIT	(1U << 0)
#define SCHED_CPUFREQ_MIGRATION	(1U << 1)

#ifdef CONFIG_CPU_FREQ
void cpufreq_remove_update_util_hook(int cpu);

static inline unsigned long map_util_freq(unsigned long util,
					unsigned long freq, unsigned long cap)
{
	return (freq + (freq >> 2)) * util / cap;
}
#endif /* CONFIG_CPU_FREQ */

#endif /* _LINUX_SCHED_CPUFREQ_H */
