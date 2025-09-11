#include <net/mptcp.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <linux/inet.h> 
#include <net/cfg80211.h>

#define PROC_NAME "rm5xxq_proc"
#define BUFFER_SIZE 128

/*************************************
 * MPTCP Scheduler
 *************************************/

struct ms_sched_priv
{
    u32 last_rbuf_opti;
    u32 min_srtt_us;
    u32 max_srtt_us;
};

struct ms_sched_cb
{
    s16 lambda_1000; /* values range from min_lambda * 100 to max_lambda * 100 */
    u32 last_lambda_update;
};

static struct ms_sched_priv *ms_sched_get_priv(const struct tcp_sock *tp)
{
    return (struct ms_sched_priv *)&tp->mptcp->mptcp_sched[0];
}

static struct ms_sched_cb *ms_sched_get_cb(const struct tcp_sock *tp)
{
    return (struct ms_sched_cb *)&tp->mpcb->mptcp_sched[0];
}

static void get_ip_address(struct sock *sk, char *ip_str)
{
    if (sk->sk_family == AF_INET)
    { // IPv4
        snprintf(ip_str, INET6_ADDRSTRLEN, "%pI4", &sk->sk_rcv_saddr);
        // // pr_info("MS_SCHED v0.1: Subflow IPv4 address: %s", ip_str);
    }
    else
    {
        pr_warn("MS_SCHED v0.1: Unknown address family for subflow");
    }
}

/**
 * Check if an IP address belongs to the wlan0 interface
 * @param ip_str IP address string to check
 * @return true if IP belongs to wlan0, false otherwise
 */
static bool is_ip_of_wifi(const char *ip_str)
{
    struct net_device *dev;
    struct in_device *in_dev;
    struct in_ifaddr *ifa;
    __be32 target_ip;
    bool found = false;

    if (!ip_str)
        return false;

    target_ip = in_aton(ip_str);

    // Get the wlan0 device
    dev = dev_get_by_name(&init_net, "wlan0");
    if (!dev)
        return false;

    // Check if the device has the target IP
    rcu_read_lock();
    in_dev = __in_dev_get_rcu(dev);
    if (in_dev) {
        for (ifa = in_dev->ifa_list; ifa; ifa = ifa->ifa_next) {
            if (ifa->ifa_address == target_ip) {
                found = true;
                break;
            }
        }
    }
    rcu_read_unlock();

    dev_put(dev);
    return found;
}

/*************************************
 * Based on Blest
 *************************************/
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

static void ms_sched_update_lambda(struct sock *meta_sk, struct sock *sk)
{
    struct ms_sched_cb *ms_cb = ms_sched_get_cb(tcp_sk(meta_sk));
    struct ms_sched_priv *ms_p = ms_sched_get_priv(tcp_sk(sk));

    if (tcp_jiffies32 - ms_cb->last_lambda_update < usecs_to_jiffies(ms_p->min_srtt_us >> 3))
        return;

    /* if there have been retransmissions of packets of the slow flow
     * during the slow flows last RTT => increase lambda
     * otherwise decrease
     */
    if (tcp_sk(meta_sk)->retrans_stamp)
    {
        /* need to slow down on the slow flow */
        ms_cb->lambda_1000 += dyn_lambda_bad;
    }
    else
    {
        /* use the slow flow more */
        ms_cb->lambda_1000 -= dyn_lambda_good;
    }

    /* cap lambda_1000 to its value range */
    ms_cb->lambda_1000 = min_t(s16, ms_cb->lambda_1000, max_lambda * 100);
    ms_cb->lambda_1000 = max_t(s16, ms_cb->lambda_1000, min_lambda * 100);

    ms_cb->last_lambda_update = tcp_jiffies32;
}

