#!/bin/bash

# =============================================================================
# ROOTFS MANAGEMENT FUNCTIONS
# =============================================================================

rootfs_setup(){
    local arch mirror suite
    
    # Debug: Check if CONFIG_FILE is available
    if [ -z "$CONFIG_FILE" ]; then
        log_error "CONFIG_FILE variable is not set in rootfs_setup"
        return 1
    fi
    
    log_info "Using config file: $CONFIG_FILE"
    
    arch=$(parse_yaml "$CONFIG_FILE" "debian.arch")
    suite=$(parse_yaml "$CONFIG_FILE" "debian.suite") 
    mirror=$(parse_yaml "$CONFIG_FILE" "debian.mirror")

    log_info "Parsed values: arch='$arch', suite='$suite', mirror='$mirror'"
    
    # Validate configuration values
    if [ -z "$arch" ] || [ -z "$suite" ] || [ -z "$mirror" ]; then
        log_error "Missing Debian configuration in YAML file"
        log_error "Required: debian.arch, debian.suite, debian.mirror"
        return 1
    fi

    cd "$VM_DIR" || { log_error "Failed to change to VM directory"; return 1; }
    log_info "Setting up root filesystem in: $VM_DIR"
    log_info "Configuration: $arch $suite from $mirror"
    
    if [ -d rootfs ]; then
        log_warning "Root filesystem already exists. Removing..."
        sudo rm -rf rootfs
    fi
    
    log_info "Creating Debian root filesystem..."
    if ! sudo debootstrap --arch="$arch" "$suite" rootfs "$mirror"; then
        log_error "Failed to create root filesystem"
        log_error "Check your network connection and Debian configuration"
        return 1
    fi

    # Set permissions
    sudo chown -R "$(whoami)":"$(whoami)" rootfs
    log_success "Root filesystem created"
    return 0
}

rootfs_config(){
    local username password root_password packages rootfs_size_mb
    username=$(parse_yaml "$CONFIG_FILE" "vm.username")
    password=$(parse_yaml "$CONFIG_FILE" "vm.password")
    root_password=$(parse_yaml "$CONFIG_FILE" "vm.root_password")
    rootfs_size_mb=$(parse_yaml "$CONFIG_FILE" "vm.rootfs_size_mb")
    
    cd "$VM_DIR" || { log_error "Failed to change to VM directory"; return 1; }
    log_info "Configuring root filesystem..."
    
    if [ ! -d rootfs ]; then
        log_error "Root filesystem directory not found. Run rootfs setup first."
        return 1
    fi
    
    # Get packages from YAML
    local packages=""
    while IFS= read -r package; do
        if [ -n "$package" ]; then
            packages+="$package "
        fi
    done < <(get_config_array "$CONFIG_FILE" "packages")

    log_info "Installing packages and configuring system..."
    sudo chroot rootfs /bin/bash <<EOF
set -e
apt update
apt upgrade -y
DEBIAN_FRONTEND=noninteractive apt install -y $packages

# Configure root password
echo "root:$root_password" | chpasswd
sed -i 's/#PermitRootLogin prohibit-password/PermitRootLogin yes/' /etc/ssh/sshd_config

# Create user account
useradd -m -s /bin/bash "$username"
echo "$username:$password" | chpasswd
usermod -aG sudo "$username"

# Configure SSH
systemctl enable ssh
sed -i 's/#PasswordAuthentication yes/PasswordAuthentication yes/' /etc/ssh/sshd_config

# Enable networking service for ifupdown
systemctl enable networking

# Give tcpdump capabilities to run without sudo
setcap cap_net_raw,cap_net_admin=eip /usr/bin/tcpdump
EOF

    if [ $? -ne 0 ]; then
        log_error "Failed to configure root filesystem"
        return 1
    fi

    # Create network interfaces configuration for manual control
    log_info "Configuring network interfaces..."
    local rootfs_dir="$VM_DIR/rootfs"

    sudo tee "$rootfs_dir/etc/network/interfaces" > /dev/null <<EOF
auto lo
iface lo inet loopback

# Interfaces will be managed by network-setup service
# They will automatically request DHCP when brought up
EOF

    # Create network setup script
    log_info "Installing network setup service..."
    sudo cp "${MAIN_DIR}/scripts/network-setup.sh" "$rootfs_dir/usr/local/bin/"
    sudo chmod +x "$rootfs_dir/usr/local/bin/network-setup.sh"
    
    # Install systemd service
    sudo cp "${MAIN_DIR}/scripts/network-setup.service" "$rootfs_dir/etc/systemd/system/"

    # Enable the service
    sudo chroot "$rootfs_dir" systemctl enable network-setup.service

    # Create filesystem image
    _create_rootfs_image "$rootfs_size_mb"
    return $?
}

