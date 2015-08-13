/*
 * drivers/thermal/msm_thermal_simple.c
 *
 * Copyright (C) 2014-2015, Sultanxda <sultanxda@gmail.com>
 *
 * Originally based off the MSM8x60 thermal implementation by:
 * Copyright (c) 2012, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#define pr_fmt(fmt) "MSM_THERMAL: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/kernel.h>
#include <linux/msm_tsens.h>
#include <linux/slab.h>

#define TSENS_SENSOR 0
#define DEFAULT_SAMPLING_MS 3000

enum thermal_state {
	UNTHROTTLE,
	LOW_THROTTLE,
	MID_THROTTLE,
	HIGH_THROTTLE,
};

struct throttle_policy {
	enum thermal_state cpu_throttle;
	unsigned int throttle_freq;
};

static DEFINE_PER_CPU(struct throttle_policy, throttle_info);
static struct delayed_work thermal_work;
static struct workqueue_struct *thermal_wq;

struct thermal_config {
	unsigned int freq_high_KHz;
	unsigned int freq_mid_KHz;
	unsigned int freq_low_KHz;
	unsigned int trip_high_degC;
	unsigned int trip_mid_degC;
	unsigned int trip_low_degC;
	unsigned int reset_high_degC;
	unsigned int reset_mid_degC;
	unsigned int reset_low_degC;
	unsigned int sampling_ms;
	unsigned int enabled;
	unsigned int user_maxfreq;
};

static struct thermal_config *t_conf;

static void msm_thermal_main(struct work_struct *work)
{
	struct tsens_device tsens_dev;
	struct throttle_policy *t;
	unsigned long temp;
	unsigned int cpu, old_throttle;
	bool throttle_logged = false;
	int ret;

	tsens_dev.sensor_num = TSENS_SENSOR;
	ret = tsens_get_temp(&tsens_dev, &temp);
	/* bound check */
	if (ret || temp > 1000) {
		pr_err("Unable to read tsens sensor #%d\n", tsens_dev.sensor_num);
		goto reschedule;
	}

	get_online_cpus();
	for_each_possible_cpu(cpu) {
		t = &per_cpu(throttle_info, cpu);

		old_throttle = t->cpu_throttle;

		/* low trip point */
		if ((temp >= t_conf->trip_low_degC) &&
		(temp < t_conf->trip_mid_degC) &&
			(t->cpu_throttle == UNTHROTTLE)) {
			t->throttle_freq = t_conf->freq_low_KHz;
			t->cpu_throttle = LOW_THROTTLE;
		/* low clear point */
		} else if ((temp <= t_conf->reset_low_degC) &&
			(t->cpu_throttle > UNTHROTTLE)) {
			t->cpu_throttle = UNTHROTTLE;
		/* mid trip point */
		} else if ((temp >= t_conf->trip_mid_degC) &&
			(temp < t_conf->trip_high_degC) &&
			(t->cpu_throttle < MID_THROTTLE)) {
			t->throttle_freq = t_conf->freq_mid_KHz;
			t->cpu_throttle = MID_THROTTLE;
		/* mid clear point */
		} else if ((temp < t_conf->reset_mid_degC) &&
			(t->cpu_throttle > LOW_THROTTLE)) {
			t->throttle_freq = t_conf->freq_low_KHz;
			t->cpu_throttle = LOW_THROTTLE;
		/* high trip point */
		} else if ((temp >= t_conf->trip_high_degC) &&
			(t->cpu_throttle < HIGH_THROTTLE)) {
			t->throttle_freq = t_conf->freq_high_KHz;
			t->cpu_throttle = HIGH_THROTTLE;
		/* high clear point */
		} else if ((temp < t_conf->reset_high_degC) &&
			(t->cpu_throttle > MID_THROTTLE)) {
			t->throttle_freq = t_conf->freq_mid_KHz;
			t->cpu_throttle = MID_THROTTLE;
		}

		/* logging */
		if ((t->cpu_throttle != old_throttle) && !throttle_logged) {
			if (t->cpu_throttle)
				pr_warn("Setting CPU to %uKHz! temp: %luC\n",
							t->throttle_freq, temp);
			else
				pr_warn("CPU unthrottled! temp: %luC\n", temp);
			throttle_logged = true;
		}

		/* trigger cpufreq notifier */
		if (cpu_online(cpu))
			cpufreq_update_policy(cpu);
	}
	put_online_cpus();

reschedule:
	queue_delayed_work_on(0, thermal_wq, &thermal_work,
				msecs_to_jiffies(t_conf->sampling_ms));
}

static void unthrottle_all_cpus(void)
{
	struct throttle_policy *t;
	unsigned int cpu;

	get_online_cpus();
	for_each_possible_cpu(cpu) {
		t = &per_cpu(throttle_info, cpu);
		t->cpu_throttle = UNTHROTTLE;
		if (cpu_online(cpu))
			cpufreq_update_policy(cpu);
	}
	put_online_cpus();
}

