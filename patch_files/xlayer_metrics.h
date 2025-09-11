/* SPDX-License-Identifier: GPL-2.0 */
/*
 * XLayer MPTCP Scheduler - Network Metrics Collection
 *
 * This header provides network metrics collection functionality for the
 * XLayer scheduler, including WiFi/5G bitrate monitoring and background
 * thread management for real-time cross-layer optimization.
 *
 * Features:
 * - WiFi bitrate collection via cfg80211 APIs
 * - 5G bitrate configuration via proc interface
 * - Background thread for continuous metrics monitoring
 * - Thread-safe data structures with spinlocks
 * - IP-based interface classification (WiFi vs 5G)
 *
 * Author: Tiago Sousa <tiagojoseas@gmail.com>
 */

#ifndef __XLAYER_METRICS_H
#define __XLAYER_METRICS_H

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/kthread.h>
#include <linux/spinlock.h>
#include <linux/delay.h>
#include <linux/proc_fs.h>
#include <linux/uaccess.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/slab.h>
#include <net/cfg80211.h>
#include <linux/in.h>

/* Configuration Constants */
#define XLAYER_PROC_NAME "xlayer_5g_proc"
#define XLAYER_BUFFER_SIZE 128
#define XLAYER_METRICS_UPDATE_INTERVAL 1  /* seconds */
#define XLAYER_WIFI_INTERFACE "wlan0"

/* Network Metrics Data Structure */
struct xlayer_net_metrics {
	long wifi_bitrate;     /* Current WiFi bitrate (Kbps) */
	long nr_bitrate;       /* Current 5G/NR bitrate (bps) from proc */
	spinlock_t lock;       /* Thread-safe access */
	unsigned long last_update; /* Last metrics update timestamp */
};

/* Thread Management Structure */
struct xlayer_thread_mgmt {
	struct task_struct *metrics_thread;
	bool thread_running;
	bool stop_requested;
};

/* Proc Interface Management */
struct xlayer_proc_interface {
	char buffer[XLAYER_BUFFER_SIZE];
	ssize_t buffer_len;
	int configured_5g_bitrate;  /* User-configured 5G bitrate */
};

/* Global XLayer Metrics Instance */
static struct xlayer_net_metrics xlayer_metrics = {
	.wifi_bitrate = -1,
	.nr_bitrate = -1,
	.lock = __SPIN_LOCK_UNLOCKED(xlayer_metrics.lock),
	.last_update = 0,
};

static struct xlayer_thread_mgmt xlayer_thread = {
	.metrics_thread = NULL,
	.thread_running = false,
	.stop_requested = false,
};

static struct xlayer_proc_interface xlayer_proc = {
	.buffer = {0},
	.buffer_len = 0,
	.configured_5g_bitrate = 0,
};

/**
 * xlayer_extract_ip_address() - Extract IP address from socket for classification
 * @sk: Socket to extract IP from
 * @ip_str: Buffer to store IP string (must be at least 16 bytes)
 */
static inline void xlayer_extract_ip_address(struct sock *sk, char *ip_str)
{
	if (sk->sk_family == AF_INET) {
		snprintf(ip_str, 16, "%pI4", &sk->sk_rcv_saddr);
	} else {
		strcpy(ip_str, "unknown");
	}
}

/**
 * xlayer_is_wifi_interface() - Check if IP belongs to WiFi interface
 * @ip_str: IP address string to check
 * @return: true if IP belongs to WiFi interface, false otherwise
 */
static bool xlayer_is_wifi_interface(const char *ip_str)
{
	struct net_device *dev;
	struct in_device *in_dev;
	struct in_ifaddr *ifa;
	__be32 target_ip;
	bool is_wifi = false;

	if (!ip_str)
		return false;

	target_ip = in_aton(ip_str);

	/* Check WiFi interface first */
	dev = dev_get_by_name(&init_net, XLAYER_WIFI_INTERFACE);
	if (!dev) {
		/* Try wlp* pattern for modern naming */
		/* This is a simplified check - could be enhanced */
		return false;
	}

	rcu_read_lock();
	in_dev = __in_dev_get_rcu(dev);
	if (in_dev) {
		for (ifa = in_dev->ifa_list; ifa; ifa = ifa->ifa_next) {
			if (ifa->ifa_address == target_ip) {
				is_wifi = true;
				break;
			}
		}
	}
	rcu_read_unlock();

	dev_put(dev);
	return is_wifi;
}

/**
 * xlayer_collect_wifi_metrics() - Collect WiFi bitrate metrics
 * @interface_name: Name of WiFi interface (e.g., "wlan0") -> XLAYER_WIFI_INTERFACE
 * @return: Current WiFi bitrate in Kbps, negative on error
 */
