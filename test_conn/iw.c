#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/netdevice.h>
#include <linux/inetdevice.h>
#include <net/cfg80211.h>


static int get_wifi_info(char *interface_name)
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
        pr_err("Não foi possível encontrar a interface Wi-Fi\n");
        return -ENODEV;
    }
    
    // Obtém o dispositivo Wi-Fi
    wdev = dev->ieee80211_ptr;
    if (!wdev || !wdev->netdev)
    {
        dev_put(dev);
        pr_err("Dispositivo Wi-Fi não encontrado\n");
        return -ENODEV;
    }

    // Obtém o wiphy (estrutura que representa a interface Wi-Fi)
    wiphy = wdev->wiphy;
    if (!wiphy)
    {
        dev_put(dev);
        pr_err("Wiphy não encontrado\n");
        return -ENODEV;
    }

    // u8 address

    // Obtém informações do BSS
    bss = cfg80211_get_bss(wiphy, NULL, NULL, NULL, 0, 0, 0);
    if (!bss)
    {
        pr_err("Não foi possível obter o BSS\n");
        return -ENODATA;
    }

    sinfo = kzalloc(sizeof(*sinfo), GFP_KERNEL);
    if (!sinfo)
    {
        pr_err("Não foi possível alocar memória para sinfo\n");
        return -ENOMEM;
    }

    ret = cfg80211_get_station(dev, bss->bssid, sinfo);
    if (ret)
    {
        pr_err("Não foi possível obter informações da estação\n");
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

static int __init wifi_info_module_init(void)
{
    // imprimir nomes das interfaces disponíveis no sistema
    
    /**
    struct net_device *dev;
    struct list_head *pos;
    pr_info("Interfaces disponíveis:\n");
    list_for_each(pos, &init_net.dev_base_head)
    {
        dev = list_entry(pos, struct net_device, dev_list);
        pr_info("Interface %s with tx_rate %d\n", dev->name, get_wifi_info(dev->name));
    }
    */

    get_wifi_info("wlp0s20f3");
      
    

    return 0;
}

static void __exit wifi_info_module_exit(void)
{
    pr_info("Wifi Info Module Unloaded\n");
}

module_init(wifi_info_module_init);
module_exit(wifi_info_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Tiago Sousa");
MODULE_DESCRIPTION("Módulo para obter informações de Wi-Fi no kernel");