/* how many bytes will sk send during the rtt of another, slower flow? */
static u32 ms_sched_estimate_bytes(struct sock *sk, u32 time_8)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct ms_sched_priv *ms_p = ms_sched_get_priv(tp);
    struct ms_sched_cb *ms_cb = ms_sched_get_cb(mptcp_meta_tp(tp));
    u32 avg_rtt, num_rtts, ca_cwnd, packets;

    avg_rtt = (ms_p->min_srtt_us + ms_p->max_srtt_us) / 2;
    if (avg_rtt == 0)
        num_rtts = 1; /* sanity */
    else
        num_rtts = (time_8 / avg_rtt) + 1; /* round up */

    /* during num_rtts, how many bytes will be sent on the flow?
     * assumes for simplification that Reno is applied as congestion-control
     */
    if (tp->snd_ssthresh == TCP_INFINITE_SSTHRESH)
    {
        /* we are in initial slow start */
        if (num_rtts > 16)
            num_rtts = 16;                              /* cap for sanity */
        packets = tp->snd_cwnd * ((1 << num_rtts) - 1); /* cwnd + 2*cwnd + 4*cwnd */
    }
    else
    {
        ca_cwnd = max(tp->snd_cwnd, tp->snd_ssthresh + 1); /* assume we jump to CA already */
        packets = (ca_cwnd + (num_rtts - 1) / 2) * num_rtts;
    }

    return div_u64(((u64)packets) * tp->mss_cache * ms_cb->lambda_1000, 1000);
}

static u32 ms_sched_estimate_linger_time(struct sock *sk)
{
    struct tcp_sock *tp = tcp_sk(sk);
    struct ms_sched_priv *ms_p = ms_sched_get_priv(tp);
    u32 estimate, slope, inflight, cwnd;

    inflight = tcp_packets_in_flight(tp) + 1; /* take into account the new one */
    cwnd = tp->snd_cwnd;

    if (inflight >= cwnd)
    {
        estimate = ms_p->max_srtt_us;
    }
    else
    {
        slope = ms_p->max_srtt_us - ms_p->min_srtt_us;
        if (cwnd == 0)
            cwnd = 1; /* sanity */
        estimate = ms_p->min_srtt_us + (slope * inflight) / cwnd;
    }

    return (tp->srtt_us > estimate) ? tp->srtt_us : estimate;
}

/**
 * Get Wi-Fi Data Rate
 */

