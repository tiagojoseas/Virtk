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
 * Original Design: Simone Ferlin, Ozgu Alay, Olivier Mehani, Roksana Boreli
 * MPTCP v1 Implementation: Tiago Sousa <tiagojoseas@gmail.com>
 */

#include "blest.h"
#include "xlayer.h"


/* XLayer global state */
static BlestConnData *xlayer_global;

static void mptcp_sched_xlayer_init(struct mptcp_sock *msk)
{
	/* Allocate global state */
	xlayer_global = kzalloc(sizeof(BlestConnData), GFP_KERNEL);
	xlayer_global->lambda_1000 = lambda * 100;
	xlayer_global->total_decisions = 0;
	xlayer_global->hol_prevented = 0;
	pr_info("XLayer scheduler initialized (lambda=%d.%d)\n", 
		lambda / 10, lambda % 10);
}

static void mptcp_sched_xlayer_release(struct mptcp_sock *msk)
{
	pr_info("XLayer: %u decisions, %u HoL prevented\n", 
		xlayer_global->total_decisions, xlayer_global->hol_prevented);
	kfree(xlayer_global);
}

static int mptcp_sched_xlayer_get_subflow(struct mptcp_sock *msk,
					 struct mptcp_sched_data *data)
{
	struct mptcp_subflow_context *default_subflow, *minrtt_subflow, *maxdr_subflow, *wifi_subflow, *nr_subflow, *subflow;
	struct sock *best_ssk, *maxdr_ssk, *meta_sk = (struct sock *)msk;
	struct tcp_sock *best_tp, *meta_tp;
	char ip_str[16];
	long wifi_ratio, nr_ratio;

	xlayer_global->total_decisions++;

	minrtt_subflow = find_min_rtt_subflow(msk, xlayer_global);
	if (!minrtt_subflow) {
		return -EINVAL;
	}

	default_subflow = __mptcp_sched_minrtt_get_subflow(msk, data);
	if (!default_subflow) {
		default_subflow = minrtt_subflow;
	}

	/* Classify subflows by network interface (WiFi vs 5G) */
	mptcp_for_each_subflow(msk, subflow) {
		if (!mptcp_subflow_is_available(subflow))
			continue;

		best_ssk = mptcp_subflow_tcp_sock(subflow);
		if (!best_ssk)
			continue;

		xlayer_extract_ip_address(best_ssk, ip_str);
		
		if (xlayer_is_wifi_interface(ip_str)) {
			wifi_subflow = subflow;
		} else {
			nr_subflow = subflow;
		}
	}

	// Reset best_ssk
	best_ssk = NULL;
	
	/* Get current network metrics */
	xlayer_get_metrics(&wifi_ratio, &nr_ratio);

	/* Cross-layer subflow selection based on bitrates */
	if (wifi_subflow && nr_subflow) {
		/* Both available - select based on performance */
		if (wifi_ratio > 0 && nr_ratio > 0) {
			if (wifi_ratio > nr_ratio) {
				maxdr_subflow = wifi_subflow;
			} else {
				maxdr_subflow = nr_subflow;
			}
		} else {
			/* No metrics - fallback to RTT */
			maxdr_subflow = minrtt_subflow;
		}
	} else if (wifi_subflow) {
		maxdr_subflow = wifi_subflow;
	} else if (nr_subflow) {
		maxdr_subflow = nr_subflow;
	} else {
		maxdr_subflow = minrtt_subflow;
		pr_info("XLayer: No available subflows!\n");
	}
	
	best_ssk = mptcp_subflow_tcp_sock(default_subflow);
	maxdr_ssk = mptcp_subflow_tcp_sock(maxdr_subflow);


	if (best_ssk && maxdr_ssk && best_ssk != maxdr_ssk) {
		u32 slow_linger_time, fast_bytes, slow_inflight_bytes, slow_bytes, avail_space;
		u32 buffered_bytes = 0;

		meta_tp = tcp_sk(meta_sk);
		best_tp = tcp_sk(best_ssk);

		blest_update_lambda(meta_sk, best_ssk, xlayer_global);

		slow_linger_time = blest_estimate_linger_time(best_ssk, xlayer_global);
		fast_bytes = blest_estimate_bytes(maxdr_ssk, slow_linger_time, xlayer_global);

		slow_inflight_bytes = best_tp->write_seq - best_tp->snd_una;
		slow_bytes = buffered_bytes + slow_inflight_bytes;

		avail_space = (slow_bytes < meta_tp->snd_wnd) ? 
			(meta_tp->snd_wnd - slow_bytes) : 0;

		if (fast_bytes > avail_space) {
			xlayer_global->hol_prevented++;
			return -EAGAIN;
		}
	}

	mptcp_subflow_set_scheduled(default_subflow, 1);
	return 0;
}

static struct mptcp_sched_ops mptcp_sched_xlayer = {
	.init		= mptcp_sched_xlayer_init,
	.release	= mptcp_sched_xlayer_release,
	.get_subflow	= mptcp_sched_xlayer_get_subflow,
	.name		= "xlayer",
	.owner		= THIS_MODULE,
};

static int __init mptcp_xlayer_register(void)
{
	int ret;
	
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

	return 0;
}

static void __exit mptcp_xlayer_unregister(void)
{
	/* Unregister scheduler from MPTCP framework */
	mptcp_unregister_scheduler(&mptcp_sched_xlayer);
	
	/* Clean up metrics collection system */
	xlayer_metrics_cleanup();
}

module_init(mptcp_xlayer_register);
module_exit(mptcp_xlayer_unregister);

MODULE_AUTHOR("Simone Ferlin, Updated for MPTCP v1 by Tiago Sousa");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("XLayer scheduler for MPTCP v1 with WiFi/5G awareness");
MODULE_VERSION("1.0");