static long xlayer_collect_wifi_metrics(const char *interface_name)
{
	struct net_device *ndev;
	struct wireless_dev *wdev;
	struct wiphy *wiphy;
	struct cfg80211_bss *bss;
	struct station_info *sinfo;
	int ret, bitrate = -ENODATA;

	ndev = dev_get_by_name(&init_net, interface_name);
	if (!ndev)
		return -ENODEV;

	wdev = ndev->ieee80211_ptr;
	if (!wdev || !wdev->netdev) {
		dev_put(ndev);
		return -ENODEV;
	}

	wiphy = wdev->wiphy;
	if (!wiphy) {
		dev_put(ndev);
		return -ENODEV;
	}

	/* Get current BSS info */
	bss = cfg80211_get_bss(wiphy, NULL, NULL, NULL, 0, 
			       IEEE80211_BSS_TYPE_ANY, IEEE80211_PRIVACY_ANY);
	if (!bss) {
		dev_put(ndev);
		return -ENODATA;
	}

	sinfo = kzalloc(sizeof(*sinfo), GFP_KERNEL);
	if (!sinfo) {
		cfg80211_put_bss(wiphy, bss);
		dev_put(ndev);
		return -ENOMEM;
	}

	/* Get station information */
	ret = cfg80211_get_station(ndev, bss->bssid, sinfo);
	if (ret == 0) {
		/* Prefer RX bitrate, fallback to TX */
		if (sinfo->filled & BIT_ULL(NL80211_STA_INFO_RX_BITRATE)) {
			bitrate = cfg80211_calculate_bitrate(&sinfo->rxrate);
		} else if (sinfo->filled & BIT_ULL(NL80211_STA_INFO_TX_BITRATE)) {
			bitrate = cfg80211_calculate_bitrate(&sinfo->txrate);
		}
	}

	kfree(sinfo);
	cfg80211_put_bss(wiphy, bss);
	dev_put(ndev);

	return bitrate > 0 ? bitrate : -ENODATA;
}

/**
 * xlayer_get_5g_bitrate() - Get configured 5G bitrate
 * @return: Configured 5G bitrate in bps
 */
static inline long xlayer_get_5g_bitrate(void)
{
	return xlayer_proc.configured_5g_bitrate;
}

/**
 * xlayer_update_metrics() - Update network metrics atomically
 * @wifi_rate: WiFi bitrate in Kbps
 * @nr_rate: 5G bitrate in bps
 */
static void xlayer_update_metrics(long wifi_rate, long nr_rate)
{
	spin_lock(&xlayer_metrics.lock);
	xlayer_metrics.wifi_bitrate = wifi_rate;
	xlayer_metrics.nr_bitrate = nr_rate;
	xlayer_metrics.last_update = jiffies;
	spin_unlock(&xlayer_metrics.lock);
}

/**
 * xlayer_get_metrics() - Get current network metrics safely
 * @wifi_rate: Pointer to store WiFi bitrate
 * @nr_rate: Pointer to store 5G bitrate
 */
static void xlayer_get_metrics(long *wifi_rate, long *nr_rate)
{
	spin_lock(&xlayer_metrics.lock);
	if (wifi_rate)
		*wifi_rate = xlayer_metrics.wifi_bitrate;
	if (nr_rate)
		*nr_rate = xlayer_metrics.nr_bitrate;
	spin_unlock(&xlayer_metrics.lock);
}

/**
 * xlayer_metrics_thread() - Background thread for continuous metrics collection
 * @data: Unused thread parameter
 * @return: 0 on normal exit
 */
static int xlayer_metrics_thread(void *data)
{
	long wifi_bitrate, nr_bitrate;

	// pr_info("xlayer: Network metrics collection thread started\n");

	while (!xlayer_thread.stop_requested && !kthread_should_stop()) {
		/* Collect WiFi metrics */
		wifi_bitrate = xlayer_collect_wifi_metrics(XLAYER_WIFI_INTERFACE);

		/* Get 5G metrics from proc configuration */
		nr_bitrate = xlayer_get_5g_bitrate();

		/* Update metrics atomically */
		xlayer_update_metrics(wifi_bitrate, nr_bitrate);

		/* Debug logging (can be removed in production) */
		if (wifi_bitrate > 0 || nr_bitrate > 0) {
			pr_debug("xlayer: metrics update - WiFi: %ld Kbps, 5G: %ld bps\n",
				 wifi_bitrate, nr_bitrate);
		}

		/* Sleep for configured interval */
		ssleep(XLAYER_METRICS_UPDATE_INTERVAL);
	}

	// pr_info("xlayer: Network metrics collection thread stopping\n");
	return 0;
}

/**
 * xlayer_start_metrics_thread() - Start the metrics collection thread
 * @return: 0 on success, negative error code on failure
 */
