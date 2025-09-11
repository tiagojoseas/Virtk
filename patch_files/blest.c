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

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/tcp.h>
#include <net/tcp.h>
#include <net/mptcp.h>
#include "protocol.h"
#include "common_lib.h"

/* BLEST global state */
static struct {
	s16 lambda_1000;
	u32 total_decisions;
	u32 hol_prevented;
	u32 last_lambda_update;
} blest_global = {
	.lambda_1000 = 1200, /* default 1.2 */
	.total_decisions = 0,
	.hol_prevented = 0,
	.last_lambda_update = 0
};

static void blest_update_lambda(struct sock *meta_sk, struct sock *sk)
{
	struct tcp_sock *meta_tp = tcp_sk(meta_sk);
	struct tcp_sock *tp = tcp_sk(sk);
	u32 min_srtt_us = tp->srtt_us;

	if (tcp_jiffies32 - blest_global.last_lambda_update < usecs_to_jiffies(min_srtt_us >> 3))
		return;

	if (meta_tp->retrans_stamp) {
		blest_global.lambda_1000 += dyn_lambda_bad;
	} else {
		blest_global.lambda_1000 -= dyn_lambda_good;
	}

	/* cap lambda_1000 to its value range */
	if (blest_global.lambda_1000 > (max_lambda * 100))
		blest_global.lambda_1000 = max_lambda * 100;
	if (blest_global.lambda_1000 < (min_lambda * 100))
		blest_global.lambda_1000 = min_lambda * 100;

	blest_global.last_lambda_update = tcp_jiffies32;
}

static u32 blest_estimate_bytes(struct sock *sk, u32 time_us)
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

	return div_u64(((u64)packets) * tp->mss_cache * blest_global.lambda_1000, 1000);
}

static u32 blest_estimate_linger_time(struct sock *sk)
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

static void mptcp_sched_blest_init(struct mptcp_sock *msk)
{
	blest_global.lambda_1000 = lambda * 100;
	blest_global.total_decisions = 0;
	blest_global.hol_prevented = 0;
	pr_info("BLEST scheduler initialized (lambda=%d.%d)\n", 
		lambda / 10, lambda % 10);
}

static void mptcp_sched_blest_release(struct mptcp_sock *msk)
{
	pr_info("BLEST: %u decisions, %u HoL prevented\n", 
		blest_global.total_decisions, blest_global.hol_prevented);
}

static int mptcp_sched_blest_get_subflow(struct mptcp_sock *msk,
					 struct mptcp_sched_data *data)
{
	struct mptcp_subflow_context *default_subflow, *minrtt_subflow;
	struct sock *best_ssk, *fastest_ssk, *meta_sk;
	struct tcp_sock *best_tp, *meta_tp;

	blest_global.total_decisions++;

	minrtt_subflow = find_min_rtt_subflow(msk);
	if (!minrtt_subflow) {
		return -EINVAL;
	}

	default_subflow = __mptcp_sched_minrtt_get_subflow(msk, data);
	if (!default_subflow) {
		default_subflow = minrtt_subflow;
	}

	best_ssk = mptcp_subflow_tcp_sock(default_subflow);
	fastest_ssk = mptcp_subflow_tcp_sock(minrtt_subflow);
	meta_sk = (struct sock *)msk;

	if (best_ssk && fastest_ssk && best_ssk != fastest_ssk) {
		u32 slow_linger_time, fast_bytes, slow_inflight_bytes, slow_bytes, avail_space;
		u32 buffered_bytes = 0;

		meta_tp = tcp_sk(meta_sk);
		best_tp = tcp_sk(best_ssk);

		blest_update_lambda(meta_sk, best_ssk);

		slow_linger_time = blest_estimate_linger_time(best_ssk);
		fast_bytes = blest_estimate_bytes(fastest_ssk, slow_linger_time);

		slow_inflight_bytes = best_tp->write_seq - best_tp->snd_una;
		slow_bytes = buffered_bytes + slow_inflight_bytes;

		avail_space = (slow_bytes < meta_tp->snd_wnd) ? 
			(meta_tp->snd_wnd - slow_bytes) : 0;

		if (fast_bytes > avail_space) {
			blest_global.hol_prevented++;
			return -EAGAIN;
		}
	}

	mptcp_subflow_set_scheduled(default_subflow, 1);
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