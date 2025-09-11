/* SPDX-License-Identifier: GPL-2.0 */
/*
 * MPTCP MinRTT Scheduler - Shared Functions Header
 *
 * This header provides common functions for MPTCP schedulers
 * that need RTT-based subflow selection and availability checks.
 *
 * These functions are shared between MinRTT and BLEST schedulers
 * to avoid code duplication while maintaining modularity.
 */

#ifndef __MPTCP_MINRTT_H
#define __MPTCP_MINRTT_H

#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/tcp.h>
#include <linux/limits.h>
#include <linux/inet.h>
#include <net/tcp.h>
#include <net/inet_connection_sock.h>


/* Check if subflow is in a good state for sending data */
static inline bool mptcp_subflow_is_available(struct mptcp_subflow_context *subflow)
{
	struct sock *ssk = mptcp_subflow_tcp_sock(subflow);
	struct tcp_sock *tp;
	struct inet_connection_sock *icsk;
	
	if (!ssk)
		return false;
	
	/* Check if subflow is active and can send data */
	if (!__mptcp_subflow_active(subflow))
		return false;

	/* Skip if socket is not in a good state for sending */
	if (!__tcp_can_send(ssk))
		return false;

	/* Skip if no memory available for sending */
	if (!sk_stream_memory_free(ssk))
		return false;

	tp = tcp_sk(ssk);
	icsk = inet_csk(ssk);
	
	/* Check congestion window - avoid subflows that are cwnd-limited */
	if (tcp_packets_in_flight(tp) >= tp->snd_cwnd)
		return false;
	
	/* Avoid subflows in problematic congestion states */
	if (icsk->icsk_ca_state == TCP_CA_Loss)
		return false;
	
	/* Avoid subflows in recovery if we have alternatives */
	if (icsk->icsk_ca_state == TCP_CA_Recovery && tp->snd_cwnd <= 4)
		return false;
	
	/* Check if subflow has sufficient send window */
	if (tcp_wnd_end(tp) <= tp->snd_nxt)
		return false;
	
	/* Avoid subflows that are probe timeout (very slow) */
	if (icsk->icsk_probes_out > 0)
		return false;
	
	return true;
}

/* Get effective RTT for a subflow */
static inline u32 mptcp_subflow_get_rtt(struct mptcp_subflow_context *subflow)
{
	struct sock *ssk = mptcp_subflow_tcp_sock(subflow);
	struct tcp_sock *tp;
	u32 rtt;
	
	if (!ssk)
		return U32_MAX;
	
	tp = tcp_sk(ssk);
	
	/* Use smoothed RTT (srtt_us) as the RTT metric */
	rtt = tp->srtt_us >> 3; /* Convert from 8*RTT to RTT */
	
	/* If no RTT measurement available, use a high value */
	if (rtt == 0)
		rtt = U32_MAX;
	
	return rtt;
}

/* Select best subflow from a list of candidates */
static inline struct mptcp_subflow_context *
get_minrtt_sock(struct mptcp_sock *msk, bool use_backup, bool check_send)
{
	struct mptcp_subflow_context *subflow, *best_subflow = NULL;
	u32 min_rtt = U32_MAX;
	u32 current_rtt;
	bool is_backup;
	struct sock *ssk;
	
	mptcp_for_each_subflow(msk, subflow) {
		if (!mptcp_subflow_is_available(subflow))
			continue;
		
		/* Additional check to ensure subflow can send data */
		if(check_send) {
			ssk = mptcp_subflow_tcp_sock(subflow);
			if (!ssk || !__tcp_can_send(ssk))
				continue;
		}
		
		/* Check if this is a backup subflow */
		is_backup = !!(subflow->backup);
		
		/* Skip if we're not looking for backup flows and this is backup */
		if (!use_backup && is_backup)
			continue;
		
		/* Skip if we're looking for backup flows and this is not backup */
		if (use_backup && !is_backup)
			continue;
		
		current_rtt = mptcp_subflow_get_rtt(subflow);
		
		/* Select subflow with minimum RTT */
		if (current_rtt < min_rtt) {
			min_rtt = current_rtt;
			best_subflow = subflow;
		}
	}
	
	return best_subflow;
}

/* Common fallback logic for selecting any available subflow */
static inline struct mptcp_subflow_context *
mptcp_select_fallback_subflow(struct mptcp_sock *msk)
{
	struct mptcp_subflow_context *subflow;
	struct sock *ssk;
	
	mptcp_for_each_subflow(msk, subflow) {
		ssk = mptcp_subflow_tcp_sock(subflow);
		if (!ssk)
			continue;

		if (__mptcp_subflow_active(subflow) && 
		    __tcp_can_send(ssk)) {
			return subflow;
		}
	}
	
	return NULL;
}

static inline struct mptcp_subflow_context * __mptcp_sched_minrtt_get_subflow(struct mptcp_sock *msk,
					  struct mptcp_sched_data *data)
{
	struct mptcp_subflow_context *best_subflow;
	
	/* Try to get best active subflow first */
	best_subflow = get_minrtt_sock(msk, 0, true);
	if (best_subflow) {
		// mptcp_subflow_set_scheduled(best_subflow, 1);

		return best_subflow;
	}
	
	/* Fall back to backup subflows */
	best_subflow = get_minrtt_sock(msk, 1, true);
	if (best_subflow) {
		// mptcp_subflow_set_scheduled(best_subflow, 1);
		return best_subflow;
	}
	
	// /* Try fallback logic as last resort */
	// best_subflow = mptcp_select_fallback_subflow(msk);
	// if (best_subflow) {
	// 	mptcp_subflow_set_scheduled(best_subflow, 1);
	// 	return 0;
	// }
	
	/* No suitable subflow found */
	return NULL;
}

