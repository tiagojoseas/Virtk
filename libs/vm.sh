#!/bin/bash

# =============================================================================
# VM MANAGEMENT FUNCTIONS
# =============================================================================

vm_start(){
    local kernel_version memory cores
    kernel_version=$(parse_yaml "$CONFIG_FILE" "kernel.version")
    memory=$(parse_yaml "$CONFIG_FILE" "vm.memory")
    cores=$(parse_yaml "$CONFIG_FILE" "vm.number_cores")
    
    cd "$VM_DIR" || { log_error "Failed to change to VM directory"; return 1; }
    log_info "Starting VM from: $VM_DIR"
    
    # Validate required files
    if ! _validate_vm_files "$kernel_version"; then
        return 1
    fi
    
    # Get network configuration
    local network_args
    if ! network_args=$(_get_network_config); then
        log_error "Failed to get network configuration"
        return 1
    fi

    local kvm_args=""
    
    if [ -r /dev/kvm ] && (lsmod | grep -q kvm_intel || lsmod | grep -q kvm_amd || lsmod | grep -q kvm); then
        kvm_args="-enable-kvm"
        log_info "KVM acceleration enabled" >&2
    else
        log_warning "KVM not available, using software emulation" >&2
        log_warning "Do you want to activate KVM? (y/n)" >&2
        read -r answer
        if [[ "$answer" == "y" ]]; then
            sudo modprobe kvm
            sudo modprobe kvm_intel || sudo modprobe kvm_amd
            if lsmod | grep -q kvm; then
                kvm_args="-enable-kvm"
            fi
        fi
    fi
    
    log_info "Starting QEMU VM:"
    log_info "  Memory: $memory"
    log_info "  Cores: $cores"
    log_info "  Kernel: linux-$kernel_version/arch/x86/boot/bzImage"
    log_info "  Root FS: rootfs.img"
    log_info "  Network: $network_args"
    log_info "  KVM: ${kvm_args:-disabled}"

    read -p "Press Enter to continue..."
    
    # Start VM
    qemu-system-x86_64 \
        $kvm_args \
        -m "$memory" \
        -smp "$cores" \
        -kernel "linux-$kernel_version/arch/x86/boot/bzImage" \
        -drive file=rootfs.img,format=raw \
        -append "root=/dev/sda rw console=ttyS0 net.ifnames=1 biosdevname=0" \
        -nographic \
        ${network_args}
}

_validate_vm_files(){
    local kernel_version="$1"
    local valid=true
    
    if [ ! -f "linux-$kernel_version/arch/x86/boot/bzImage" ]; then
        log_error "Kernel image not found: linux-$kernel_version/arch/x86/boot/bzImage"
        log_error "Run: ./script.sh $CONFIG_FILE --kernel"
        valid=false
    fi
    
    if [ ! -f "rootfs.img" ]; then
        log_error "Root filesystem image not found: rootfs.img"
        log_error "Run: ./script.sh $CONFIG_FILE --rootfs"
        valid=false
    fi
    
    if [ "$valid" = false ]; then
        return 1
    fi
    
    return 0
}

_get_network_config(){
    local bridge_names network_args=""
    
    # Get bridge configuration
    if ! mapfile -t bridge_names < <(get_yaml_subkeys "$CONFIG_FILE" "vm.bridges"); then
        log_warning "No bridge configuration found"
        return 0
    fi
    
    for bridge_name in "${bridge_names[@]}"; do
        if [ -n "$bridge_name" ]; then
            local mac_address
            mac_address=$(parse_yaml "$CONFIG_FILE" "vm.bridges.${bridge_name}.mac_address")
            if [ -n "$mac_address" ]; then
                network_args+="-nic bridge,br=$bridge_name,mac=$mac_address "
            else
                log_warning "No MAC address found for bridge: $bridge_name"
            fi
        fi
    done
    
    echo "$network_args"
    return 0
}

vm_status(){
    local kernel_version
    kernel_version=$(parse_yaml "$CONFIG_FILE" "kernel.version")

    cd "$VM_DIR" || { log_error "Failed to change to VM directory"; return 1; }

    log_info "VM Status for $VM_NAME:"
    log_info "  Directory: $VM_DIR"
    
    # Check kernel
    if [ -f "linux-$kernel_version/arch/x86/boot/bzImage" ]; then
        log_success "  Kernel: Ready"
    else
        log_warning "  Kernel: Not built"
    fi
    
    # Check rootfs
    if [ -f "rootfs.img" ]; then
        local img_size
        img_size=$(du -h rootfs.img | cut -f1)
        log_success "  Root FS: Ready ($img_size)"
    else
        log_warning "  Root FS: Not created"
    fi
    
    # Check if VM is ready to start
    if [ -f "linux-$kernel_version/arch/x86/boot/bzImage" ] && [ -f "rootfs.img" ]; then
        log_success "  Status: Ready to start"
        log_info "  Start with: ./script.sh $(basename "$CONFIG_FILE") --vm"
    else
        log_warning "  Status: Setup required"
        log_info "  Setup with: ./script.sh $(basename "$CONFIG_FILE") --all"
    fi
}

vm_clean(){
    cd "$VM_DIR" || { log_error "Failed to change to VM directory"; return 1; }
    
    log_warning "This will remove all VM data for $VM_NAME"
    read -p "Are you sure? (y/N): " -n 1 -r
    echo
    
    if [[ ! $REPLY =~ ^[Yy]$ ]]; then
        log_info "Operation cancelled"
        return 0
    fi
    
    # Clean components
    log_info "Cleaning VM data..."
    
    # Unmount if mounted
    if mountpoint -q mnt_img 2>/dev/null; then
        sudo umount mnt_img
    fi
    
    # Remove all VM files
    cd "$MAIN_DIR" || return 1
    if [ -d "VirtK_Machines/$VM_NAME" ]; then
        rm -rf "VirtK_Machines/$VM_NAME"
        log_success "VM $VM_NAME completely removed"
    else
        log_warning "VM directory not found"
    fi
}