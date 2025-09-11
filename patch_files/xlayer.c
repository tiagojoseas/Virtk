// SPDX-License-Identifier: GPL-2.0
/*
 * MPTCP XLayer Scheduler - Cross-Layer Optimization for MPTCP v1
 *
 * Combines network-layer information (WiFi/5G bitrates) with transport-layer
 * scheduling to optimize MPTCP subflow selection and prevent head-of-line blocking.
 *
 * Algorithm: Select subflow with higher current bitrate, fallback to minRTT.
 * Includes BLEST-style HoL blocking prevention.
 *
 * Features:
 * - Real-time WiFi/5G metrics collection (background thread)
 * - Cross-layer subflow selection
 * - Proc interface for 5G bitrate: /proc/xlayer_5g_proc
 * - Thread-safe operation with spinlocks
 *
 * Usage:
 *   echo xlayer > /proc/sys/net/mptcp/scheduler
 *   echo "150000000" > /proc/xlayer_5g_proc
 *
 * Original Design: Simone Ferlin, Ozgu Alay, Olivier Mehani, Roksana Boreli
 * MPTCP v1 Implementation: Tiago Sousa <tiagojoseas@gmail.com>
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/types.h>
#include <linux/tcp.h>
#include <linux/inet.h>
#include <net/tcp.h>
#include <net/inet_connection_sock.h>
#include <linux/in.h>
#include "protocol.h"
#include "common_lib.h"
#include "xlayer_metrics.h"

/* XLayer uses shared lambda parameter from common_lib.h */

/* XLayer global data - simplified, metrics moved to xlayer_metrics.h */
static struct {
	s16 lambda_1000;
	u32 total_decisions;
	u32 hol_prevented;
} xlayer_global = {
	.lambda_1000 = 0,
	.total_decisions = 0,
	.hol_prevented = 0,
};

static void mptcp_sched_xlayer_init(struct mptcp_sock *msk)
{
	xlayer_global.lambda_1000 = lambda * 100;
	xlayer_global.total_decisions = 0;
	xlayer_global.hol_prevented = 0;
	pr_info("XLayer scheduler with cross-layer optimization initialized (lambda=%d.%d)\n", lambda / 10, lambda % 10);
}

static void mptcp_sched_xlayer_release(struct mptcp_sock *msk)
{
	// pr_info("XLayer scheduler released - Total decisions: %u, HoL prevented: %u\n", xlayer_global.total_decisions, xlayer_global.hol_prevented);
}

/*
 * Core XLayer subflow selection algorithm with cross-layer optimization
 * 
 * Algorithm:
 * 1. Find fastest subflow (min RTT) for fallback
 * 2. Classify subflows by IP (WiFi vs 5G) 
 * 3. Select subflow with higher bitrate
 * 4. Check for head-of-line blocking risk
 * 5. Override with fastest subflow if HoL blocking detected
 */
static int mptcp_sched_xlayer_get_subflow(struct mptcp_sock *msk,
					 struct mptcp_sched_data *data)
{
	struct mptcp_subflow_context *subflow, *fastest_subflow = NULL;
	struct mptcp_subflow_context *wifi_subflow = NULL, *nr_subflow = NULL;
	struct mptcp_subflow_context *best_subflow = NULL;
	struct sock *ssk, *fastest_ssk, *best_ssk;
	struct tcp_sock *best_tp, *meta_tp;
	char ip_str[16];
	long wifi_ratio, nr_ratio;
	u32 slow_inflight, fast_capacity, meta_wnd;

	xlayer_global.total_decisions++;

	/* Find fastest subflow using shared function */
	fastest_subflow = find_min_rtt_subflow(msk);
	if (!fastest_subflow) {
		pr_debug("xlayer: no available subflow found\n");
		return -EINVAL;
	}

	/* Classify subflows by network interface (WiFi vs 5G) */
	mptcp_for_each_subflow(msk, subflow) {
		if (!mptcp_subflow_is_available(subflow))
			continue;

		ssk = mptcp_subflow_tcp_sock(subflow);
		if (!ssk)
			continue;

		xlayer_extract_ip_address(ssk, ip_str);
		
		if (xlayer_is_wifi_interface(ip_str)) {
			wifi_subflow = subflow;
		} else {
			nr_subflow = subflow;
		}
	}

	/* Get current network metrics */
	xlayer_get_metrics(&wifi_ratio, &nr_ratio);

	/* Cross-layer subflow selection based on bitrates */
	if (wifi_subflow && nr_subflow) {
		/* Both available - select based on performance */
		if (wifi_ratio > 0 && nr_ratio > 0) {
			if (wifi_ratio > nr_ratio) {
				best_subflow = wifi_subflow;
				pr_debug("xlayer: Selected WiFi (higher bitrate: %ld > %ld)\n", 
					 wifi_ratio, nr_ratio);
			} else {
				best_subflow = nr_subflow;  
				pr_debug("xlayer: Selected 5G (higher bitrate: %ld >= %ld)\n",
					 nr_ratio, wifi_ratio);
			}
		} else {
			/* No metrics - fallback to RTT */
			best_subflow = fastest_subflow;
			pr_debug("xlayer: No valid metrics, falling back to minRTT\n");
		}
	} else if (wifi_subflow) {
		best_subflow = wifi_subflow;
		pr_debug("xlayer: Selected WiFi (only option)\n");
	} else if (nr_subflow) {
		best_subflow = nr_subflow;
		pr_debug("xlayer: Selected 5G (only option)\n");
	} else {
		best_subflow = fastest_subflow;
		pr_debug("xlayer: No classified subflows, using minRTT fallback\n");
	}

