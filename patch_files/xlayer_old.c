/**
 * @file mptcp_ms.c
 * @brief MPTCP scheduler for MS (Multi-Stream) based on BLEST
 * @details This scheduler is designed to optimize the performance of MPTCP
 *          by selecting the best subflow based on the current
 *          network conditions.
 *          This implementation was tested with the MPTCP module in a Raspberry
 *          Pi 4, running a modified Kernel 5.4.83 to support the Arch
 *          Architecture for the Raspberry Pi.
 *          If you want to use the same Kernel environment, check the
 *          repository on GitHub: https://github.com/tiagojoseas/raspberry-mptcp
 * @author Tiago Sousa <tiagojoseas@gmail.com> <tiago.sousa@dtx-colab.pt>
 * @date 2025-05-14
 * @version 1.0
 */

#include <linux/module.h>
#include <net/mptcp.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/kthread.h>
#include <linux/delay.h>
#include <linux/string.h>
#include <linux/netdevice.h> // Required for dev_get_by_index
#include <linux/inet.h>      // For IP address formatting functions
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <net/cfg80211.h>
#include <linux/init.h>
#include <linux/proc_fs.h>
#include "mptcp_ms_lib.h"

struct sock *ms_get_available_subflow(struct sock *meta_sk, struct sk_buff *skb,
                                      bool zero_wnd_test)
{
    // int i = 0;
    struct mptcp_cb *mpcb = tcp_sk(meta_sk)->mpcb;
    struct sock *currentsk = NULL, *minsk = NULL, *blestsk = NULL, *mssk = NULL, *defaultsk = NULL;
    struct tcp_sock *auxttp;
    struct mptcp_tcp_sock *mptcp;
    struct ms_sched_priv *ms_p;
    struct tcp_sock *meta_tp, *besttp;
    u32 min_srtt = U32_MAX;

    /***********************************************************
     *  MY IMPLEMENTATION  *
     ***********************************************************/
    char aux_str[16] = {0}; // Buffer for IP
    // long wifi_rtt, nr_rtt = 0;
    long wifi_ratio, nr_ratio = 0, aux_ratio = 0;
    // bool wifi_metrics_valid, nr_metrics_valid = false;
    struct sock *sock_wifi = NULL, *sock_5g = NULL;
    // char log_msg[100] = {0};
    /**********************************************************/

    /* Answer data_fin on same subflow!!! */
    if (meta_sk->sk_shutdown & RCV_SHUTDOWN &&
        skb && mptcp_is_data_fin(skb))
    {
        mptcp_for_each_sub(mpcb, mptcp)
        {
            currentsk = mptcp_to_sock(mptcp);

            if (tcp_sk(currentsk)->mptcp->path_index == mpcb->dfin_path_index &&
                mptcp_is_available(currentsk, skb, zero_wnd_test))
                return currentsk;
        }
    }

    auxttp = tcp_sk(meta_sk);

    // print rtt of the meta_sk
    // pr_info("MS_SCHED v0.1: Meta sk RTT: %d\n", auxttp->srtt_us);

    /* First, find the overall best subflow */
    mptcp_for_each_sub(mpcb, mptcp)
    {
        currentsk = mptcp_to_sock(mptcp);
        auxttp = tcp_sk(currentsk);
        ms_p = ms_sched_get_priv(auxttp);

        /* Set of states for which we are allowed to send data */
        if (!mptcp_sk_can_send(currentsk))
            continue;

        /* We do not send data on this subflow unless it is
         * fully established, i.e. the 4th ack has been received.
         */
        if (auxttp->mptcp->pre_established)
            continue;

        ms_p->min_srtt_us = min(ms_p->min_srtt_us, auxttp->srtt_us);
        ms_p->max_srtt_us = max(ms_p->max_srtt_us, auxttp->srtt_us);

        if (auxttp->srtt_us < min_srtt)
        {
            min_srtt = auxttp->srtt_us;
            minsk = currentsk;
        }

        /***********************************************************
         *  MY IMPLEMENTATION  *
         ***********************************************************/
        /* get ip addr */
        get_ip_address(currentsk, aux_str);

        // Check if this IP belongs to wlan0 interface (WiFi)
        if (is_ip_of_wifi(aux_str))
        {
            // pr_info("MS_SCHED v0.1: WiFi subflow found\n");
            sock_wifi = currentsk;
        }
        else
        {
            // pr_info("MS_SCHED v0.1: 5G subflow found\n");
            sock_5g = currentsk;
        }

        /**********************************************************/
    }

    // /***********************************************************
    //  *  MY IMPLEMENTATION  *
    //  ***********************************************************/

    spin_lock(&net_info.lock);
    wifi_ratio = net_info.wifi_ratio;
    nr_ratio = net_info.nr_ratio;
    spin_unlock(&net_info.lock);

    // printk(KERN_INFO "MS SCHED: Wi-Fi: %d   5g: %d\n", wifi_ratio, nr_ratio);

    if (sock_wifi && sock_5g)
    {
        if (wifi_ratio > nr_ratio)
        {
            // pr_info("MS_SCHED v0.1: WiFi has better ratio\n");
            mssk = sock_wifi;
        }
        else
        {
            // pr_info("MS_SCHED v0.1: 5G has better ratio\n");
            mssk = sock_5g;
        }
    }
    else if (sock_wifi)
    {
        // pr_info("MS_SCHED v0.1: Only WiFi subflow available\n");
        mssk = sock_wifi;
    }
    else if (sock_5g)
    {
        // pr_info("MS_SCHED v0.1: Only 5G subflow available\n");
        mssk = sock_5g;
    }
    else
    {
        mssk = NULL;
        // pr_info("MS_SCHED v0.1: No subflow available\n");
    }

    /***********************************************************/
    // write id of metrics envolved to log file
    defaultsk = get_available_subflow(meta_sk, skb, zero_wnd_test);
    blestsk = defaultsk;

    if (!mssk)
    {
        mssk = minsk;
    }

    /* if we decided to use a slower flow, we have the option of not using it at all */
    else if (blestsk && mssk && blestsk != mssk)
    {
        u32 slow_linger_time, fast_bytes, slow_inflight_bytes, slow_bytes, avail_space;
        u32 buffered_bytes = 0;
        meta_tp = tcp_sk(meta_sk);
        besttp = tcp_sk(blestsk);

        ms_sched_update_lambda(meta_sk, blestsk);

        slow_linger_time = ms_sched_estimate_linger_time(blestsk);
        fast_bytes = ms_sched_estimate_bytes(mssk, slow_linger_time);

        if (skb)
            buffered_bytes = skb->len;

        slow_inflight_bytes = besttp->write_seq - besttp->snd_una;
        slow_bytes = buffered_bytes + slow_inflight_bytes; // bytes of this SKB plus those in flight already

        avail_space = (slow_bytes < meta_tp->snd_wnd) ? (meta_tp->snd_wnd - slow_bytes) : 0;

        if (fast_bytes > avail_space)
        {
            blestsk = NULL;
        }
    }

    return blestsk;
}

