#!/bin/bash

# see: https://wiki.archlinux.org/title/QEMU#Tap_networking_with_QEMU

# =============================================================================
# HELPER FUNCTIONS
# =============================================================================

calculate_network_parameters() {
    local ip_address="$1"
    local netmask="$2"
    
    # Calculate mask bits
    mask_bits=0
    for octet in $(echo "$netmask" | sed 's/\./ /g'); do
        binbits=$(echo "obase=2; ibase=10; ${octet}" | bc | sed 's/0//g')
        mask_bits=$(expr $mask_bits + ${#binbits})
    done
    
    # Parse IP components
    IFS=. read -r i1 i2 i3 i4 <<< "$ip_address"
    IFS=. read -r m1 m2 m3 m4 <<< "$netmask"
    
    # Calculate DHCP range
    dhcp_range_start="$((i1 & m1)).$((i2 & m2)).$((i3 & m3)).2"
    dhcp_range_end="$((i1 & m1)).$((i2 & m2)).$((i3 & m3)).254"
    
    # Calculate network base and broadcast
    network_base="$((i1 & m1)).$((i2 & m2)).$((i3 & m3)).$((i4 & m4))"
    network_end="$((i1 | (255 - m1))).$((i2 | (255 - m2))).$((i3 | (255 - m3))).$((i4 | (255 - m4)))"
}

check_external_interface() {
    local external="$1"
    local bridge_id="$2"
    
    if [[ -n "$external" ]]; then
        if ! ip link show "$external" &>/dev/null; then
            log_error "External interface '$external' not found for bridge: $bridge_id"
            return 1
        else
            log_info "Internet access via: $external"
            return 0
        fi
    else
        return 1
    fi
}

setup_qemu_bridge_conf() {
    local bridge_id="$1"
    
    sudo touch /etc/qemu/bridge.conf
    
    if ! grep -q "^allow $bridge_id$" /etc/qemu/bridge.conf 2>/dev/null; then
        log_info "Adding $bridge_id to /etc/qemu/bridge.conf"
        echo "allow $bridge_id" | sudo tee -a /etc/qemu/bridge.conf >/dev/null
    else
        log_info "$bridge_id already allowed in /etc/qemu/bridge.conf"
    fi
}

update_bridge_ip() {
    local bridge_id="$1"
    local ip_address="$2"
    local mask_bits="$3"
    
    sudo ip addr flush dev "$bridge_id"
    sudo ip addr add "$ip_address"/"$mask_bits" dev "$bridge_id"
    sudo ip link set dev "$bridge_id" up
}

cleanup_existing_dnsmasq() {
    local bridge_id="$1"
    local pid_file="/var/run/dnsmasq-$bridge_id.pid"
    local lease_file="/var/lib/misc/dnsmasq-$bridge_id.leases"
    
    # Kill existing dnsmasq processes
    if [[ -f "$pid_file" ]]; then
        sudo kill "$(cat "$pid_file")" 2>/dev/null || true
        sudo rm -f "$pid_file"
    fi
    
    sudo pkill -f "dnsmasq.*$bridge_id" 2>/dev/null || true
    sudo rm -f "/tmp/dnsmasq-$bridge_id.conf"
    sudo rm -f "/var/log/dnsmasq-$bridge_id.log"
    sudo rm -f "$lease_file"
    sleep 3
}

get_files(){
    local bridge_id="$1"
    local config_file="/tmp/dnsmasq-$bridge_id.conf"
    local pid_file="/var/run/dnsmasq-$bridge_id.pid"
    local lease_file="/var/lib/misc/dnsmasq-$bridge_id.leases"
    local log_file="/var/log/dnsmasq-$bridge_id.log"

    echo "Config file: $config_file"
    echo "PID file: $pid_file"
    echo "Lease file: $lease_file"
    echo "Log file: $log_file"
}

create_dnsmasq_config() {
    local bridge_id="$1"
    local dhcp_range_start="$2"
    local dhcp_range_end="$3"
    local ip_address="$4"
    local netmask="$5"
    local internet_access="$6"
    local config_file="/tmp/dnsmasq-$bridge_id.conf"
    local pid_file="/var/run/dnsmasq-$bridge_id.pid"
    local lease_file="/var/lib/misc/dnsmasq-$bridge_id.leases"
    local log_file="/var/log/dnsmasq-$bridge_id.log"

    # Create the files if they don't exist
    sudo touch "$config_file" "$pid_file" "$lease_file" "$log_file"

    # Base configuration
    sudo tee "$config_file" > /dev/null << EOF
# Interface configuration
interface=$bridge_id
bind-interfaces
except-interface=lo

# DHCP configuration
dhcp-range=$dhcp_range_start,$dhcp_range_end,12h
dhcp-option=3,$ip_address
dhcp-option=1,$netmask
dhcp-authoritative
dhcp-leasefile=$lease_file
dhcp-lease-max=250

# Logging
log-dhcp
log-facility=$log_file
pid-file=$pid_file

# Uncomment to disable DNS server 
# port=0
# no-resolv
# no-hosts
EOF

    # DNS configuration based on internet access
    if [[ "$internet_access" == "true" ]]; then
        echo "dhcp-option=6,8.8.8.8,8.8.4.4" | sudo tee -a "$config_file" > /dev/null
    else
        echo "dhcp-option=6,$ip_address" | sudo tee -a "$config_file" > /dev/null
    fi
    
    echo "$config_file"
}

start_dnsmasq_server() {
    local bridge_id="$1"
    local config_file="$2"
    local pid_file="/var/run/dnsmasq-$bridge_id.pid"
    
    # Test configuration
    if ! sudo dnsmasq --conf-file="$config_file" --test 2>/tmp/dnsmasq-test-$bridge_id.log; then
        log_error "dnsmasq configuration test failed:"
        sudo cat "/tmp/dnsmasq-test-$bridge_id.log" | while read -r line; do
            log_error "  $line"
        done
        return 1
    fi
    
    # Start dnsmasq
    sudo dnsmasq --conf-file="$config_file" 2>/dev/null &
    sleep 3
    
    # Verify startup - check both process and log file
    local actual_pid log_file="/var/log/dnsmasq-$bridge_id.log"
    actual_pid=$(pgrep -f "dnsmasq.*$bridge_id" | head -1)
    
    # Check if process is running OR if log shows successful start
    if [[ -n "$actual_pid" ]] && kill -0 "$actual_pid" 2>/dev/null; then
        log_success "DHCP server started successfully for $bridge_id (PID: $actual_pid)"
        echo "$actual_pid" | sudo tee "$pid_file" > /dev/null
        return 0
    elif [[ -f "$log_file" ]] && grep -q "sockets bound exclusively to interface $bridge_id" "$log_file"; then
        # Process might have forked, try to find it by interface
        actual_pid=$(pgrep -f "interface.*$bridge_id" | head -1)
        if [[ -n "$actual_pid" ]]; then
            log_success "DHCP server started successfully for $bridge_id (PID: $actual_pid)"
            echo "$actual_pid" | sudo tee "$pid_file" > /dev/null
            return 0
        else
            # Log shows success but can't find PID - that's okay
            log_success "DHCP server started successfully for $bridge_id"
            return 0
        fi
    else
        log_error "Failed to start DHCP server for $bridge_id"
        return 1
    fi
}

verify_dnsmasq_status() {
    local bridge_id="$1"
    local dhcp_range_start="$2"
    local dhcp_range_end="$3"
    
    sleep 1
    local log_file="/var/log/dnsmasq-$bridge_id.log"
    local pid_file="/var/run/dnsmasq-$bridge_id.pid"
    local lease_file="/var/lib/misc/dnsmasq-$bridge_id.leases"

    
    if [[ -f "$log_file" ]]; then
        if grep -q "DHCP, IP range" "$log_file"; then
            log_success "DHCP range configured: $dhcp_range_start - $dhcp_range_end"
        fi
        if grep -q "sockets bound exclusively to interface $bridge_id" "$log_file"; then
            log_success "DHCP sockets bound successfully to $bridge_id"
        fi
        if grep -q "started.*DNS disabled" "$log_file"; then
            log_success "DHCP-only server started successfully"
        fi
        
        # Only show actual errors, not normal startup messages
        if grep -q "FAILED\|failed\|ERROR\|cannot\|unable" "$log_file"; then
            log_warning "Potential issues found in dnsmasq log:"
            grep -i "FAILED\|failed\|ERROR\|cannot\|unable" "$log_file" | while read -r line; do
                log_warning "  $line"
            done
        fi
    fi
}

show_dnsmasq_errors() {
    local bridge_id="$1"
    
    if sudo ss -ulp | grep -q ":67 "; then
        log_error "Port 67 is already in use by another process:"
        sudo ss -ulp | grep ":67 " | while read -r line; do
            log_error "  $line"
        done
    fi
    
    local log_file="/var/log/dnsmasq-$bridge_id.log"
    if [[ -f "$log_file" ]]; then
        log_error "Check $log_file for details:"
        sudo tail -15 "$log_file" | while read -r line; do
            log_error "  $line"
        done
    fi
}

setup_iptables_rules() {
    local bridge_id="$1"
    local ip_address="$2"
    local mask_bits="$3"
    local external="$4"
    local internet_access="$5"
    
    if [[ "$internet_access" == "true" && -n "$external" ]]; then
        # Remove existing rules to avoid duplicates
        sudo iptables -t nat -D POSTROUTING -s "$ip_address"/"$mask_bits" -o "$external" -j MASQUERADE 2>/dev/null || true
        sudo iptables -D FORWARD -i "$bridge_id" -o "$external" -j ACCEPT 2>/dev/null || true
        sudo iptables -D FORWARD -i "$external" -o "$bridge_id" -m state --state RELATED,ESTABLISHED -j ACCEPT 2>/dev/null || true
        
        # Add new rules
        sudo iptables -t nat -A POSTROUTING -s "$ip_address"/"$mask_bits" -o "$external" -j MASQUERADE
        sudo iptables -A FORWARD -i "$bridge_id" -o "$external" -j ACCEPT
        sudo iptables -A FORWARD -i "$external" -o "$bridge_id" -m state --state RELATED,ESTABLISHED -j ACCEPT
        
        log_success "NAT setup complete for $bridge_id -> $external (Internet access enabled)"
    else
        # Block internet access for isolated bridges
        sudo iptables -D FORWARD -i "$bridge_id" -j DROP 2>/dev/null || true
        sudo iptables -A FORWARD -i "$bridge_id" -j DROP
        log_info "$bridge_id configured as isolated network (no internet access)"
    fi
}

check_bridge_configuration() {
    local bridge_id="$1"
    local ip_address="$2"
    local mask_bits="$3"
    
    local result_ip current_ip current_mask
    result_ip=$(ip addr show dev "$bridge_id" | grep "inet " | awk '{print $2}')
    current_ip=$(echo "$result_ip" | cut -d'/' -f1)
    current_mask=$(echo "$result_ip" | cut -d'/' -f2)
    
    if [[ "$current_ip" != "$ip_address" || "$current_mask" != "$mask_bits" ]]; then
        log_warning "Current bridge configuration does not match desired configuration."
        log_warning "Current IP: $current_ip/$current_mask"
        log_warning "Desired IP: $ip_address/$mask_bits"
        
        read -p "Do you want to update the bridge configuration? (y/n) " answer
        if [[ "$answer" == "y" ]]; then
            return 0  # Need to update
        else
            return 1  # Don't update
        fi
    else
        log_success "Bridge $bridge_id configuration matches desired configuration."
        return 2  # Configuration matches
    fi
}

# =============================================================================
# MAIN FUNCTIONS
# =============================================================================

setup_dhcp_server() {
    local bridge_id="$1"
    local ip_address="$2"
    local netmask="$3"
    local internet_access="$4"
    local dhcp_range_start="$5"
    local dhcp_range_end="$6"
    local network_base="$7"
    
    local pid_file="/var/run/dnsmasq-$bridge_id.pid"
    
    log_info "Starting DHCP server for $bridge_id: $dhcp_range_start - $dhcp_range_end"
    log_info "Network base: $network_base, Gateway: $ip_address"
    
    # Cleanup existing processes
    cleanup_existing_dnsmasq "$bridge_id" 
    
    # Create configuration
    local config_file
    config_file=$(create_dnsmasq_config "$bridge_id" "$dhcp_range_start" "$dhcp_range_end" "$ip_address" "$netmask" "$internet_access")
    log_info "Using config file: $config_file"
    
    # Start DHCP server
    if start_dnsmasq_server "$bridge_id" "$config_file"; then
        verify_dnsmasq_status "$bridge_id" "$dhcp_range_start" "$dhcp_range_end"
        return 0
    else
        show_dnsmasq_errors "$bridge_id"
        return 1
    fi
}

bridge_setup() {
    local bridge_id="$1"
    local ip_address dhcp_range_start dhcp_range_end external netmask internet_access=false
    local mask_bits network_base network_end

    # Parse configuration
    ip_address=$(parse_yaml "$CONFIG_FILE" "vm.bridges.$bridge_id.ip")
    external=$(parse_yaml "$CONFIG_FILE" "vm.bridges.$bridge_id.external")
    netmask=$(parse_yaml "$CONFIG_FILE" "vm.bridges.$bridge_id.netmask")

    # Calculate network parameters
    calculate_network_parameters "$ip_address" "$netmask"

    log_info "Setting up bridge: $bridge_id with IP: $ip_address"
    log_info "Netmask: $netmask $mask_bits (bits)"
    log_info "DHCP range: $dhcp_range_start - $dhcp_range_end"

    # Check external interface
    if check_external_interface "$external" "$bridge_id"; then
        internet_access=true
    fi

    # Setup QEMU bridge configuration
    setup_qemu_bridge_conf "$bridge_id"

    # Create or check bridge
    local start_dhcp_server=false
    if ! ip link show "$bridge_id" &>/dev/null; then
        sudo ip link add name "$bridge_id" type bridge
        update_bridge_ip "$bridge_id" "$ip_address" "$mask_bits"
        start_dhcp_server=true
    else
        log_warning "Bridge $bridge_id already exists. Checking configuration..."
        
        case $(check_bridge_configuration "$bridge_id" "$ip_address" "$mask_bits") in
            0) # Need to update
                log_info "Updating bridge configuration..."
                update_bridge_ip "$bridge_id" "$ip_address" "$mask_bits"
                start_dhcp_server=true
                ;;
            1) # Don't update
                start_dhcp_server=false
                ;;
            2) # Configuration matches
                if ! pgrep -f "dnsmasq.*interface=$bridge_id" > /dev/null; then
                    log_info "DHCP server not running for $bridge_id, will start it"
                    start_dhcp_server=true
                else
                    log_info "DHCP server already running for $bridge_id"
                    start_dhcp_server=false
                fi
                ;;
        esac
    fi

    # Start DHCP server if needed
    if [[ "$start_dhcp_server" == "true" ]]; then
        setup_dhcp_server "$bridge_id" "$ip_address" "$netmask" "$internet_access" \
                         "$dhcp_range_start" "$dhcp_range_end" "$network_base"
    fi

    # Setup iptables rules
    setup_iptables_rules "$bridge_id" "$ip_address" "$mask_bits" "$external" "$internet_access"
}

