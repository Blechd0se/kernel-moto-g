/*
 * Copyright (c) 2013, Francisco Franco <franciscofranco.1990@gmail.com>. 
 *
 * Small algorithm changes for more performance
 * Copyright (c) 2014, Alexander Christ <alex.christ@hotmail.de>
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
 * Simple no bullshit hot[un]plug driver for SMP
 */

/*
 * TODO   - Enable sysfs entries for better tuning
 *        - Add Thermal Throttle Driver
 */

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/notifier.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/sysfs.h>
#include <linux/kobject.h>

#define DEFAULT_FIRST_LEVEL	85
#define DEFAULT_SECOND_LEVEL	75
#define HIGH_LOAD_COUNTER	25
#define SAMPLING_RATE_MS	500

struct cpu_stats
{
	unsigned int default_first_level;
	unsigned int default_second_level;
	unsigned long timestamp;

	/* For the three hot-plug-able Cores */
	unsigned int counter[2];
	unsigned int cpu_load_stats[3];
} stats;

struct cpu_load_data {
        u64 prev_cpu_idle;
        u64 prev_cpu_wall;
};

static DEFINE_PER_CPU(struct cpu_load_data, cpuload);

static struct workqueue_struct *wq;
static struct delayed_work decide_hotplug;

static unsigned int sampling_rate;

static inline int get_cpu_load(unsigned int cpu)
{
	struct cpu_load_data *pcpu = &per_cpu(cpuload, cpu);
	struct cpufreq_policy policy;
	u64 cur_wall_time, cur_idle_time;
	unsigned int idle_time, wall_time;
	unsigned int cur_load;

	cpufreq_get_policy(&policy, cpu);

	cur_idle_time = get_cpu_idle_time(cpu, &cur_wall_time, true);

	wall_time = (unsigned int) (cur_wall_time - pcpu->prev_cpu_wall);
	pcpu->prev_cpu_wall = cur_wall_time;

	idle_time = (unsigned int) (cur_idle_time - pcpu->prev_cpu_idle);
	pcpu->prev_cpu_idle = cur_idle_time;

	if (unlikely(!wall_time || wall_time < idle_time))
		return 0;

	cur_load = 100 * (wall_time - idle_time) / wall_time;

	return (cur_load * policy.cur) / policy.max;
}

/*
 * Returns the average load for all currently onlined cpus
 */

static int get_load_for_all_cpu(void)
{
	int cpu;
	int load = 0;
	struct cpu_stats *s = &stats;

	for_each_online_cpu(cpu) {

		s->cpu_load_stats[cpu] = 0;
		s->cpu_load_stats[cpu] = get_cpu_load(cpu);
		load = load + s->cpu_load_stats[cpu];
	}
	
	load = (unsigned int) (load / num_online_cpus());	
	return load;
}

/*
 * Calculates the load for a given cpu
 */ 

static void calculate_load_for_cpu(int cpu) 
{
	struct cpufreq_policy policy;
	struct cpu_stats *s = &stats;

	for_each_online_cpu(cpu) {
		cpufreq_get_policy(&policy, cpu);
		/*  
		 * We are above our threshold, so update our counter for cpu.
		 * Consider this only, if we are on our max frequency
		 */
		if (get_cpu_load(cpu) >= s->default_first_level &&
			get_load_for_all_cpu() >= s->default_second_level
			&& likely(s->counter[cpu] < HIGH_LOAD_COUNTER)
			&& cpufreq_quick_get(cpu) == policy.max) {
				s->counter[cpu] += 2;
		}

		else {
			if (s->counter[cpu] > 0)
				s->counter[cpu]--;
		}

		/* Reset CPU */
		if (cpu)
			break;
	}	

}

/**
 * Simple load based decision algorithm to determ
 * how many cores should be on- or offlined
 */