static int get_wifi_info_by_ip(const char *ip_addr)
{
    struct net_device *dev = NULL, *found_dev = NULL;
    struct wireless_dev *wdev;
    struct wiphy *wiphy;
    struct in_device *in_dev;
    struct in_ifaddr *ifa;
    struct cfg80211_bss *bss = NULL;
    struct station_info *sinfo = NULL;
    struct rate_info *rate_tx, *rate_rx;
    __be32 target_ip;
    int ret = -ENODEV, bitrate = -1;

    if (!ip_addr)
        return -EINVAL;

    target_ip = in_aton(ip_addr);

    /* Procurar a interface com o IP especificado */
    rcu_read_lock();
    for_each_netdev_rcu(&init_net, dev) {
        in_dev = __in_dev_get_rcu(dev);
        if (!in_dev)
            continue;

        for (ifa = in_dev->ifa_list; ifa; ifa = ifa->ifa_next) {
            if (ifa->ifa_address == target_ip) {
                dev_hold(dev);  // proteger dev fora da RCU
                found_dev = dev;
                break;
            }
        }

        if (found_dev)
            break;
    }
    rcu_read_unlock();

    if (!found_dev) {
        // pr_err("WiFi info: Interface com IP %s não encontrada\n", ip_addr);
        return -ENODEV;
    }

    /* Verificar se é interface wireless */
    wdev = found_dev->ieee80211_ptr;
    if (!wdev || !wdev->netdev) {
        // pr_err("WiFi info: Interface %s não é wireless\n", found_dev->name);
        ret = -ENODEV;
        goto out;
    }

    wiphy = wdev->wiphy;
    if (!wiphy) {
        // pr_err("WiFi info: Wiphy não disponível\n");
        ret = -ENODEV;
        goto out;
    }

    /* Obter o BSS associado */
    bss = cfg80211_get_bss(wiphy, NULL, NULL, NULL, 0, 0, 0);
    if (!bss) {
        // pr_err("WiFi info: BSS não encontrado\n");
        ret = -ENODATA;
        goto out;
    }

    /* Alocar e obter informações da estação */
    sinfo = kzalloc(sizeof(*sinfo), GFP_KERNEL);
    if (!sinfo) {
        // pr_err("WiFi info: Falha ao alocar memória\n");
        ret = -ENOMEM;
        goto out_bss;
    }

    ret = cfg80211_get_station(found_dev, bss->bssid, sinfo);
    if (ret) {
        // pr_err("WiFi info: Falha ao obter dados da estação (%d)\n", ret);
        goto out_free;
    }

    /* Debug/logs */
    // pr_info("Ligado a %pM (interface %s)\n", bss->bssid, found_dev->name);
    // pr_info("  TX: %llu bytes (%u pkts), RX: %llu bytes (%u pkts), Signal: %d dBm\n",
    //         sinfo->tx_bytes, sinfo->tx_packets,
    //         sinfo->rx_bytes, sinfo->rx_packets,
    //         sinfo->signal);

    // rate_rx = &sinfo->rxrate;
    rate_tx = &sinfo->txrate;

    // bitrate = cfg80211_calculate_bitrate(rate_rx);
    // pr_info("  Bitrate RX: %d Mbps\n", bitrate);

    bitrate = cfg80211_calculate_bitrate(rate_tx);
    // pr_info("  Bitrate TX: %d Mbps\n", bitrate);

    // bit rate is 4333 but the real value is 433.3 MBit/s
    // bitrate to bits
    bitrate = bitrate * 100000;

out_free:
    kfree(sinfo);
out_bss:
    cfg80211_put_bss(wiphy, bss);
out:
    dev_put(found_dev);
    return bitrate;
}


static int get_wifi_info_by_name(char *interface_name)
{
    struct net_device *dev;
    struct wireless_dev *wdev;
    struct wiphy *wiphy;
    struct in_ifaddr *if_info;
    struct cfg80211_bss *bss;
    struct station_info *sinfo;
    struct rate_info *rate_tx, *rate_rx;
    int ret, bitrate;

    // Obtém a interface Wi-Fi. Altere 'wlp0s20f3' para o nome da interface correta.
    dev = dev_get_by_name(&init_net, interface_name);
    if (!dev)
    {
        // pr_err("Não foi possível encontrar a interface Wi-Fi\n");
        return -ENODEV;
    }
    
    // Obtém o dispositivo Wi-Fi
    wdev = dev->ieee80211_ptr;
    if (!wdev || !wdev->netdev)
    {
        dev_put(dev);
        // pr_err("Dispositivo Wi-Fi não encontrado\n");
        return -ENODEV;
    }

    // Obtém o wiphy (estrutura que representa a interface Wi-Fi)
    wiphy = wdev->wiphy;
    if (!wiphy)
    {
        dev_put(dev);
        // pr_err("Wiphy não encontrado\n");
        return -ENODEV;
    }

    // u8 address

    // Obtém informações do BSS
    bss = cfg80211_get_bss(wiphy, NULL, NULL, NULL, 0, 0, 0);
    if (!bss)
    {
        // // pr_err("Não foi possível obter o BSS\n");
        return -ENODATA;
    }

    sinfo = kzalloc(sizeof(*sinfo), GFP_KERNEL);
    if (!sinfo)
    {
        // pr_err("Não foi possível alocar memória para sinfo\n");
        return -ENOMEM;
    }

    ret = cfg80211_get_station(dev, bss->bssid, sinfo);
    if (ret)
    {
        // pr_err("Não foi possível obter informações da estação\n");
        kfree(sinfo);
        return ret;
    }

    pr_info("Connected to %pM (on %s)\n", bss->bssid, wdev->netdev->name);
        
    // roughly
    rcu_read_lock();
    if_info = rcu_dereference(dev->ip_ptr->ifa_list);
    printk("\t\t inet address: %pI4, mask: %pI4\n", &if_info->ifa_address, &if_info->ifa_mask);
    rcu_read_unlock();
    
    // Frequência (MHz)
    pr_info("\t\t freq: %d MHz\n", bss->channel->center_freq);
    
    // RX: Bytes e pacotes
    pr_info("\t\t rx: %llu bytes (%u pacotes)\n", sinfo->rx_bytes, sinfo->rx_packets);
    
    // TX: Bytes e pacotes
    pr_info("\t\t tx: %llu bytes (%u pacotes)\n", sinfo->tx_bytes, sinfo->tx_packets);
    
    // Sinal de recepção
    pr_info("\t\t signal: %d dBm\n", sinfo->signal);

    rate_tx = &sinfo->txrate;
    rate_rx = &sinfo->rxrate;

    // Taxa de recepção
    bitrate = cfg80211_calculate_bitrate(rate_rx);
    pr_info("\t\t rx bitrate: %d MBit/s\n", bitrate);

    // Taxa de transmissão
    bitrate = cfg80211_calculate_bitrate(rate_tx);
    pr_info("\t\t tx bitrate: %d MBit/s\n", bitrate);

    // Liberar recursos
    kfree(sinfo);
    cfg80211_put_bss(wiphy, bss);
    dev_put(dev);

    return bitrate;
}



