#!/bin/bash

# Network setup script for VM interfaces
# This script brings up network interfaces created by QEMU bridges

LOG_FILE="/var/log/network-setup.log"

log_message() {
    echo "$(date): $1" >> "$LOG_FILE"
    echo "$(date): $1"  # Also output to console for debugging
}

log_message "Starting network interface setup..."

# Wait a bit for interfaces to be detected
sleep 2

# List all available interfaces for debugging
log_message "Available interfaces: $(ls /sys/class/net/)"

# Bring up all ethernet interfaces except loopback and request DHCP
for interface in /sys/class/net/enp0s*; do
    if [ -d "$interface" ]; then
        iface=$(basename "$interface")
        
        # Skip if it's the loopback
        if [[ "$iface" == "lo" ]]; then
            continue
        fi

        log_message "Processing interface: $iface"
        
        # Bring up the interface
        ip link set dev "$iface" up
        log_message "Interface $iface brought up"
        
        # Request DHCP lease with timeout and in background
        log_message "Requesting DHCP for $iface (timeout: 10s)"
        timeout 10 dhclient -1 -v "$iface" > /dev/null 2>&1 &
        
        # Don't wait - let it run in background
    fi
done

log_message "Network interface setup completed - DHCP requested for all interfaces (background)"

# Exit immediately, don't wait for dhclient processes
exit 0