bridges_setup() {
    log_info "Setting up bridge network..."

    local name_bridges
    name_bridges=$(get_yaml_subkeys "$CONFIG_FILE" vm.bridges)
    if [ $? -eq 0 ]; then
        for bridge in $name_bridges; do
            bridge_setup "$bridge"
        done
    else
        echo "Seção 'vm.bridges' não encontrada ou vazia"
        return 1
    fi

    sudo sysctl -w net.ipv4.ip_forward=1 >/dev/null
    return 0
}

bridges_teardown() {
    log_info "Tearing down bridge network..."

    local name_bridges
    name_bridges=$(get_yaml_subkeys "$CONFIG_FILE" vm.bridges)
    if [ $? -eq 0 ]; then
        for bridge in $name_bridges; do

            # remove dnsmasq process and pid file
            cleanup_existing_dnsmasq "$bridge"


            log_info "Removing bridge: $bridge"
            sudo ip link set dev "$bridge" down
            sudo ip link delete "$bridge"
        done
    else
        echo "Seção 'vm.bridges' não encontrada ou vazia"
        return 1
    fi

    sudo sysctl -w net.ipv4.ip_forward=0 >/dev/null
    return 0
}

bridges_status() {
    log_info "Checking bridge status..."

    local name_bridges
    name_bridges=$(get_yaml_subkeys "$CONFIG_FILE" vm.bridges)
    if [ $? -eq 0 ]; then
        for bridge in $name_bridges; do
            if ip link show "$bridge" >/dev/null 2>&1; then
                bridge_ip=$(ip addr show dev "$bridge" | grep "inet " | awk '{print $2}')
                log_info "Bridge $bridge is up - IP: $bridge_ip"
                
                # Check DHCP server status
                if pgrep -f "dnsmasq.*$bridge" > /dev/null; then
                    dhcp_pid=$(pgrep -f "dnsmasq.*$bridge")
                    log_info "  DHCP server running (PID: $dhcp_pid)"
                    
                    # Check if DHCP port is open on this specific bridge
                    if sudo ss -ulp | grep -q ":bootps.*$bridge"; then
                        log_success "  DHCP server is listening on $bridge:67"
                    elif sudo netstat -uln | grep -q ":67 "; then
                        log_warning "  DHCP server listening on port 67 but may not be bound to $bridge"
                    else
                        log_warning "  DHCP server not listening on port 67"
                    fi
                else
                    log_warning "  DHCP server not running"
                fi
                
                # Show connected devices and recent DHCP activity
                if [[ -f "/var/log/dnsmasq-$bridge.log" ]]; then
                    log_info "  Recent DHCP log entries:"
                    sudo tail -10 "/var/log/dnsmasq-$bridge.log" 2>/dev/null | while read -r line; do
                        if [[ "$line" =~ DHCP ]]; then
                            log_info "    $line"
                        fi
                    done
                fi
                
                # Show bridge interfaces
                bridge_ports=$(ls "/sys/class/net/$bridge/brif/" 2>/dev/null | tr '\n' ' ')
                if [[ -n "$bridge_ports" ]]; then
                    log_info "  Connected interfaces: $bridge_ports"
                    
                    # Show MAC addresses of connected interfaces
                    for port in $bridge_ports; do
                        if [[ -f "/sys/class/net/$port/address" ]]; then
                            mac=$(cat "/sys/class/net/$port/address")
                            log_info "    $port: $mac"
                        fi
                    done
                else
                    log_warning "  No interfaces connected to bridge"
                fi
                
                # Manual DHCP test suggestion
                log_info "  To test DHCP manually on VMs, run: sudo dhclient -v enp0s5"
                
            else
                log_warning "Bridge $bridge is down"
            fi
        done
    else
        echo "Seção 'vm.bridges' não encontrada ou vazia"
        return 1
    fi
}