static int cpu_do_throttle(struct notifier_block *nb, unsigned long val, void *data)
{
	struct cpufreq_policy *policy = data;
	struct throttle_policy *t = &per_cpu(throttle_info, policy->cpu);
	unsigned int user_max = t_conf->user_maxfreq;

	if (val != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	switch (t->cpu_throttle) {
	case UNTHROTTLE:
		policy->max = user_max ? user_max : policy->cpuinfo.max_freq;
		break;
	case LOW_THROTTLE:
	case MID_THROTTLE:
	case HIGH_THROTTLE:
		if (user_max && (user_max < t->throttle_freq))
			policy->max = user_max;
		else
			policy->max = t->throttle_freq;
		break;
	}

	if (policy->min > policy->max)
		policy->min = policy->cpuinfo.min_freq;

	return NOTIFY_OK;
}

static struct notifier_block cpu_throttle_nb = {
	.notifier_call = cpu_do_throttle,
};

/*********************** SYSFS START ***********************/
static struct kobject *msm_thermal_kobject;

static ssize_t high_thresh_write(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int data[3];
	int ret = sscanf(buf, "%u %u %u", &data[0], &data[1], &data[2]);

	if (ret != 3)
		return -EINVAL;

	t_conf->freq_high_KHz = data[0];
	t_conf->trip_high_degC = data[1];
	t_conf->reset_high_degC = data[2];

	return size;
}

static ssize_t mid_thresh_write(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int data[3];
	int ret = sscanf(buf, "%u %u %u", &data[0], &data[1], &data[2]);

	if (ret != 3)
		return -EINVAL;

	t_conf->freq_mid_KHz = data[0];
	t_conf->trip_mid_degC = data[1];
	t_conf->reset_mid_degC = data[2];

	return size;
}

static ssize_t low_thresh_write(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int data[3];
	int ret = sscanf(buf, "%u %u %u", &data[0], &data[1], &data[2]);

	if (ret != 3)
		return -EINVAL;

	t_conf->freq_low_KHz = data[0];
	t_conf->trip_low_degC = data[1];
	t_conf->reset_low_degC = data[2];

	return size;
}

static ssize_t sampling_ms_write(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int data;
	int ret = sscanf(buf, "%u", &data);

	if (ret != 1)
		return -EINVAL;

	t_conf->sampling_ms = data;

	return size;
}

static ssize_t enabled_write(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int data;
	int ret = sscanf(buf, "%u", &data);

	if (ret != 1)
		return -EINVAL;

	t_conf->enabled = data;

	cancel_delayed_work_sync(&thermal_work);

	if (data)
		queue_delayed_work_on(0, thermal_wq, &thermal_work, 0);
	else
		unthrottle_all_cpus();

	return size;
}

static ssize_t user_maxfreq_write(struct device *dev,
		struct device_attribute *attr, const char *buf, size_t size)
{
	unsigned int data;
	int ret = sscanf(buf, "%u", &data);

	if (ret != 1)
		return -EINVAL;

	t_conf->user_maxfreq = data;

	return size;
}

static ssize_t high_thresh_read(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u %u %u\n", t_conf->freq_high_KHz,
			t_conf->trip_high_degC, t_conf->reset_high_degC);
}

static ssize_t mid_thresh_read(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u %u %u\n", t_conf->freq_mid_KHz,
			t_conf->trip_mid_degC, t_conf->reset_mid_degC);
}

static ssize_t low_thresh_read(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u %u %u\n", t_conf->freq_low_KHz,
			t_conf->trip_low_degC, t_conf->reset_low_degC);
}

static ssize_t sampling_ms_read(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", t_conf->sampling_ms);
}

static ssize_t enabled_read(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", t_conf->enabled);
}

static ssize_t user_maxfreq_read(struct device *dev,
		struct device_attribute *attr, char *buf)
{
	return snprintf(buf, PAGE_SIZE, "%u\n", t_conf->user_maxfreq);
}

static DEVICE_ATTR(high_thresh, 0644, high_thresh_read, high_thresh_write);
static DEVICE_ATTR(mid_thresh, 0644, mid_thresh_read, mid_thresh_write);
static DEVICE_ATTR(low_thresh, 0644, low_thresh_read, low_thresh_write);
static DEVICE_ATTR(sampling_ms, 0644, sampling_ms_read, sampling_ms_write);
static DEVICE_ATTR(enabled, 0644, enabled_read, enabled_write);
static DEVICE_ATTR(user_maxfreq, 0644, user_maxfreq_read, user_maxfreq_write);

static struct attribute *msm_thermal_attr[] = {
	&dev_attr_high_thresh.attr,
	&dev_attr_mid_thresh.attr,
	&dev_attr_low_thresh.attr,
	&dev_attr_sampling_ms.attr,
	&dev_attr_enabled.attr,
	&dev_attr_user_maxfreq.attr,
	NULL
};

static struct attribute_group msm_thermal_attr_group = {
	.attrs  = msm_thermal_attr,
};
/*********************** SYSFS END ***********************/

static int __init msm_thermal_init(void)
{
	int ret;

	thermal_wq = alloc_workqueue("msm_thermal_wq", WQ_HIGHPRI, 0);
	if (!thermal_wq) {
		pr_err("Failed to allocate workqueue\n");
		ret = -EFAULT;
		goto err;
	}

	cpufreq_register_notifier(&cpu_throttle_nb, CPUFREQ_POLICY_NOTIFIER);

	t_conf = kzalloc(sizeof(struct thermal_config), GFP_KERNEL);
	if (!t_conf) {
		pr_err("Failed to allocate thermal_config struct\n");
		ret = -ENOMEM;
		goto err;
	}

	t_conf->sampling_ms = DEFAULT_SAMPLING_MS;

	INIT_DELAYED_WORK(&thermal_work, msm_thermal_main);

	msm_thermal_kobject = kobject_create_and_add("msm_thermal", kernel_kobj);
	if (!msm_thermal_kobject) {
		pr_err("Failed to create kobject\n");
		ret = -ENOMEM;
		goto err;
	}

	ret = sysfs_create_group(msm_thermal_kobject, &msm_thermal_attr_group);
	if (ret) {
		pr_err("Failed to create sysfs interface\n");
		kobject_put(msm_thermal_kobject);
	}
err:
	return ret;
}
fs_initcall(msm_thermal_init);
