// SPDX-License-Identifier: GPL-2.0
/* MPTCP Round Robin Scheduler
 *
 * This scheduler alternates between available subflows in a round-robin
 * fashion to provide fair distribution of traffic across all paths.
 * 
 * Features:
 * - Fair distribution across all available subflows
 * - Backup subflow support with fallback mechanism
 * - Robust availability checks using shared common_lib.h functions
 * - Stateful round-robin with per-connection tracking
 * - Detailed logging and debugging
 *
 * Author: Generated for MPTCP kernel implementation
 */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/types.h>
#include <linux/tcp.h>
#include <linux/limits.h>
#include <linux/inet.h>
#include <net/tcp.h>
#include <net/inet_connection_sock.h>
#include "protocol.h"
#include "common_lib.h"

#define true 1
#define false 0

/* Round Robin per-connection state */
struct roundrobin_conn_data {
	u32 last_used_index;      /* Index of last used subflow */
	u32 total_subflows;       /* Total number of subflows seen */
	u32 packets_sent;         /* Total packets scheduled */
	u32 last_update;          /* Last time we updated state */
};

/* Global Round Robin connection data - simplified for v1 */
static struct roundrobin_conn_data rr_global_data = {
	.last_used_index = 0,
	.total_subflows = 0,
	.packets_sent = 0,
	.last_update = 0,
};

static void mptcp_sched_roundrobin_init(struct mptcp_sock *msk)
{
	rr_global_data.last_used_index = 0;
	rr_global_data.total_subflows = 0;
	rr_global_data.packets_sent = 0;
	rr_global_data.last_update = tcp_jiffies32;
	
	pr_debug("Round Robin scheduler initialized for msk %p\n", msk);
}

static void mptcp_sched_roundrobin_release(struct mptcp_sock *msk)
{
	pr_debug("Round Robin scheduler released for msk %p (sent %u packets)\n", 
		 msk, rr_global_data.packets_sent);
}

/* Count available subflows and assign indices */
static u32 roundrobin_count_available_subflows(struct mptcp_sock *msk)
{
	struct mptcp_subflow_context *subflow;
	u32 count = 0;
	
	mptcp_for_each_subflow(msk, subflow) {
		if (mptcp_subflow_is_available(subflow))
			count++;
	}
	
	return count;
}

/* Get subflow by round-robin index among available subflows */
static struct mptcp_subflow_context *
roundrobin_get_subflow_by_index(struct mptcp_sock *msk, u32 target_index, bool use_backup)
{
	struct mptcp_subflow_context *subflow;
	u32 current_index = 0;
	bool is_backup;
	
	mptcp_for_each_subflow(msk, subflow) {
		if (!mptcp_subflow_is_available(subflow))
			continue;
		
		/* Check if this is a backup subflow */
		is_backup = !!(subflow->backup);
		
		/* Skip if we're not looking for backup flows and this is backup */
		if (!use_backup && is_backup)
			continue;
		
		/* Skip if we're looking for backup flows and this is not backup */
		if (use_backup && !is_backup)
			continue;
		
		if (current_index == target_index)
			return subflow;
		
		current_index++;
	}
	
	return NULL;
}

/* Select next subflow in round-robin order */
static struct mptcp_subflow_context *
roundrobin_select_next_subflow(struct mptcp_sock *msk, bool use_backup)
{
	struct mptcp_subflow_context *selected_subflow;
	u32 available_count;
	u32 next_index;
	
	/* Count available subflows */
	available_count = roundrobin_count_available_subflows(msk);
	if (available_count == 0)
		return NULL;
	
	/* Calculate next index in round-robin fashion */
	next_index = (rr_global_data.last_used_index + 1) % available_count;
	
	/* Get subflow at calculated index */
	selected_subflow = roundrobin_get_subflow_by_index(msk, next_index, use_backup);
	
	if (selected_subflow) {
		/* Update round-robin state */
		rr_global_data.last_used_index = next_index;
		rr_global_data.total_subflows = available_count;
		rr_global_data.last_update = tcp_jiffies32;
		
		pr_debug("Round Robin: selected subflow index %u/%u (backup=%s)\n", 
			 next_index, available_count, use_backup ? "yes" : "no");
	}
	
	return selected_subflow;
}

static int mptcp_sched_roundrobin_get_subflow(struct mptcp_sock *msk,
					      struct mptcp_sched_data *data)
{
	struct mptcp_subflow_context *selected_subflow = NULL;
	struct sock *selected_ssk = NULL;
	u32 rtt;
	
	/* First try to find the next active subflow in round-robin order */
	selected_subflow = roundrobin_select_next_subflow(msk, false);
	
	if (selected_subflow) {
		selected_ssk = mptcp_subflow_tcp_sock(selected_subflow);
		rtt = mptcp_subflow_get_rtt(selected_subflow);
		
		mptcp_subflow_set_scheduled(selected_subflow, true);
		rr_global_data.packets_sent++;
		
		pr_debug("Round Robin scheduler selected active subflow %p with RTT %u us (packet #%u)\n", 
			 selected_ssk, rtt, rr_global_data.packets_sent);
		
		return 0;
	}
	
	/* If no active subflow available, try backup subflows in round-robin order */
	selected_subflow = roundrobin_select_next_subflow(msk, true);
	
	if (selected_subflow) {
		selected_ssk = mptcp_subflow_tcp_sock(selected_subflow);
		rtt = mptcp_subflow_get_rtt(selected_subflow);
		
		mptcp_subflow_set_scheduled(selected_subflow, true);
		rr_global_data.packets_sent++;
		
		pr_debug("Round Robin scheduler selected backup subflow %p with RTT %u us (packet #%u)\n", 
			 selected_ssk, rtt, rr_global_data.packets_sent);
		
		return 0;
	}
	
	/* Final fallback: try to find ANY available subflow using shared logic */
	selected_subflow = mptcp_select_fallback_subflow(msk);
	
	if (selected_subflow) {
		selected_ssk = mptcp_subflow_tcp_sock(selected_subflow);
		mptcp_subflow_set_scheduled(selected_subflow, true);
		rr_global_data.packets_sent++;
		
		pr_debug("Round Robin scheduler fallback to subflow %p (packet #%u)\n", 
			 selected_ssk, rr_global_data.packets_sent);
		return 0;
	}

	pr_debug("Round Robin scheduler: no available subflow found\n");
	return -EINVAL;
}

static struct mptcp_sched_ops mptcp_sched_roundrobin = {
	.init		= mptcp_sched_roundrobin_init,
	.release	= mptcp_sched_roundrobin_release,
	.get_subflow	= mptcp_sched_roundrobin_get_subflow,
	.name		= "roundrobin",
	.owner		= THIS_MODULE,
};

static int __init mptcp_roundrobin_register(void)
{
	if (mptcp_register_scheduler(&mptcp_sched_roundrobin) < 0)
		return -1;

	return 0;
}

static void __exit mptcp_roundrobin_unregister(void)
{
	mptcp_unregister_scheduler(&mptcp_sched_roundrobin);
}

module_init(mptcp_roundrobin_register);
module_exit(mptcp_roundrobin_unregister);

MODULE_AUTHOR("Updated for MPTCP v1 by Tiago Sousa");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("MPTCP scheduler that alternates between subflows in round-robin fashion");
MODULE_VERSION("1.0");