/**
 * Get 5G-NR Data Rate
*/

static char proc_buffer[BUFFER_SIZE];
static ssize_t proc_len = 0;
static int bitrate_5g = 0;


static int get_5g_info(void)
{
    // this value is in bits
    
    return bitrate_5g;
}

ssize_t proc_read(struct file *file, char __user *user_buf, size_t count, loff_t *pos) {
    if (*pos > 0 || proc_len == 0)
        return 0;
    if (copy_to_user(user_buf, proc_buffer, proc_len))
        return -EFAULT;
    *pos = proc_len;
    return proc_len;
}

ssize_t proc_write(struct file *file, const char __user *user_buf, size_t count, loff_t *pos) {
    proc_len = count > BUFFER_SIZE ? BUFFER_SIZE : count;
    if (copy_from_user(proc_buffer, user_buf, proc_len))
        return -EFAULT;

    // Garante que a string termina em '\0'
    if (proc_len < BUFFER_SIZE)
        proc_buffer[proc_len] = '\0';
    else
        proc_buffer[BUFFER_SIZE - 1] = '\0';
     
    bitrate_5g = 0;
    if (sscanf(proc_buffer, "%d", &bitrate_5g) != 1)
    {
        printk(KERN_ERR "MS SCHED: bitrate_5g value: Invalid input\n");
        return -EINVAL;
    }
    // print value bitrate_5g
    printk(KERN_INFO "MS SCHED: bitrate_5g value: %s %d\n",proc_buffer, bitrate_5g);

    return proc_len;
}

static struct file_operations proc_file_ops = {
    .owner = THIS_MODULE,
    .read = proc_read,
    .write = proc_write,
};


// Parte global
static struct task_struct *net_metrics_thread;
static bool net_metrics_thread_running = false;

struct net_metrics {
    long wifi_ratio;
    long nr_ratio;
    spinlock_t lock;
};

static struct net_metrics net_info = {
    .wifi_ratio = -1,
    .nr_ratio = -1,
    .lock = __SPIN_LOCK_UNLOCKED(net_info.lock),
};

static int net_metrics_updater_thread(void *data)
{
    while (!kthread_should_stop()) {
        //long wifi = get_wifi_info_by_ip("192.168.1.100");
        long wifi = get_wifi_info_by_name("wlan0");
        long nr = get_5g_info(); // Substitui com a função correta

        spin_lock(&net_info.lock);
        net_info.wifi_ratio = wifi;
        net_info.nr_ratio = nr;
        spin_unlock(&net_info.lock);

        ssleep(1); // Espera 1 segundo antes de repetir
    }

    return 0;
}