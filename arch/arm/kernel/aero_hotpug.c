/*
 * Copyright (c) 2013, Francisco Franco <franciscofranco.1990@gmail.com>. 
 *
 * Small algorithm changes for more performance/battery life
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

#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/cpu.h>
#include <linux/workqueue.h>
#include <linux/sched.h>
#include <linux/timer.h>
#include <linux/cpufreq.h>
#include <linux/module.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/lcd_notify.h>

#define DEFAULT_FIRST_LEVEL	90
#define DEFAULT_SECOND_LEVEL	70
#define HIGH_LOAD_COUNTER	25
#define SAMPLING_RATE		4
#define DEFAULT_MIN_ONLINE	1
#define DEFAULT_UP_FREQUENCY	1100000
#define SMART_LOAD_CALC

struct hotplug_data {
	/* The threshold level for the average load of all onlined cpus */
	unsigned int all_cpus_threshold;

	/* The default sampling rate for the hotplug driver */
	unsigned int hotplug_sampling;

	/* The minimum amount of time for a cpu to stay online */
	unsigned int min_online_time;

	/* The first threshold level for a single cpu */
	unsigned int single_cpu_threshold;

	/* Short load spikes will be forcing cpus to come online */
	bool low_latency;

	/* The frequency threshold at or above onlining starts */
	unsigned int up_frequency;

	/* If enabled, only one core will be active during suspend */
	bool battery_saver;

	/* Debug flag */
	bool debug;

	unsigned long timestamp;
	unsigned int online_cpus;
	unsigned int possible_cpus;

	/* For the three hot-plug-able cores */
	unsigned int counter[2];
	unsigned int cpu_load_stats[3];
} *hot_data;

struct cpu_load_data {
        u64 prev_cpu_idle;
        u64 prev_cpu_wall;
};

#ifndef SMART_LOAD_CALC
static DEFINE_PER_CPU(struct cpu_load_data, cpuload);
#endif
static struct workqueue_struct *wq;
static struct delayed_work decide_hotplug;
static struct work_struct resume;
static struct work_struct suspend;
static struct notifier_block notif;

#ifndef SMART_LOAD_CALC
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
#endif

/*
 * Wrapper function which returns the current cpu load
 */
static inline int get_load(int cpu)
{
	int load;
#ifndef SMART_LOAD_CALC
	load = get_cpu_load(cpu);
#else
	load = cpufreq_quick_get_util(cpu);
#endif
	return load;
}

/*
 * Returns the average load for all currently onlined cpus
 */

static inline int get_load_for_all_cpu(void)
{
	int cpu;
	int load = 0;

	get_online_cpus();
	for_each_online_cpu(cpu) {

		hot_data->cpu_load_stats[cpu] = 0;
		hot_data->cpu_load_stats[cpu] = get_load(cpu);
		load = load + hot_data->cpu_load_stats[cpu];
	}
	put_online_cpus();
	
	return (load / hot_data->online_cpus);
}

static void __ref set_cpu_up(int cpu) 
{
	if (!cpu_online(cpu)
		&& hot_data->online_cpus != hot_data->possible_cpus) {

		if (hot_data->debug)
			pr_info("[Hot-Plug]: CPU%u ready for onlining\n", cpu);

		cpu_up(cpu);
		hot_data->timestamp = jiffies;
		hot_data->online_cpus = num_online_cpus();
	}
	return;
}

/*
 * Calculates the load for a given cpu
 */ 

static inline void calculate_load_for_cpu(int cpu) 
{
	int avg_load, cpu_load;

	cpu_load = get_load(cpu);
	avg_load = get_load_for_all_cpu();

	/* CPU is stressed */
	if (hot_data->low_latency) {
		if (cpu_load >= 100 && avg_load >= 100) {
			if (hot_data->debug)
				pr_info("[Hot-Plug]: CPU%u is stressed, "
					"considering boosting CPU%u \n", 
					cpu, (cpu + 2));
			set_cpu_up(cpu + 2);
			return;
		}
	}

	/*  
	 * We are above our threshold, so update our counter for cpu.
	 * Consider this only, if we are on our max frequency
	 */
	if (cpu_load >= hot_data->single_cpu_threshold &&
		avg_load >= hot_data->all_cpus_threshold
		&& hot_data->counter[cpu] < HIGH_LOAD_COUNTER
		&& cpufreq_quick_get(cpu) >= hot_data->up_frequency) {

		hot_data->counter[cpu] += 2;
	} else {
		if (hot_data->counter[cpu] > 0)
			hot_data->counter[cpu]--;
	}	
}