static void __ref decide_hotplug_func(struct work_struct *work)
{
	int i, j;
	int current_cpu = 0;
	struct cpu_stats *s = &stats;

	/* Do load calculation for each cpu counter */

	for (i = 0, j = 2; i < 2; i++, j++) {
		calculate_load_for_cpu(i);

		if (s->counter[i] >= 10 && get_load_for_all_cpu() >= s->default_second_level
			&& (num_online_cpus() != num_possible_cpus())) {
			if (!cpu_online(j)) {
				printk("[Hot-Plug]: CPU%u ready for onlining\n", j);
				cpu_up(j);
				s->timestamp = jiffies;
			}
		}
		else {
			/* Prevent fast on-/offlining */ 
			if (time_is_after_jiffies(s->timestamp + (HZ * 3))) {		
				/* Rearm you work_queue immediatly */
				queue_delayed_work_on(0, wq, &decide_hotplug, sampling_rate);
			}
			else {
				if (s->counter[i] > 0 && cpu_online(j)) {
						
						/*
						 * Decide which core should be offlined
						 */

						if (get_cpu_load(j) > get_cpu_load(j+1) 
								&& cpu_online(j+1))
							current_cpu = j + 1;
						else if (get_cpu_load(j) > get_cpu_load(j-1) 
								&& cpu_online(j-1) && j-1 != 1)
							current_cpu = j - 1;
						else
							current_cpu = j;
						
						printk("[Hot-Plug]: CPU%u ready for offlining\n", current_cpu);	
						cpu_down(current_cpu);
						s->timestamp = jiffies;
				}
			}
		}
	}
	
	/* Make a dedicated work_queue */
	queue_delayed_work_on(0, wq, &decide_hotplug, sampling_rate);
}

/* Start sysfs attributes */

static ssize_t show_sampling_rate(struct kobject *kobj,
					struct attribute *attr, char *buf)
{
	return sprintf(buf, "%u\n", sampling_rate);
}

static ssize_t store_sampling_rate(struct kobject *kobj,
					 struct attribute *attr,
					 const char *buf, size_t count)
{
	unsigned int val;

	sscanf(buf, "%u", &val);

	sampling_rate = val;
	return count;
}
static struct global_attr sampling_rate_attr = __ATTR(sampling_rate, 0666,
					show_sampling_rate, store_sampling_rate);

static struct attribute *falcon_hotplug_attributes[] = 
{
	&sampling_rate_attr.attr,
	NULL
};

static struct attribute_group hotplug_attr_group = 
{
	.attrs  = falcon_hotplug_attributes,
};

static struct kobject *hotplug_control_kobj;

int __init falcon_hotplug_init(void)
{
	int ret;
	struct cpu_stats *s = &stats;
	pr_info("Falcon Hotplug driver started.\n");
    
	/* init everything here */
	s->default_first_level = DEFAULT_FIRST_LEVEL;
	s->default_second_level = DEFAULT_SECOND_LEVEL;
	/* Resetting Counters */
	s->counter[0] = 0;
	s->counter[1] = 0;
	s->timestamp = jiffies;
	sampling_rate = SAMPLING_RATE_MS;
	
	wq = create_singlethread_workqueue("falcon_hotplug_workqueue");
    
	if (!wq)
		return -ENOMEM;
    
	hotplug_control_kobj = kobject_create_and_add("hotplug_control", kernel_kobj);
	if (!hotplug_control_kobj) {
		pr_err("%s hotplug_control kobject create failed!\n", __FUNCTION__);
		return -ENOMEM;
        }

	ret = sysfs_create_group(hotplug_control_kobj,
			&hotplug_attr_group);
        if (ret) {
		pr_err("%s hotplug_control sysfs create failed!\n", __FUNCTION__);
		kobject_put(hotplug_control_kobj);
		return ret;
	}

	INIT_DELAYED_WORK(&decide_hotplug, decide_hotplug_func);
	queue_delayed_work_on(0, wq, &decide_hotplug, sampling_rate);
	
	return 0;
}
MODULE_AUTHOR("Francisco Franco <franciscofranco.1990@gmail.com>, "
	      "Alexander Christ <alex.christ@hotmail.de");
MODULE_DESCRIPTION("Simple SMP hotplug driver");
MODULE_LICENSE("GPL");

late_initcall(falcon_hotplug_init);
