/*
 *  drivers/cpufreq/cpufreq_sublime.c
 *
 *  Copyright (C)  2001 Russell King
 *            (C)  2003 Venkatesh Pallipadi <venkatesh.pallipadi@intel.com>.
 *                      Jun Nakajima <jun.nakajima@intel.com>
 *            (C)  2009 Alexander Clouter <alex@digriz.org.uk>
 *            (C)  2015 Dela Anthonio
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <linux/cpufreq.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/kernel_stat.h>
#include <linux/kobject.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/notifier.h>
#include <linux/percpu-defs.h>
#include <linux/slab.h>
#include <linux/sysfs.h>
#include <linux/types.h>
#include <linux/touchboost.h>

#include "cpufreq_governor.h"

/* Sublime governor macros */
#define DEF_HIGHSPEED_FREQUENCY_UP_THRESHOLD (95)
#define DEF_FREQUENCY_UP_THRESHOLD           (70)
#define DEF_FREQUENCY_DOWN_THRESHOLD         (30)
#define DEF_HIGHSPEED_FREQUENCY              (1836000)
#define MIN_INPUT_EVENT_FREQUENCY            (1428000)
#define INPUT_EVENT_DURATION                 (50000)
#define BASE_FREQUENCY_DELTA                 (10000)
#define MAX_BASE_FREQUENCY_DELTA             (50000)
#define FREQUENCY_DELTA_RESISTANCE           (200)
#define MINIMUM_SAMPLING_RATE                (15000)

static DEFINE_PER_CPU(struct sb_cpu_dbs_info_s, sb_cpu_dbs_info);

/* Return a value to add to the current CPU frequency. The value returned is
 * directly proportional to the difference between the current frequency and a
 * maximum frequency, and also in direct proportion to the load.
 */
static inline unsigned int freq_boost(unsigned int freq_target_delta,
                                      unsigned int load)
{
        return (BASE_FREQUENCY_DELTA + freq_target_delta)
            * load / FREQUENCY_DELTA_RESISTANCE;
}

/* Return a value to subtract from the current CPU frequency. The value returned
 * is directly proportional to the difference between the current frequency and
 * a minimum frequency, and also in direct proportion to the load.
 */
static inline unsigned int freq_reduction(unsigned int freq_target_delta,
                                          unsigned int load)
{
        return  (BASE_FREQUENCY_DELTA + freq_target_delta)
                * (110 - load) / FREQUENCY_DELTA_RESISTANCE;
}

/*
 * Every sampling_rate, if current idle time is less than 30% (default),
 * try to increase the frequency. Every sampling_rate if the current idle
 * time is more than 70% (default), try to decrease the frequency.
 */
static void sb_check_cpu(int cpu, unsigned int load)
{
	struct sb_cpu_dbs_info_s *dbs_info = &per_cpu(sb_cpu_dbs_info, cpu);
	struct cpufreq_policy *policy = dbs_info->cdbs.cur_policy;
	struct dbs_data *dbs_data = policy->governor_data;
	struct sb_dbs_tuners *sb_tuners = dbs_data->tuners;
	unsigned int freq_target;
        unsigned int freq_target_delta;

        /* Check for input event */
        if (input_event_boost(INPUT_EVENT_DURATION)){
            /* Ensure that the frequency is at least the minimum input event
             * frequency. If the load is high, then scale the frequency directly
             * proportional to the load to ensure a responsive frequency. */
                freq_target = ((policy->max - policy->min)
                               * load / 100) + policy->min;
                dbs_info->requested_freq = max(freq_target,
                                               MIN_INPUT_EVENT_FREQUENCY);

                __cpufreq_driver_target(policy, dbs_info->requested_freq,
                                        dbs_info->requested_freq > policy->cur ?
                                        CPUFREQ_RELATION_H : CPUFREQ_RELATION_L);

                return;
        }

	/* Check for frequency decrease */
	if (load < sb_tuners->down_threshold) {

		// break out early if the frequency is set to the minimum
		if (policy->cur == policy->min)
			return;

                freq_target_delta = policy->cur - policy->min;
                freq_target = freq_reduction(freq_target_delta, load);
                if (dbs_info->requested_freq > freq_target) {
                        dbs_info->requested_freq -= freq_target;
                        if (dbs_info->requested_freq < policy->min)
                                dbs_info->requested_freq = policy->min;
                }
                else
                    dbs_info->requested_freq = policy->min;

		__cpufreq_driver_target(policy, dbs_info->requested_freq,
                                        CPUFREQ_RELATION_L);
		return;
	}

	/* Check for high-speed frequency increase */
	if (load > sb_tuners->highspeed_up_threshold) {

	    // Stop if the current speed is already the maximum
	    if (dbs_info->requested_freq == policy->max)
                    return;

            freq_target_delta = policy->max - policy->cur;
            dbs_info->requested_freq += freq_boost(freq_target_delta, load);

            // Make sure the requested frequency is at most the maximum frequency
            if (dbs_info->requested_freq > policy->max)
                    dbs_info->requested_freq = policy->max;

	    __cpufreq_driver_target(policy, dbs_info->requested_freq,
                                    CPUFREQ_RELATION_H);
	    return;
	}

	/* Check for frequency increase */
	 if (load >= sb_tuners->up_threshold) {

	    /* break out early if the high-speed freq is already set */
	    if (dbs_info->requested_freq == sb_tuners->highspeed_freq)
	        return;

            if (policy->cur < sb_tuners->highspeed_freq) {
                    freq_target_delta = sb_tuners->highspeed_freq - policy->cur;
                    dbs_info->requested_freq += freq_boost(freq_target_delta,
                                                           load);
                    if (dbs_info->requested_freq > sb_tuners->highspeed_freq)
                            dbs_info->requested_freq = sb_tuners->highspeed_freq;
            }
            else
                    dbs_info->requested_freq = sb_tuners->highspeed_freq;

            if (dbs_info->requested_freq > policy->max)                    dbs_info->requested_freq = policy->max;


	    __cpufreq_driver_target(policy, dbs_info->requested_freq,
                                    CPUFREQ_RELATION_H);
	    return;
	}

}

