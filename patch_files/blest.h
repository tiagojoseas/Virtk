/* SPDX-License-Identifier: GPL-2.0 */


#ifndef __MPTCP_BLEST_H
#define __MPTCP_BLEST_H

#include "utils.h"

static unsigned char lambda __read_mostly = 12;
module_param(lambda, byte, 0644);
MODULE_PARM_DESC(lambda, "Divided by 10 for scaling factor of fast flow rate estimation");

static unsigned char max_lambda __read_mostly = 13;
module_param(max_lambda, byte, 0644);
MODULE_PARM_DESC(max_lambda, "Divided by 10 for maximum scaling factor of fast flow rate estimation");

static unsigned char min_lambda __read_mostly = 10;
module_param(min_lambda, byte, 0644);
MODULE_PARM_DESC(min_lambda, "Divided by 10 for minimum scaling factor of fast flow rate estimation");

static unsigned char dyn_lambda_good = 10; /* 1% */
module_param(dyn_lambda_good, byte, 0644);
MODULE_PARM_DESC(dyn_lambda_good, "Decrease of lambda in positive case.");

static unsigned char dyn_lambda_bad = 40; /* 4% */
module_param(dyn_lambda_bad, byte, 0644);
MODULE_PARM_DESC(dyn_lambda_bad, "Increase of lambda in negative case.");

/* BLEST per-connection data */
typedef struct {
	s16 lambda_1000; /* values range from min_lambda * 100 to max_lambda * 100 */
	u32 last_lambda_update;
	u32 min_srtt_us;
	u32 max_srtt_us;
	u32 total_decisions;
	u32 hol_prevented;
} BlestConnData;



/* Estimate how many bytes will be sent during a given time */
static u32 __maybe_unused blest_estimate_bytes(struct sock *sk, u32 time_us, BlestConnData * blest_global_data)
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

	// return div_u64(((u64)packets) * tp->mss_cache * blest_global_data->lambda_1000, 1000);
	return div_u64(((u64)packets) * tp->mss_cache, 1000);
}

/* Estimate linger time for a subflow */
static u32 __maybe_unused blest_estimate_linger_time(struct sock *ssk, BlestConnData * blest_global_data)
{
	struct tcp_sock *tp;
	u32 estimate, slope, inflight, cwnd;

	if (!ssk)
		return U32_MAX;

	tp = tcp_sk(ssk);
	inflight = tcp_packets_in_flight(tp) + 1; /* account for new packet */
	cwnd = tp->snd_cwnd;

	if (inflight >= cwnd) {
		estimate = blest_global_data->max_srtt_us;
	} else {
		slope = blest_global_data->max_srtt_us - blest_global_data->min_srtt_us;
		if (cwnd == 0)
			cwnd = 1; /* sanity */
		estimate = blest_global_data->min_srtt_us + (slope * inflight) / cwnd;
	}

	return max(tp->srtt_us >> 3, estimate);
}

/* Update lambda based on retransmission behavior */
static void __maybe_unused blest_update_lambda(struct sock *meta_sk, struct sock *sk, BlestConnData * blest_global_data)
{
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct tcp_sock *tp = tcp_sk(sk);
	u32 min_srtt_us = tp->srtt_us;

	if (tcp_jiffies32 - blest_global_data->last_lambda_update < usecs_to_jiffies(min_srtt_us >> 3))
		return;

	if (meta_tp->retrans_stamp) {
		blest_global_data->lambda_1000 += dyn_lambda_bad;
	} else {
		blest_global_data->lambda_1000 -= dyn_lambda_good;
	}

	/* cap lambda_1000 to its value range */
	blest_global_data->lambda_1000 = min_t(s16, blest_global_data->lambda_1000, max_lambda * 100);
	blest_global_data->lambda_1000 = max_t(s16, blest_global_data->lambda_1000, min_lambda * 100);

	blest_global_data->last_lambda_update = tcp_jiffies32;
}

/* BLEST-specific subflow availability check */
static inline bool blest_subflow_is_available(struct mptcp_subflow_context *subflow, BlestConnData * blest_global_data)
{
	/* Use the shared availability check first */
	return mptcp_subflow_is_available(subflow);
}

/* Find the minimum RTT subflow using shared logic */
static inline struct mptcp_subflow_context *find_min_rtt_subflow(struct mptcp_sock *msk, BlestConnData * blest_global_data)
{
	struct mptcp_subflow_context *subflow, *min_subflow = NULL;
	u32 min_rtt = U32_MAX;
	u32 current_rtt;

	mptcp_for_each_subflow(msk, subflow) {
		if (!blest_subflow_is_available(subflow, blest_global_data))
			continue;

		blest_global_data->min_srtt_us = min(blest_global_data->min_srtt_us, mptcp_subflow_get_rtt(subflow));
		blest_global_data->max_srtt_us = max(blest_global_data->max_srtt_us, mptcp_subflow_get_rtt(subflow));

		current_rtt = mptcp_subflow_get_rtt(subflow);
		
		if (current_rtt < min_rtt) {
			min_rtt = current_rtt;
			min_subflow = subflow;
		}
	}

	return min_subflow;
}

#endif /* __MPTCP_BLEST_H */