_create_rootfs_image(){
    local rootfs_size_mb="$1"
    
    if [ -f rootfs.img ]; then
        log_warning "rootfs.img already exists. Recreating..."
        # Ensure it's unmounted before removing
        if mountpoint -q mnt_img 2>/dev/null; then
            sudo umount mnt_img 2>/dev/null || true
        fi
        rm -f rootfs.img
    fi
    
    log_info "Creating rootfs.img with size ${rootfs_size_mb}M..."
    if ! dd if=/dev/zero of=rootfs.img bs=1M count="$rootfs_size_mb" status=progress; then
        log_error "Failed to create rootfs.img"
        return 1
    fi
    
    if ! mkfs.ext4 -q rootfs.img; then
        log_error "Failed to format rootfs.img"
        return 1
    fi

    log_info "Copying rootfs contents into rootfs.img..."
    mkdir -p mnt_img
    
    if ! sudo mount -o loop rootfs.img mnt_img; then
        log_error "Failed to mount rootfs.img"
        return 1
    fi
    
    # Use trap to ensure unmount on exit
    trap 'sudo umount mnt_img 2>/dev/null || true' EXIT
    
    if ! sudo cp -a rootfs/. mnt_img/; then
        log_error "Failed to copy rootfs contents"
        sudo umount mnt_img 2>/dev/null || true
        trap - EXIT
        return 1
    fi
    
    sync
    sudo umount mnt_img
    trap - EXIT
    sudo chown "$(whoami)":"$(whoami)" rootfs.img
    
    log_success "Root filesystem configured and image created"
    return 0
}

rootfs_clean(){
    cd "$VM_DIR" || { log_error "Failed to change to VM directory"; return 1; }
    
    # Unmount if mounted
    if mountpoint -q mnt_img 2>/dev/null; then
        sudo umount mnt_img
    fi
    
    if [ -d rootfs ]; then
        log_info "Removing rootfs directory..."
        sudo rm -rf rootfs
        log_success "Root filesystem directory removed"
    fi
    
    if [ -f rootfs.img ]; then
        log_info "Removing rootfs.img..."
        rm -f rootfs.img
        log_success "Root filesystem image removed"
    fi
    
    if [ -d mnt_img ]; then
        log_info "Removing mount directory..."
        rmdir mnt_img 2>/dev/null || true
    fi
}

rootfs_status(){
    cd "$VM_DIR" || { log_error "Failed to change to VM directory"; return 1; }
    
    log_info "Root Filesystem Status for $VM_NAME:"
    
    if [ -d rootfs ]; then
        local rootfs_size
        rootfs_size=$(du -sh rootfs | cut -f1)
        log_info "  Root filesystem: Present ($rootfs_size)"
    else
        log_warning "  Root filesystem: Not created"
    fi
    
    if [ -f rootfs.img ]; then
        local img_size
        img_size=$(du -h rootfs.img | cut -f1)
        log_success "  Filesystem image: rootfs.img ($img_size) - Ready"
    else
        log_warning "  Filesystem image: Not created"
    fi
    
    if [ -d mnt_img ]; then
        if mountpoint -q mnt_img 2>/dev/null; then
            log_warning "  Mount point: mnt_img (Currently mounted)"
        else
            log_info "  Mount point: mnt_img (Available)"
        fi
    fi
}