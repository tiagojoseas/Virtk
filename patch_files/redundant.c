// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2022, SUSE. */

#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/list.h>
#include <linux/rculist.h>
#include <linux/spinlock.h>
#include "protocol.h"

static void mptcp_sched_red_init(struct mptcp_sock *msk)
{
    // Print a message when the redundant scheduler is initialized
    // pr_info("redundant scheduler initialized\n");
}

static void mptcp_sched_red_release(struct mptcp_sock *msk)
{
    // pr_info("redundant scheduler released\n");
}


static int mptcp_red_get_subflow(struct mptcp_sock *msk,
                                  struct mptcp_sched_data *data)
{
	struct mptcp_subflow_context *subflow;

    mptcp_for_each_subflow(msk, subflow) {
		mptcp_subflow_set_scheduled(subflow, true);
        
    }
	return 0;
}

struct mptcp_sched_ops red = {
	.init		= (void *)mptcp_sched_red_init,
	.release	= (void *)mptcp_sched_red_release,
	.get_subflow	= (void *)mptcp_red_get_subflow,
	.name		= "redundant",
};

static int __init mptcp_redundant_register(void)
{
    int ret = mptcp_register_scheduler(&red);
    if (!ret)
        printk(KERN_INFO "MPTCP redundant scheduler registered successfully\n");
    return ret;
}
module_init(mptcp_redundant_register);

static void __exit mptcp_redundant_unregister(void)
{
    mptcp_unregister_scheduler(&red);
}
module_exit(mptcp_redundant_unregister);