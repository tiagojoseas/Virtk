# VirtK

This is a project that aims to help developers to Virtualize a Kernel (*VirtK*) in a very simple way. 

```
sh script.sh 
```

Mounting script (copy to inside the VM):

```sh
#!/bin/bash

mkdir -p hostshare
mount -t 9p -o trans=virtio,version=9p2000.L hostshare hostshare

# Change the limits of the MPTCP
ip mptcp limits set add_addr_accepted 8 subflows 8

# Flush endpoints
ip mptcp endpoint flush # flush all endpoints

# List of the interface names
interfaces=( "enp0s4" "enp0s5" )

# Add ifaces as endpoints of mptcp
for iface in "${interfaces[@]}"; do
    ip_addr=$(ip -f inet addr show "$iface" | awk '/inet / {print $2}' | cut -d'/' -f1)
    if [[ -n "$ip_addr" ]]; then
        echo "Adicionar $ip_addr da interface: $iface"
        ip mptcp endpoint add "$ip_addr" dev "$iface" signal
    else
        echo "Nenhum IP encontrado para $iface"
    fi
done

```