static int xlayer_start_metrics_thread(void)
{
	if (xlayer_thread.thread_running)
		return -EALREADY;

	xlayer_thread.stop_requested = false;
	xlayer_thread.metrics_thread = kthread_run(xlayer_metrics_thread, NULL, 
						    "xlayer_metrics");
	
	if (IS_ERR(xlayer_thread.metrics_thread)) {
		int ret = PTR_ERR(xlayer_thread.metrics_thread);
		xlayer_thread.metrics_thread = NULL;
		return ret;
	}

	xlayer_thread.thread_running = true;
	return 0;
}

/**
 * xlayer_stop_metrics_thread() - Stop the metrics collection thread
 */
static void xlayer_stop_metrics_thread(void)
{
	if (!xlayer_thread.thread_running)
		return;

	xlayer_thread.stop_requested = true;
	
	if (xlayer_thread.metrics_thread) {
		kthread_stop(xlayer_thread.metrics_thread);
		xlayer_thread.metrics_thread = NULL;
	}
	
	xlayer_thread.thread_running = false;
}

/* Proc Interface Functions */

/**
 * xlayer_proc_read() - Read current 5G bitrate configuration
 */
static ssize_t xlayer_proc_read(struct file *file, char __user *user_buf, 
				size_t count, loff_t *pos)
{
	int len;
	char temp_buf[32];

	if (*pos > 0)
		return 0;

	len = snprintf(temp_buf, sizeof(temp_buf), "%d\n", 
		       xlayer_proc.configured_5g_bitrate);

	if (len > count)
		len = count;

	if (copy_to_user(user_buf, temp_buf, len))
		return -EFAULT;

	*pos = len;
	return len;
}

/**
 * xlayer_proc_write() - Configure 5G bitrate via proc interface
 */
static ssize_t xlayer_proc_write(struct file *file, const char __user *user_buf,
				 size_t count, loff_t *pos)
{
	int new_bitrate;
	size_t len;

	len = count > (XLAYER_BUFFER_SIZE - 1) ? (XLAYER_BUFFER_SIZE - 1) : count;

	if (copy_from_user(xlayer_proc.buffer, user_buf, len))
		return -EFAULT;

	xlayer_proc.buffer[len] = '\0';
	xlayer_proc.buffer_len = len;

	if (sscanf(xlayer_proc.buffer, "%d", &new_bitrate) != 1) {
		return -EINVAL;
	}

	if (new_bitrate < 0) {
		pr_warn("xlayer: Invalid 5G bitrate %d, must be >= 0\n", new_bitrate);
		return -EINVAL;
	}

	xlayer_proc.configured_5g_bitrate = new_bitrate;
	// pr_info("xlayer: 5G bitrate configured to %d bps\n", new_bitrate);

	return count;
}

static const struct proc_ops xlayer_proc_ops = {
	.proc_read = xlayer_proc_read,
	.proc_write = xlayer_proc_write,
};

/**
 * xlayer_init_proc_interface() - Initialize proc interface
 * @return: 0 on success, negative error on failure
 */
static int xlayer_init_proc_interface(void)
{
	if (!proc_create(XLAYER_PROC_NAME, 0666, NULL, &xlayer_proc_ops)) {
		pr_err("xlayer: Failed to create /proc/%s\n", XLAYER_PROC_NAME);
		return -ENOMEM;
	}

	// pr_info("xlayer: Created /proc/%s for 5G bitrate configuration\n", XLAYER_PROC_NAME);
	return 0;
}

/**
 * xlayer_cleanup_proc_interface() - Clean up proc interface
 */
static void xlayer_cleanup_proc_interface(void)
{
	remove_proc_entry(XLAYER_PROC_NAME, NULL);
	// pr_info("xlayer: Removed /proc/%s\n", XLAYER_PROC_NAME);
}

/**
 * xlayer_metrics_init() - Initialize metrics collection system
 * @return: 0 on success, negative error on failure
 */
static inline int xlayer_metrics_init(void)
{
	int ret;

	/* Initialize proc interface first */
	ret = xlayer_init_proc_interface();
	if (ret < 0)
		return ret;

	/* Start metrics collection thread */
	ret = xlayer_start_metrics_thread();
	if (ret < 0) {
		xlayer_cleanup_proc_interface();
		return ret;
	}

	// pr_info("xlayer: Metrics collection system initialized\n");
	return 0;
}

/**
 * xlayer_metrics_cleanup() - Clean up metrics collection system
 */
static inline void xlayer_metrics_cleanup(void)
{
	xlayer_stop_metrics_thread();
	xlayer_cleanup_proc_interface();
	// pr_info("xlayer: Metrics collection system cleaned up\n");
}

#endif /* __XLAYER_METRICS_H */