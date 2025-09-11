// SPDX-License-Identifier: GPL-2.0
/*	MPTCP BLEST Scheduler - Simplified with HoL Prevention
 *
 *	BLEST (Blocking Estimation) Scheduler to reduce HoL-blocking 
 *	and spurious retransmissions in MPTCP.
 *
 *	Algorithm Design:
 *	Simone Ferlin <ferlin@simula.no>
 *	Ozgu Alay <ozgu@simula.no>
 *	Olivier Mehani <olivier.mehani@nicta.com.au>
 *	Roksana Boreli <roksana.boreli@nicta.com.au>
 *
 *	Simplified Implementation:
 *	Tiago Sousa
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include "protocol.h"
#include "common_lib.h"

/* BLEST uses shared lambda parameter from common_lib.h */

/* BLEST global data */
static struct {
	s16 lambda_1000;
	u32 total_decisions;
	u32 hol_prevented;
} blest_global = {
	.lambda_1000 = 0,
	.total_decisions = 0,
	.hol_prevented = 0
};

static void mptcp_sched_blest_init(struct mptcp_sock *msk)
{
	blest_global.lambda_1000 = lambda * 100;
	blest_global.total_decisions = 0;
	blest_global.hol_prevented = 0;
	// pr_info("BLEST scheduler with HoL prevention initialized (lambda=%d.%d)\n", lambda / 10, lambda % 10);
}

static void mptcp_sched_blest_release(struct mptcp_sock *msk)
{
	// pr_info("BLEST scheduler released - Total decisions: %u, HoL prevented: %u\n", blest_global.total_decisions, blest_global.hol_prevented);
}

static int mptcp_sched_blest_get_subflow(struct mptcp_sock *msk,
					 struct mptcp_sched_data *data)
{
	struct mptcp_subflow_context *default_subflow, *minrtt_subflow;
	struct sock *best_ssk, *fastest_ssk, *meta_sk;
	struct tcp_sock *best_tp, *meta_tp;
	u32 slow_inflight, fast_capacity, meta_wnd;

	blest_global.total_decisions++;

	/* Find fastest (min RTT) subflow using shared function */
	minrtt_subflow = find_min_rtt_subflow(msk);
	if (!minrtt_subflow) {
		pr_debug("BLEST: no available subflow found\n");
		return -EINVAL;
	}

	/* Find overall best subflow (may be different due to congestion) */
	default_subflow = __mptcp_sched_minrtt_get_subflow(msk, data);
	if (!default_subflow) {
		/* Fallback to fastest subflow if no best subflow found */
		default_subflow = minrtt_subflow;
	}

	best_ssk = mptcp_subflow_tcp_sock(default_subflow);
	fastest_ssk = mptcp_subflow_tcp_sock(minrtt_subflow);

	/* If same subflow, no HoL risk */
	if (default_subflow == minrtt_subflow) {
		mptcp_subflow_set_scheduled(default_subflow, 1);
		pr_debug("BLEST: selected best subflow (no HoL risk)\n");
		return 0;
	}

	/* HoL-blocking prevention logic */
	best_tp = tcp_sk(best_ssk);
	meta_sk = (struct sock *)msk;
	meta_tp = tcp_sk(meta_sk);

	/* Update lambda based on current conditions */
	common_update_lambda(msk, default_subflow);

	/* Simple HoL prevention: check if slow subflow would block fast one */
	slow_inflight = best_tp->write_seq - best_tp->snd_una;
	fast_capacity = tcp_sk(fastest_ssk)->snd_cwnd * tcp_sk(fastest_ssk)->mss_cache;
	meta_wnd = tcp_wnd_end(meta_tp) - meta_tp->write_seq;

	/* If using slow subflow would prevent fast subflow from sending efficiently */
	if (slow_inflight * blest_global.lambda_1000 > fast_capacity * 1000 && meta_wnd < fast_capacity) {
		/* HoL blocking risk - use fastest subflow */
		mptcp_subflow_set_scheduled(minrtt_subflow, 1);
		blest_global.hol_prevented++;
		pr_debug("BLEST: HoL prevention triggered - using fastest subflow\n");
		return 0;
	}

	/* Safe to use slower subflow */
	mptcp_subflow_set_scheduled(default_subflow, 1);
	pr_debug("BLEST: using subflow safely (no HoL blocking risk)\n");
	
	return 0;
}

static struct mptcp_sched_ops mptcp_sched_blest = {
	.init		= mptcp_sched_blest_init,
	.release	= mptcp_sched_blest_release,
	.get_subflow	= mptcp_sched_blest_get_subflow,
	.name		= "blest",
	.owner		= THIS_MODULE,
};

static int __init mptcp_blest_register(void)
{
	if (mptcp_register_scheduler(&mptcp_sched_blest) < 0) {
		pr_err("BLEST scheduler registration failed\n");
		return -1;
	}

	// pr_info("BLEST scheduler with HoL prevention registered successfully\n");
	return 0;
}

static void __exit mptcp_blest_unregister(void)
{
	mptcp_unregister_scheduler(&mptcp_sched_blest);
}

module_init(mptcp_blest_register);
module_exit(mptcp_blest_unregister);

MODULE_AUTHOR("Simone Ferlin, Tiago Sousa");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("BLEST scheduler with simplified HoL-blocking prevention");
MODULE_VERSION("1.0");
