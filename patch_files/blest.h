/* SPDX-License-Identifier: GPL-2.0 */


#ifndef __MPTCP_BLEST_H
#define __MPTCP_BLEST_H

#include "utils.h"

/* Estimate how many bytes will be sent during a given time */
static u32 __maybe_unused blest_estimate_bytes(struct sock *sk, u32 time_us)
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

	return div_u64(((u64)packets) * tp->mss_cache, 1000);
}

/* Estimate linger time for a subflow */
static u32 __maybe_unused blest_estimate_linger_time(struct sock *ssk)
{
	struct tcp_sock *tp;
	u32 estimate, slope, inflight, cwnd;

	if (!ssk)
		return U32_MAX;

	tp = tcp_sk(ssk);
	inflight = tcp_packets_in_flight(tp) + 1; /* account for new packet */
	cwnd = tp->snd_cwnd;

	if (inflight >= cwnd) {
		estimate = tp->srtt_us;
	} else {
		slope = 0; // blest_global_data->max_srtt_us - blest_global_data->min_srtt_us;
		if (cwnd == 0)
			cwnd = 1;
		estimate = tp->srtt_us + (slope * inflight) / cwnd;
	}

	return max(tp->srtt_us >> 3, estimate);
}


/* Find the minimum RTT subflow using shared logic */
static inline struct mptcp_subflow_context *find_min_rtt_subflow(struct mptcp_sock *msk)
{
	struct mptcp_subflow_context *subflow, *min_subflow = NULL;
	u32 min_rtt = U32_MAX;
	u32 current_rtt;

	mptcp_for_each_subflow(msk, subflow) {
		if (!mptcp_subflow_is_available(subflow))
			continue;


		current_rtt = mptcp_subflow_get_rtt(subflow);
		
		if (current_rtt < min_rtt) {
			min_rtt = current_rtt;
			min_subflow = subflow;
		}
	}

	return min_subflow;
}

#endif /* __MPTCP_BLEST_H */