static void ms_sched_init(struct sock *sk)
{
    struct ms_sched_priv *ms_p = ms_sched_get_priv(tcp_sk(sk));
    struct ms_sched_cb *ms_cb = ms_sched_get_cb(tcp_sk(mptcp_meta_sk(sk)));

    ms_p->last_rbuf_opti = tcp_jiffies32;
    ms_p->min_srtt_us = U32_MAX;
    ms_p->max_srtt_us = 0;

    if (!ms_cb->lambda_1000)
    {
        ms_cb->lambda_1000 = lambda * 100;
        ms_cb->last_lambda_update = tcp_jiffies32;
    }
}

static struct mptcp_sched_ops mptcp_sched_ms = {
    .get_subflow = ms_get_available_subflow,
    .next_segment = mptcp_next_segment,
    .init = ms_sched_init,
    .name = "ms",
    .owner = THIS_MODULE,
};

static int __init ms_register(void)
{
    proc_create(PROC_NAME, 0666, NULL, &proc_file_ops);
    printk(KERN_INFO "/proc/%s created\n", PROC_NAME);

    BUILD_BUG_ON(sizeof(struct ms_sched_priv) > MPTCP_SCHED_SIZE);
    BUILD_BUG_ON(sizeof(struct ms_sched_cb) > MPTCP_SCHED_DATA_SIZE);

    net_metrics_thread_running = true;
    net_metrics_thread = kthread_run(net_metrics_updater_thread, NULL, "net_metrics_updater");
    if (IS_ERR(net_metrics_thread))
    {
        // pr_err("Failed to start net_metrics_updater thread\n");
        return PTR_ERR(net_metrics_thread);
    }

    pr_info("MS_SCHED v0.1 SCHEDULER: Registering");

    if (mptcp_register_scheduler(&mptcp_sched_ms))
    {
        // pr_err("MS_SCHED v0.1 SCHEDULER: Registration failed\n");
        return -1;
    }

    return 0;
}

static void ms_unregister(void)
{
    pr_info("MS_SCHED v0.1 SCHEDULER: Unregistering");

    if (net_metrics_thread_running)
    {
        kthread_stop(net_metrics_thread);
        net_metrics_thread_running = false;
    }

    remove_proc_entry(PROC_NAME, NULL);
    printk(KERN_INFO "/proc/%s removed\n", PROC_NAME);

    mptcp_unregister_scheduler(&mptcp_sched_ms);
}

module_init(ms_register);
module_exit(ms_unregister);

MODULE_AUTHOR("Tiago Sousa");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("ms scheduler for MPTCP, based on BLEST");
MODULE_VERSION("0.95");