/*
 * Finds the lowest operation core to offline
 */
static inline void put_cpu_down(int cpu) 
{
	int target_cpu = cpu; 
	int cpu_load = 0;
	int lowest_load = 100;
	int parent_load;
	int j, i;

	/* Prevent fast on-/offlining */ 
	if (time_is_after_jiffies(hot_data->timestamp + (HZ * hot_data->min_online_time)))	
		return;

	/* No core was online anyway */
	if (!cpu_online(cpu)) {
		if ((!cpu_online(cpu + 1)) || (!cpu_online(cpu - 1)))
			return;
	}

	/*
	 * Decide which core should be offlined
	 */
	for (j = 2; j < 4; j++) {

		if (!cpu_online(j))
			continue;

		cpu_load = get_load(j);

		/* 
		 * Check if the "child" cpus are more busy then their "parents"
		 */
		for (i = 0; i < 2; i++) {
			parent_load = get_load(i);
			if (cpu_load > parent_load &&
				cpufreq_quick_get(j) >= cpufreq_quick_get(i)) {
				if (hot_data->debug)						
					pr_info("[Hot-Plug]: Preventing CPU%u from offlining\n", j);
				return;
			}
		}

		if (cpu_load < lowest_load) {
			lowest_load = cpu_load;
			target_cpu = j;	
		}
	}

	if (hot_data->debug)						
		pr_info("[Hot-Plug]: CPU%u ready for offlining\n", target_cpu);

	cpu_down(target_cpu);
	hot_data->cpu_load_stats[target_cpu] = 0;
	hot_data->timestamp = jiffies;
	hot_data->online_cpus = num_online_cpus();
}

/**
 * Simple load based decision algorithm to determ
 * how many cores should be on- or offlined
 */

static void __ref decide_hotplug_func(struct work_struct *work)
{
	int i, j;

	/* Reschedule early if we don't need to bother about calculations */
	if (unlikely(hot_data->online_cpus == 1))
		queue_delayed_work(system_power_efficient_wq, &decide_hotplug, msecs_to_jiffies(hot_data->hotplug_sampling * HZ));

	for (i = 0, j = 2; i < 2; i++, j++) {

		/* Do load calculation for each cpu counter */
		calculate_load_for_cpu(i);

		if (hot_data->counter[i] >= 10) {
			set_cpu_up(j);
		} else {
			if (hot_data->counter[i] >= 0) {
				put_cpu_down(j);	
			}
		}
	}
	
	/* Make a dedicated work_queue */
	queue_delayed_work_on(0, wq, &decide_hotplug, msecs_to_jiffies(hot_data->hotplug_sampling * HZ));
}

static inline void suspend_func(struct work_struct *work)
{	 
	int cpu;

	if (hot_data->battery_saver) {
		for_each_online_cpu(cpu) 
			if (cpu)
				cpu_down(cpu);
	}

	hot_data->online_cpus = num_online_cpus();

	if (hot_data->debug)
		pr_info("[Hot-Plug]: Early Suspend stopping Hotplug work. CPUs online: %d\n", hot_data->online_cpus);
}

static inline void resume_func(struct work_struct *work)
{
	/* Online only the second core */
	if (hot_data->battery_saver)
		set_cpu_up(1);

	/* Resetting Counters */
	hot_data->counter[0] = 0;
	hot_data->counter[1] = 0;

	if (hot_data->debug)
		pr_info("[Hot-Plug]: Late Resume starting Hotplug work. CPUs online: %d\n", hot_data->online_cpus);

	queue_delayed_work_on(0, wq, &decide_hotplug, msecs_to_jiffies(hot_data->hotplug_sampling * HZ));
}

static int lcd_notifier_callback(struct notifier_block *this,
	unsigned long event, void *data)
{
	if (event == LCD_EVENT_ON_START)
		queue_work(system_power_efficient_wq, &resume);
	else if (event == LCD_EVENT_OFF_START)
		queue_work(system_power_efficient_wq, &suspend);

	return NOTIFY_OK;
}

/*** Start sysfs entries ***/
#define show_tunable(file_name, object)					\
static ssize_t show_##file_name						\
(struct kobject *kobj, struct attribute *attr, char *buf)		\
{									\
    return sprintf(buf, "%u\n", hot_data->object);			\
}

