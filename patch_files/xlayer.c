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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/tcp.h>
#include <net/tcp.h>
#include <net/mptcp.h>
#include "protocol.h"
#include "common_lib.h"
#include "xlayer_metrics.h"

static unsigned char dyn_lambda_good __read_mostly = 10;
module_param(dyn_lambda_good, byte, 0644);
MODULE_PARM_DESC(dyn_lambda_good, "Decrease of lambda in positive case.");

static unsigned char dyn_lambda_bad __read_mostly = 40;
module_param(dyn_lambda_bad, byte, 0644);
MODULE_PARM_DESC(dyn_lambda_bad, "Increase of lambda in negative case.");

/* XLayer global state */
static struct {
	s16 lambda_1000;
	u32 total_decisions;
	u32 hol_prevented;
	u32 last_lambda_update;
} xlayer_global = {
	.lambda_1000 = 1200, /* default 1.2 */
	.total_decisions = 0,
	.hol_prevented = 0,
	.last_lambda_update = 0
};

static void xlayer_update_lambda(struct sock *meta_sk, struct sock *sk)
{
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct tcp_sock *tp = tcp_sk(sk);
	u32 min_srtt_us = tp->srtt_us;

	if (tcp_jiffies32 - xlayer_global.last_lambda_update < usecs_to_jiffies(min_srtt_us >> 3))
		return;

	if (meta_tp->retrans_stamp) {
		xlayer_global.lambda_1000 += dyn_lambda_bad;
	} else {
		xlayer_global.lambda_1000 -= dyn_lambda_good;
	}

	/* cap lambda_1000 to its value range */
	if (xlayer_global.lambda_1000 > (max_lambda * 100))
		xlayer_global.lambda_1000 = max_lambda * 100;
	if (xlayer_global.lambda_1000 < (min_lambda * 100))
		xlayer_global.lambda_1000 = min_lambda * 100;

	xlayer_global.last_lambda_update = tcp_jiffies32;
}

static u32 xlayer_estimate_bytes(struct sock *sk, u32 time_us)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 avg_rtt, num_rtts, ca_cwnd, packets;
	u32 min_srtt_us = tp->srtt_us;
	u32 max_srtt_us = tp->srtt_us;

	avg_rtt = (min_srtt_us + max_srtt_us) / 2;
	if (avg_rtt == 0)
		num_rtts = 1;
	else
		num_rtts = (time_us / avg_rtt) + 1;

	if (tp->snd_ssthresh == TCP_INFINITE_SSTHRESH) {
		if (num_rtts > 16)
			num_rtts = 16;
		packets = tp->snd_cwnd * ((1 << num_rtts) - 1);
	} else {
		ca_cwnd = max(tp->snd_cwnd, tp->snd_ssthresh + 1);
		packets = (ca_cwnd + (num_rtts - 1) / 2) * num_rtts;
	}

	return div_u64(((u64)packets) * tp->mss_cache * xlayer_global.lambda_1000, 1000);
}

static u32 xlayer_estimate_linger_time(struct sock *sk)
{
	struct tcp_sock *tp = tcp_sk(sk);
	u32 estimate, slope, inflight, cwnd;
	u32 min_srtt_us = tp->srtt_us;
	u32 max_srtt_us = tp->srtt_us;

	inflight = tcp_packets_in_flight(tp) + 1;
	cwnd = tp->snd_cwnd;

	if (inflight >= cwnd) {
		estimate = max_srtt_us;
	} else {
		slope = max_srtt_us - min_srtt_us;
		if (cwnd == 0)
			cwnd = 1;
		estimate = min_srtt_us + (slope * inflight) / cwnd;
	}

	return (tp->srtt_us > estimate) ? tp->srtt_us : estimate;
}

static void mptcp_sched_xlayer_init(struct mptcp_sock *msk)
{
	xlayer_global.lambda_1000 = lambda * 100;
	xlayer_global.total_decisions = 0;
	xlayer_global.hol_prevented = 0;
	pr_info("XLayer scheduler initialized (lambda=%d.%d)\n", 
		lambda / 10, lambda % 10);
}

static void mptcp_sched_xlayer_release(struct mptcp_sock *msk)
{
	pr_info("XLayer: %u decisions, %u HoL prevented\n", 
		xlayer_global.total_decisions, xlayer_global.hol_prevented);
}

static int mptcp_sched_xlayer_get_subflow(struct mptcp_sock *msk,
					 struct mptcp_sched_data *data)
{
	struct mptcp_subflow_context *subflow, *fastest_subflow = NULL;
	struct mptcp_subflow_context *wifi_subflow = NULL, *nr_subflow = NULL;
	struct mptcp_subflow_context *best_subflow = NULL;
	struct sock *ssk, *fastest_ssk, *best_ssk, *meta_sk;
	struct tcp_sock *best_tp, *meta_tp;
	char ip_str[16];
	long wifi_ratio, nr_ratio;

	xlayer_global.total_decisions++;

	/* Find fastest subflow using shared function */
	fastest_subflow = find_min_rtt_subflow(msk);
	if (!fastest_subflow) {
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
			} else {
				best_subflow = nr_subflow;
			}
		} else {
			/* No metrics - fallback to RTT */
			best_subflow = fastest_subflow;
		}
	} else if (wifi_subflow) {
		best_subflow = wifi_subflow;
	} else if (nr_subflow) {
		best_subflow = nr_subflow;
	} else {
		best_subflow = fastest_subflow;
	}

	if (!best_subflow) {
		return -EINVAL;
	}

	/* If selected fastest subflow, no HoL risk */
	if (best_subflow == fastest_subflow) {
		data->contexts[0] = best_subflow;
		data->subflows = 1;
		return 0;
	}

	/* HoL-blocking prevention logic using BLEST algorithm */
	best_ssk = mptcp_subflow_tcp_sock(best_subflow);
	fastest_ssk = mptcp_subflow_tcp_sock(fastest_subflow);
	meta_sk = (struct sock *)msk;

	if (best_ssk && fastest_ssk && best_ssk != fastest_ssk) {
		u32 slow_linger_time, fast_bytes, slow_inflight_bytes, slow_bytes, avail_space;
		u32 buffered_bytes = 0;

		meta_tp = tcp_sk(meta_sk);
		best_tp = tcp_sk(best_ssk);

		xlayer_update_lambda(meta_sk, best_ssk);

		slow_linger_time = xlayer_estimate_linger_time(best_ssk);
		fast_bytes = xlayer_estimate_bytes(fastest_ssk, slow_linger_time);

		slow_inflight_bytes = best_tp->write_seq - best_tp->snd_una;
		slow_bytes = buffered_bytes + slow_inflight_bytes;

		avail_space = (slow_bytes < meta_tp->snd_wnd) ? 
			(meta_tp->snd_wnd - slow_bytes) : 0;

		if (fast_bytes > avail_space) {
			/* HoL blocking risk - use fastest subflow */
			xlayer_global.hol_prevented++;
			data->contexts[0] = fastest_subflow;
			data->subflows = 1;
			return 0;
		}
	}

	/* Safe to use cross-layer selected subflow */
	data->contexts[0] = best_subflow;
	data->subflows = 1;
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
