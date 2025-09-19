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

#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/tcp.h>
#include <linux/limits.h>
#include <linux/inet.h>
#include <net/tcp.h>
#include <net/mptcp.h>
#include <net/inet_connection_sock.h>
#include "protocol.h"


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


#endif /* __MPTCP_MINRTT_H */
