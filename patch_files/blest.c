// SPDX-License-Identifier: GPL-2.0
/*	MPTCP BLEST Scheduler - Based on original BLEST algorithm
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
 *	Kernel 6.6 Implementation:
 *	Tiago Sousa
 */

#include "blest.h"

/* BLEST global state */
static BlestConnData *blest_global;

static void mptcp_sched_blest_init(struct mptcp_sock *msk)
{
	blest_global = kzalloc(sizeof(BlestConnData), GFP_KERNEL);
	blest_global->lambda_1000 = lambda * 100;
	blest_global->total_decisions = 0;
	blest_global->hol_prevented = 0;
	blest_global->min_srtt_us = U32_MAX; // Inicializa min RTT
	blest_global->max_srtt_us = 0;		 // Inicializa max RTT
	pr_info("BLEST scheduler initialized (lambda=%d.%d)\n",
			lambda / 10, lambda % 10);
}

static void mptcp_sched_blest_release(struct mptcp_sock *msk)
{
	pr_info("BLEST: %u decisions, %u HoL prevented\n",
			blest_global->total_decisions, blest_global->hol_prevented);
	kfree(blest_global);
}

static int mptcp_sched_blest_get_subflow(struct mptcp_sock *msk,
										 struct mptcp_sched_data *data)
{
	struct mptcp_subflow_context *default_subflow, *minrtt_subflow, *subflow;
	struct sock *best_ssk, *fastest_ssk, *meta_sk = (struct sock *)msk;
	struct tcp_sock *best_tp, *meta_tp;

	if (!blest_global)
		return -EAGAIN;

	blest_global->total_decisions++;

	// struct sk_buff *skb = skb_peek(&meta_sk->sk_receive_queue);

	// /* Answer data_fin on same subflow!!! */
	// if(meta_sk->sk_shutdown & RCV_SHUTDOWN && skb && (skb->len == 0)) {
	mptcp_for_each_subflow(msk, subflow) {
		break;
	}

	minrtt_subflow = find_min_rtt_subflow(msk, blest_global);
	if (!minrtt_subflow)
		return -EAGAIN;

	default_subflow = __mptcp_sched_minrtt_get_subflow(msk, data);
	if (!default_subflow)
		return -EAGAIN;

	best_ssk = mptcp_subflow_tcp_sock(default_subflow);
	fastest_ssk = mptcp_subflow_tcp_sock(minrtt_subflow);

	if (!best_ssk || !fastest_ssk)
		return -EAGAIN;

	if (best_ssk != fastest_ssk)
	{
		u32 slow_linger_time, fast_bytes, slow_inflight_bytes, slow_bytes, avail_space;
		u32 buffered_bytes = 0;

		meta_tp = tcp_sk(meta_sk);
		best_tp = tcp_sk(best_ssk);

		if (!meta_tp || !best_tp)
			return -EAGAIN;

		blest_update_lambda(meta_sk, best_ssk, blest_global);

		slow_linger_time = blest_estimate_linger_time(best_ssk, blest_global);
		fast_bytes = blest_estimate_bytes(fastest_ssk, slow_linger_time, blest_global);

		slow_inflight_bytes = best_tp->write_seq - best_tp->snd_una;
		slow_bytes = buffered_bytes + slow_inflight_bytes;

		avail_space = (slow_bytes < meta_tp->snd_wnd) ? (meta_tp->snd_wnd - slow_bytes) : 0;

		if (fast_bytes > avail_space)
		{
			blest_global->hol_prevented++;
			pr_info("BLEST: Blocked HoL (fast_bytes=%u, avail_space=%u)\n", fast_bytes, avail_space);
			return -EAGAIN;
		}
	}

	mptcp_subflow_set_scheduled(default_subflow, 1);
	return 0;
}

static struct mptcp_sched_ops mptcp_sched_blest = {
	.init = mptcp_sched_blest_init,
	.release = mptcp_sched_blest_release,
	.get_subflow = mptcp_sched_blest_get_subflow,
	.name = "blest",
	.owner = THIS_MODULE,
};

static int __init mptcp_blest_register(void)
{
	if (mptcp_register_scheduler(&mptcp_sched_blest) < 0)
	{
		pr_err("BLEST scheduler registration failed\n");
		return -1;
	}
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
MODULE_DESCRIPTION("BLEST scheduler with HoL-blocking prevention");
MODULE_VERSION("1.0");