static void sb_dbs_timer(struct work_struct *work)
{
	struct sb_cpu_dbs_info_s *dbs_info = container_of(work,
			struct sb_cpu_dbs_info_s, cdbs.work.work);
	unsigned int cpu = dbs_info->cdbs.cur_policy->cpu;
	struct sb_cpu_dbs_info_s *core_dbs_info = &per_cpu(sb_cpu_dbs_info,
			cpu);
	struct dbs_data *dbs_data = dbs_info->cdbs.cur_policy->governor_data;
	struct sb_dbs_tuners *sb_tuners = dbs_data->tuners;
	int delay = delay_for_sampling_rate(sb_tuners->sampling_rate);
	bool modify_all = true;

	mutex_lock(&core_dbs_info->cdbs.timer_mutex);
	if (!need_load_eval(&core_dbs_info->cdbs, sb_tuners->sampling_rate))
		modify_all = false;
	else
		dbs_check_cpu(dbs_data, cpu);

	gov_queue_work(dbs_data, dbs_info->cdbs.cur_policy, delay, modify_all);
	mutex_unlock(&core_dbs_info->cdbs.timer_mutex);
}

static int dbs_cpufreq_notifier(struct notifier_block *nb, unsigned long val,
		void *data)
{
	struct cpufreq_freqs *freq = data;
	struct sb_cpu_dbs_info_s *dbs_info =
					&per_cpu(sb_cpu_dbs_info, freq->cpu);
	struct cpufreq_policy *policy;

	if (!dbs_info->enable)
		return 0;

	policy = dbs_info->cdbs.cur_policy;

	/*
	 * we only care if our internally tracked frequency moves outside the 'valid'
	 * ranges of frequency available to us otherwise we do not change it
	*/
	if (dbs_info->requested_freq > policy->max
			|| dbs_info->requested_freq < policy->min)
		dbs_info->requested_freq = freq->new;

	return 0;
}

/************************** sysfs interface ************************/
static struct common_dbs_data sb_dbs_cdata;

static ssize_t store_sampling_rate(struct dbs_data *dbs_data, const char *buf,
		size_t count)
{
	struct sb_dbs_tuners *sb_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1)
		return -EINVAL;

	sb_tuners->sampling_rate = max(input, dbs_data->min_sampling_rate);
	return count;
}

static ssize_t store_highspeed_up_threshold(struct dbs_data *dbs_data, const char *buf,
                                  size_t count)
{
    struct sb_dbs_tuners *sb_tuners = dbs_data->tuners;
    unsigned int input;
    int ret;
    ret = sscanf(buf, "%u", &input);

    if (ret != 1 || input > 100 || input <= sb_tuners->up_threshold)
        return -EINVAL;

    sb_tuners->highspeed_up_threshold = input;
    return count;
}

static ssize_t store_up_threshold(struct dbs_data *dbs_data, const char *buf,
		size_t count)
{
	struct sb_dbs_tuners *sb_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	if (ret != 1 || input > 100 || input <= sb_tuners->down_threshold)
		return -EINVAL;

	sb_tuners->up_threshold = input;
	return count;
}

static ssize_t store_down_threshold(struct dbs_data *dbs_data, const char *buf,
		size_t count)
{
	struct sb_dbs_tuners *sb_tuners = dbs_data->tuners;
	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

	/* cannot be lower than 11 otherwise frequency will not fall */
	if (ret != 1 || input < 11 || input > 100 ||
			input >= sb_tuners->up_threshold)
		return -EINVAL;

	sb_tuners->down_threshold = input;
	return count;
}

static ssize_t store_highspeed_freq(struct dbs_data *dbs_data, const char *buf,
		size_t count)
{
	struct sb_dbs_tuners *sb_tuners = dbs_data->tuners;
        struct cpufreq_policy *policy;

	unsigned int input;
	int ret;
	ret = sscanf(buf, "%u", &input);

        if (ret != 1)
                return -EINVAL;

        if (input < policy->min)
                input = policy->min;

        else if (input > policy->max)
                input  = policy->max;

        sb_tuners->highspeed_freq = input;
	return count;
}

