// SPDX-License-Identifier: GPL-2.0
/* MPTCP Min RTT Scheduler - Original Algorithm Implementation
 *
 * This scheduler selects the subflow with the minimum RTT (Round Trip Time)
 * following the original MinRTT algorithm approach used in MPTCP v0.x
 * 
 * The original algorithm is simple:
 * 1. Find all available active subflows
 * 2. Select the one with minimum RTT (srtt_us)
 * 3. Fall back to backup subflows if no active ones available
 *
 * Author: Generated for MPTCP kernel implementation
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/tcp.h>
#include <net/tcp.h>
#include <net/mptcp.h>
#include "protocol.h"
#include "utils.h"

static void mptcp_sched_minrtt_init(struct mptcp_sock *msk)
{
	pr_info("MinRTT scheduler initialized\n");
}

static void mptcp_sched_minrtt_release(struct mptcp_sock *msk)
{
	pr_info("MinRTT scheduler released\n");
}

/* Original MinRTT algorithm: select subflow with minimum RTT */
static int mptcp_sched_minrtt_get_subflow(struct mptcp_sock *msk,
					  struct mptcp_sched_data *data)
{
	struct mptcp_subflow_context *best_subflow;
	
	/* Use the shared minRTT selection logic */
	best_subflow = __mptcp_sched_minrtt_get_subflow(msk, data);
	if (best_subflow) {
		mptcp_subflow_set_scheduled(best_subflow, 1);
		return 0;
	}
	
	/* Try fallback logic as last resort */
	best_subflow = mptcp_select_fallback_subflow(msk);
	if (best_subflow) {
		mptcp_subflow_set_scheduled(best_subflow, 1);
		return 0;
	}
	
	/* No suitable subflow found */
	pr_info("MinRTT: No suitable subflow found\n");
	return -EINVAL;
}

static struct mptcp_sched_ops mptcp_sched_minrtt = {
	.init		= mptcp_sched_minrtt_init,
	.release	= mptcp_sched_minrtt_release,
	.get_subflow	= mptcp_sched_minrtt_get_subflow,
	.name		= "minrtt",
	.owner		= THIS_MODULE,
};

static int __init mptcp_minrtt_register(void)
{
	if (mptcp_register_scheduler(&mptcp_sched_minrtt) < 0)
		return -1;

	return 0;
}

static void __exit mptcp_minrtt_unregister(void)
{
	mptcp_unregister_scheduler(&mptcp_sched_minrtt);
}

module_init(mptcp_minrtt_register);
module_exit(mptcp_minrtt_unregister);

MODULE_AUTHOR("Tiago Sousa");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MPTCP MinRTT scheduler - original algorithm");
MODULE_VERSION("1.0");