/**************************************
 * BLEST-specific Functions
 ***************************************/

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
struct blest_conn_data {
	s16 lambda_1000; /* values range from min_lambda * 100 to max_lambda * 100 */
	u32 last_lambda_update;
	u32 min_srtt_us;
	u32 max_srtt_us;
};

/* Global BLEST connection data - simplified for v1 */
static struct blest_conn_data common_global_data = {
	.lambda_1000 = 1200, /* lambda * 100 */
	.last_lambda_update = 0,
	.min_srtt_us = U32_MAX,
	.max_srtt_us = 0,
};

/* Estimate how many bytes will be sent during a given time */
static u32 __maybe_unused common_estimate_bytes(struct mptcp_subflow_context *subflow, u32 time_us)
{
	struct sock *ssk = mptcp_subflow_tcp_sock(subflow);
	struct tcp_sock *tp;
	u32 avg_rtt, num_rtts, ca_cwnd, packets;
	u32 mss;

	if (!ssk)
		return 0;

	tp = tcp_sk(ssk);
	mss = tcp_current_mss(ssk);
	
	avg_rtt = (common_global_data.min_srtt_us + common_global_data.max_srtt_us) / 2;
	if (avg_rtt == 0)
		num_rtts = 1; /* sanity */
	else
		num_rtts = (time_us / avg_rtt) + 1; /* round up */

	/* Estimate packets during num_rtts (simplified congestion control model) */
	if (tp->snd_ssthresh == TCP_INFINITE_SSTHRESH) {
		/* We are in initial slow start */
		if (num_rtts > 16)
			num_rtts = 16; /* cap for sanity */
		packets = tp->snd_cwnd * ((1 << num_rtts) - 1);
	} else {
		ca_cwnd = max(tp->snd_cwnd, tp->snd_ssthresh + 1);
		packets = (ca_cwnd + (num_rtts - 1) / 2) * num_rtts;
	}

	return div_u64(((u64)packets) * mss * common_global_data.lambda_1000, 1000);
}

/* Estimate linger time for a subflow */
static u32 __maybe_unused common_estimate_linger_time(struct mptcp_subflow_context *subflow)
{
	struct sock *ssk = mptcp_subflow_tcp_sock(subflow);
	struct tcp_sock *tp;
	u32 estimate, slope, inflight, cwnd;

	if (!ssk)
		return U32_MAX;

	tp = tcp_sk(ssk);
	inflight = tcp_packets_in_flight(tp) + 1; /* account for new packet */
	cwnd = tp->snd_cwnd;

	if (inflight >= cwnd) {
		estimate = common_global_data.max_srtt_us;
	} else {
		slope = common_global_data.max_srtt_us - common_global_data.min_srtt_us;
		if (cwnd == 0)
			cwnd = 1; /* sanity */
		estimate = common_global_data.min_srtt_us + (slope * inflight) / cwnd;
	}

	return max(tp->srtt_us >> 3, estimate);
}

/* Update lambda based on retransmission behavior */
static void __maybe_unused common_update_lambda(struct mptcp_sock *msk, struct mptcp_subflow_context *subflow)
{
	struct sock *ssk = mptcp_subflow_tcp_sock(subflow);
	struct tcp_sock *tp;
	u32 min_rtt_estimate;

	if (!ssk)
		return;

	tp = tcp_sk(ssk);
	min_rtt_estimate = common_global_data.min_srtt_us >> 3;
	
	/* Update lambda every min RTT */
	if (tcp_jiffies32 - common_global_data.last_lambda_update < 
	    usecs_to_jiffies(min_rtt_estimate))
		return;

	/* Check for retransmissions at meta level */
	if (READ_ONCE(msk->bytes_retrans) > 0) {
		/* Need to slow down on the slow flow */
		common_global_data.lambda_1000 += dyn_lambda_bad;
	} else {
		/* Use the slow flow more */
		common_global_data.lambda_1000 -= dyn_lambda_good;
	}

	/* Cap lambda_1000 to its value range */
	common_global_data.lambda_1000 = min_t(s16, common_global_data.lambda_1000, max_lambda * 100);
	common_global_data.lambda_1000 = max_t(s16, common_global_data.lambda_1000, min_lambda * 100);

	common_global_data.last_lambda_update = tcp_jiffies32;
}

/* BLEST-specific subflow availability check with RTT bounds update */
static inline bool blest_subflow_is_available(struct mptcp_subflow_context *subflow)
{
	struct sock *ssk;
	struct tcp_sock *tp;
	
	/* Use the shared availability check first */
	if (!mptcp_subflow_is_available(subflow))
		return false;
	
	/* BLEST-specific: Update RTT bounds for estimation algorithms */
	ssk = mptcp_subflow_tcp_sock(subflow);
	if (ssk) {
		tp = tcp_sk(ssk);
		common_global_data.min_srtt_us = min(common_global_data.min_srtt_us, tp->srtt_us);
		common_global_data.max_srtt_us = max(common_global_data.max_srtt_us, tp->srtt_us);
	}
	
	return true;
}

/* Find the minimum RTT subflow using shared logic */
static inline struct mptcp_subflow_context *find_min_rtt_subflow(struct mptcp_sock *msk)
{
	struct mptcp_subflow_context *subflow, *min_subflow = NULL;
	u32 min_rtt = U32_MAX;
	u32 current_rtt;

	mptcp_for_each_subflow(msk, subflow) {
		if (!blest_subflow_is_available(subflow))
			continue;

		current_rtt = mptcp_subflow_get_rtt(subflow);
		
		if (current_rtt < min_rtt) {
			min_rtt = current_rtt;
			min_subflow = subflow;
		}
	}

	return min_subflow;
}



#endif /* __MPTCP_MINRTT_H */