show_tunable(single_cpu_threshold, single_cpu_threshold);
show_tunable(hotplug_sampling, hotplug_sampling);
show_tunable(min_online_time, min_online_time);
show_tunable(all_cpus_threshold, all_cpus_threshold);
show_tunable(low_latency, low_latency);
show_tunable(debug, debug);
show_tunable(up_frequency, up_frequency);
show_tunable(battery_saver, battery_saver);

#define store_tunable(file_name, object)					\
static ssize_t store_##file_name						\
(struct kobject *kobj, struct attribute *attr, const char *buf, size_t count)	\
{										\
    unsigned int val;								\
    sscanf(buf, "%u", &val);							\
    hot_data->object = val;							\
    return count;								\
}  

store_tunable(single_cpu_threshold, single_cpu_threshold);
store_tunable(hotplug_sampling, hotplug_sampling);
store_tunable(min_online_time, min_online_time);
store_tunable(all_cpus_threshold, all_cpus_threshold);
store_tunable(low_latency, low_latency);
store_tunable(debug, debug);
store_tunable(up_frequency, up_frequency);
store_tunable(battery_saver, battery_saver);

define_one_global_rw(single_cpu_threshold);
define_one_global_rw(hotplug_sampling);
define_one_global_rw(min_online_time);
define_one_global_rw(all_cpus_threshold);
define_one_global_rw(low_latency);
define_one_global_rw(debug);
define_one_global_rw(up_frequency);
define_one_global_rw(battery_saver);

static struct attribute *aero_hotplug_attributes[] = 
{
	&hotplug_sampling.attr,
	&single_cpu_threshold.attr,
	&min_online_time.attr,
	&all_cpus_threshold.attr,
	&up_frequency.attr,
	&low_latency.attr,
	&battery_saver.attr,
	&debug.attr,
	NULL
};

static struct attribute_group hotplug_attr_group = 
{
	.attrs  = aero_hotplug_attributes,
};

static struct kobject *hotplug_control_kobj;

int __init aero_hotplug_init(void)
{
	int ret = 0;

	hot_data = kzalloc(sizeof(*hot_data), GFP_KERNEL);
	if (!hot_data)
		return -ENOMEM;

	hot_data->hotplug_sampling = SAMPLING_RATE;
	hot_data->min_online_time = DEFAULT_MIN_ONLINE;
	hot_data->single_cpu_threshold = DEFAULT_FIRST_LEVEL;
	hot_data->all_cpus_threshold = DEFAULT_SECOND_LEVEL;
	hot_data->low_latency = false;
	hot_data->debug = false;
	hot_data->up_frequency = DEFAULT_UP_FREQUENCY;
	hot_data->battery_saver = true;

	if (hot_data->debug)
		pr_info("[Hot-Plug]: Aero Hotplug driver started.\n");

	hotplug_control_kobj = kobject_create_and_add("hotplug_control", kernel_kobj);
	if (!hotplug_control_kobj) {
		pr_err("%s hotplug_control kobject create failed!\n", __FUNCTION__);
		return -ENOMEM;
	}

	ret = sysfs_create_group(hotplug_control_kobj,
			&hotplug_attr_group);
        if (ret) {
		pr_info("%s hotplug_control sysfs create failed!\n", __FUNCTION__);
		kobject_put(hotplug_control_kobj);
		return ret;
	}

	/* Resetting Counters */
	hot_data->counter[0] = 0;
	hot_data->counter[1] = 0;
	hot_data->timestamp = jiffies;
	hot_data->online_cpus = num_online_cpus();
	hot_data->possible_cpus = num_possible_cpus();

	wq = alloc_workqueue("aero_hotplug_workqueue", WQ_FREEZABLE, 1);
    
	if (!wq)
		return -ENOMEM;

	notif.notifier_call = lcd_notifier_callback;

	if (lcd_register_client(&notif)) {
		return -EINVAL;
	}

	INIT_WORK(&resume, resume_func);
	INIT_WORK(&suspend, suspend_func);
	INIT_DELAYED_WORK(&decide_hotplug, decide_hotplug_func);
	queue_delayed_work_on(0, wq, &decide_hotplug, msecs_to_jiffies(30000));
	
	return ret;
}
MODULE_AUTHOR("Francisco Franco <franciscofranco.1990@gmail.com>, "
	      "Alexander Christ <alex.christ@hotmail.de");
MODULE_DESCRIPTION("Simple SMP hotplug driver");
MODULE_LICENSE("GPLv2");

late_initcall(aero_hotplug_init);