show_store_one(sb, sampling_rate);
show_store_one(sb, highspeed_up_threshold);
show_store_one(sb, up_threshold);
show_store_one(sb, down_threshold);
show_store_one(sb, highspeed_freq);
declare_show_sampling_rate_min(sb);

gov_sys_pol_attr_rw(sampling_rate);
gov_sys_pol_attr_rw(highspeed_up_threshold);
gov_sys_pol_attr_rw(up_threshold);
gov_sys_pol_attr_rw(down_threshold);
gov_sys_pol_attr_ro(highspeed_freq);
gov_sys_pol_attr_ro(sampling_rate_min);

static struct attribute *dbs_attributes_gov_sys[] = {
	&sampling_rate_gov_sys.attr,
	&sampling_rate_min_gov_sys.attr,
	&highspeed_up_threshold_gov_sys.attr,
	&up_threshold_gov_sys.attr,
	&down_threshold_gov_sys.attr,
	&highspeed_freq_gov_sys.attr,
	NULL
};

static struct attribute_group sb_attr_group_gov_sys = {
	.attrs = dbs_attributes_gov_sys,
	.name = "sublime",
};

static struct attribute *dbs_attributes_gov_pol[] = {
	&sampling_rate_min_gov_pol.attr,
	&sampling_rate_gov_pol.attr,
	&highspeed_up_threshold_gov_pol.attr,
	&up_threshold_gov_pol.attr,
	&down_threshold_gov_pol.attr,
	&highspeed_freq_gov_pol.attr,
	NULL
};

static struct attribute_group sb_attr_group_gov_pol = {
	.attrs = dbs_attributes_gov_pol,
	.name = "sublime",
};

/************************** sysfs end ************************/

static int sb_init(struct dbs_data *dbs_data)
{
	struct sb_dbs_tuners *tuners;

	tuners = kzalloc(sizeof(struct sb_dbs_tuners), GFP_KERNEL);
	if (!tuners) {
		pr_err("%s: kzalloc failed\n", __func__);
		return -ENOMEM;
	}
        tuners->highspeed_up_threshold = DEF_HIGHSPEED_FREQUENCY_UP_THRESHOLD;
	tuners->up_threshold = DEF_FREQUENCY_UP_THRESHOLD;
	tuners->down_threshold = DEF_FREQUENCY_DOWN_THRESHOLD;
	tuners->highspeed_freq = DEF_HIGHSPEED_FREQUENCY;

	dbs_data->tuners = tuners;
	dbs_data->min_sampling_rate = MINIMUM_SAMPLING_RATE;
	mutex_init(&dbs_data->mutex);
	return 0;
}

static void sb_exit(struct dbs_data *dbs_data)
{
	kfree(dbs_data->tuners);
}

define_get_cpu_dbs_routines(sb_cpu_dbs_info);

static struct notifier_block sb_cpufreq_notifier_block = {
	.notifier_call = dbs_cpufreq_notifier,
};

static struct sb_ops sb_ops = {
	.notifier_block = &sb_cpufreq_notifier_block,
};

static struct common_dbs_data sb_dbs_cdata = {
	.governor = GOV_SUBLIME,
	.attr_group_gov_sys = &sb_attr_group_gov_sys,
	.attr_group_gov_pol = &sb_attr_group_gov_pol,
	.get_cpu_cdbs = get_cpu_cdbs,
	.get_cpu_dbs_info_s = get_cpu_dbs_info_s,
	.gov_dbs_timer = sb_dbs_timer,
	.gov_check_cpu = sb_check_cpu,
	.gov_ops = &sb_ops,
	.init = sb_init,
	.exit = sb_exit,
};

static int sb_cpufreq_governor_dbs(struct cpufreq_policy *policy,
				   unsigned int event)
{
	return cpufreq_governor_dbs(policy, &sb_dbs_cdata, event);
}

#ifndef CONFIG_CPU_FREQ_DEFAULT_GOV_SUBLIME
static
#endif
struct cpufreq_governor cpufreq_gov_sublime = {
	.name			= "sublime",
	.governor		= sb_cpufreq_governor_dbs,
	.max_transition_latency	= TRANSITION_LATENCY_LIMIT,
	.owner			= THIS_MODULE,
};

static int __init cpufreq_gov_dbs_init(void)
{
	return cpufreq_register_governor(&cpufreq_gov_sublime);
}

static void __exit cpufreq_gov_dbs_exit(void)
{
	cpufreq_unregister_governor(&cpufreq_gov_sublime);
}

MODULE_AUTHOR("Dela Anthonio");
MODULE_DESCRIPTION("'cpufreq_sublime' - A dynamic cpufreq governor for "
		"Low Latency Frequency Transition capable processors "
		"optimized for devices with the Tegra K1 processor"
		"and have limited battry life");
MODULE_LICENSE("GPL");

#ifdef CONFIG_CPU_FREQ_DEFAULT_GOV_SUBLIME
fs_initcall(cpufreq_gov_dbs_init);
#else
module_init(cpufreq_gov_dbs_init);
#endif
module_exit(cpufreq_gov_dbs_exit);