	if (!best_subflow) {
		return -EINVAL;
	}

	/* If selected fastest subflow, no HoL risk */
	if (best_subflow == fastest_subflow) {
		mptcp_subflow_set_scheduled(best_subflow, 1);
		pr_debug("XLayer: selected best subflow (no HoL risk)\n");
		return 0;
	}

	/* HoL-blocking prevention logic (BLEST integration) */
	best_ssk = mptcp_subflow_tcp_sock(best_subflow);
	fastest_ssk = mptcp_subflow_tcp_sock(fastest_subflow);
	best_tp = tcp_sk(best_ssk);
	meta_tp = tcp_sk((struct sock *)msk);

	/* Update lambda based on current conditions */
	common_update_lambda(msk, best_subflow);

	/* Simple HoL prevention: check if slow subflow would block fast one */
	slow_inflight = best_tp->write_seq - best_tp->snd_una;
	fast_capacity = tcp_sk(fastest_ssk)->snd_cwnd * tcp_sk(fastest_ssk)->mss_cache;
	meta_wnd = tcp_wnd_end(meta_tp) - meta_tp->write_seq;

	/* If using slow subflow would prevent fast subflow from sending efficiently */
	if (slow_inflight * xlayer_global.lambda_1000 > fast_capacity * 1000 && meta_wnd < fast_capacity) {
		/* HoL blocking risk - use fastest subflow */
		mptcp_subflow_set_scheduled(fastest_subflow, 1);
		xlayer_global.hol_prevented++;
		pr_debug("XLayer: HoL prevention triggered - using fastest subflow\n");
		return 0;
	}

	/* Safe to use cross-layer selected subflow */
	mptcp_subflow_set_scheduled(best_subflow, 1);
	pr_debug("XLayer: using cross-layer selected subflow safely (no HoL blocking risk)\n");
	
	return 0;
}

/* MPTCP Scheduler Registration */

/* XLayer scheduler operations structure */
static struct mptcp_sched_ops mptcp_sched_xlayer = {
	.init		= mptcp_sched_xlayer_init,
	.release	= mptcp_sched_xlayer_release,
	.get_subflow	= mptcp_sched_xlayer_get_subflow,
	.name		= "xlayer",
	.owner		= THIS_MODULE,
};

/* Module Initialization and Cleanup */

/**
 * mptcp_xlayer_register() - Initialize XLayer scheduler module
 *
 * Sets up proc interface, starts metrics thread, and registers with MPTCP.
 * Usage: echo xlayer > /proc/sys/net/mptcp/scheduler
 * 
 * Return: 0 on success, negative error on failure
 */
static int __init mptcp_xlayer_register(void)
{
	int ret;
	
	// pr_info("xlayer: Initializing XLayer MPTCP scheduler\n");
	
	/* Initialize metrics collection system (proc + thread) */
	ret = xlayer_metrics_init();
	if (ret < 0) {
		pr_err("xlayer: Failed to initialize metrics system: %d\n", ret);
		return ret;
	}

	/* Register scheduler with MPTCP framework */
	ret = mptcp_register_scheduler(&mptcp_sched_xlayer);
	if (ret < 0) {
		pr_err("xlayer: Failed to register scheduler: %d\n", ret);
		xlayer_metrics_cleanup();
		return ret;
	}

	// pr_info("xlayer: XLayer scheduler registered successfully\n");
	// pr_info("xlayer: Use 'echo xlayer > /proc/sys/net/mptcp/scheduler' to activate\n");
	return 0;
}

/**
 * mptcp_xlayer_unregister() - Clean up XLayer scheduler module
 *
 * Stops metrics thread, removes proc interface, and unregisters scheduler.
 */
static void __exit mptcp_xlayer_unregister(void)
{
	// pr_info("xlayer: Shutting down XLayer MPTCP scheduler\n");
	
	/* Unregister scheduler from MPTCP framework */
	mptcp_unregister_scheduler(&mptcp_sched_xlayer);
	
	/* Clean up metrics collection system */
	xlayer_metrics_cleanup();
	
	// pr_info("xlayer: XLayer scheduler shutdown complete\n");
}

/* Module entry and exit points */
module_init(mptcp_xlayer_register);
module_exit(mptcp_xlayer_unregister);

/* Module metadata */
MODULE_AUTHOR("Simone Ferlin, Updated for MPTCP v1 by Tiago Sousa");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("XLayer scheduler for MPTCP v1 with WiFi/5G awareness");
MODULE_VERSION("1.0");
