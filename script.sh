#!/bin/bash

set -e

# =============================================================================
# MAIN SCRIPT - VM MANAGEMENT SYSTEM
# =============================================================================

# Check if YAML file is provided
if [ $# -eq 0 ] || [ ! -f "$1" ]; then
    echo "Usage: $0 <config.yaml> [OPTIONS]"
    echo "Example: $0 server.yaml --all"
    echo ""
    echo "Available commands:"
    echo "  --all         Complete setup (kernel + rootfs + network + start VM)"
    echo "  --kernel      Setup and compile kernel only"  
    echo "  --rootfs      Setup root filesystem only"
    echo "  --network     Setup bridge network only"
    echo "  --vm          Start VM (setup network if needed)"
    echo "  --status      Show complete system status"
    echo "  --clean       Clean VM data (interactive)"
    echo "  --cache       Show kernel cache status"
    echo "  --cache-clean Clean kernel cache (interactive)"
    echo "  --teardown    Remove bridge network"
    exit 1
fi

# Initialize environment
MAIN_DIR=$(pwd)
CONFIG_FILE="$(realpath "$1")"  # Use absolute path
shift # Remove the YAML file from arguments

# Source all utility modules
source "${MAIN_DIR}/libs/utils.sh"
source "${MAIN_DIR}/libs/network.sh" 
source "${MAIN_DIR}/libs/kernel.sh"
source "${MAIN_DIR}/libs/rootfs.sh"
source "${MAIN_DIR}/libs/vm.sh"

VM_NAME=$(parse_yaml "$CONFIG_FILE" "vm.name" 2>/dev/null || echo "$(basename "$CONFIG_FILE" .yaml)")
VM_DIR="${MAIN_DIR}/VirtK_Machines/${VM_NAME}"
mkdir -p "$VM_DIR"

# Export variables so they're available in sourced modules
export CONFIG_FILE
export VM_NAME
export VM_DIR
export MAIN_DIR
export KERNEL_MACHINE
KERNEL_MACHINE=$(parse_yaml "$CONFIG_FILE" "kernel.machine")

log_info "VM Management System - $VM_NAME"
log_info "Config: $CONFIG_FILE"
log_info "VM Directory: $VM_DIR"
if [ -n "$KERNEL_MACHINE" ]; then
    log_info "Kernel machine: $KERNEL_MACHINE"
fi
echo ""


# =============================================================================
# CLEANUP AND COMPLETION
# =============================================================================

cleanup_mounts() {
    # Function to safely cleanup any mounted filesystems
    local vm_mount_dir="$VM_DIR/mnt_img"
    
    if [ -d "$vm_mount_dir" ]; then
        if mountpoint -q "$vm_mount_dir" 2>/dev/null; then
            log_info "Unmounting filesystem at $vm_mount_dir"
            if sudo umount "$vm_mount_dir" 2>/dev/null; then
                log_success "Successfully unmounted filesystem"
            else
                log_warning "Failed to unmount filesystem - may require manual intervention"
                log_info "To manually unmount: sudo umount $vm_mount_dir"
            fi
        fi
    fi
    
    # Also check for any other mounted loop devices related to rootfs.img
    local rootfs_img="$VM_DIR/rootfs.img"
    if [ -f "$rootfs_img" ]; then
        local loop_devices
        loop_devices=$(losetup -j "$rootfs_img" 2>/dev/null | cut -d: -f1)
        if [ -n "$loop_devices" ]; then
            log_info "Cleaning up loop devices for rootfs.img"
            echo "$loop_devices" | while read -r loop_dev; do
                if [ -n "$loop_dev" ]; then
                    sudo losetup -d "$loop_dev" 2>/dev/null || true
                fi
            done
        fi
    fi
}

# Always cleanup mounts at the end
cleanup_mounts


log_info "VM files location: $VM_DIR"
log_info "To check status: ./script.sh $(basename "$CONFIG_FILE") --status"

# =============================================================================
# CACHE MANAGEMENT FUNCTIONS  
# =============================================================================

kernel_cache_status(){
    local kernel_cache_dir="${MAIN_DIR}/.kernel"
    
    log_info "Kernel Cache Status:"
    log_info "  Cache directory: $kernel_cache_dir"
    
    if [ -d "$kernel_cache_dir" ]; then
        local cache_files
        cache_files=$(find "$kernel_cache_dir" -name "*.tar.xz" 2>/dev/null | wc -l)
        
        if [ "$cache_files" -gt 0 ]; then
            log_success "  Cached kernels: $cache_files"
            find "$kernel_cache_dir" -name "*.tar.xz" -exec basename {} \; | while read -r kernel_file; do
                local size
                size=$(du -h "$kernel_cache_dir/$kernel_file" | cut -f1)
                log_info "    $kernel_file ($size)"
            done
        else
            log_warning "  No cached kernels found"
        fi
        
        local total_size
        total_size=$(du -sh "$kernel_cache_dir" 2>/dev/null | cut -f1)
        log_info "  Total cache size: ${total_size:-0B}"
    else
        log_warning "  Cache directory does not exist"
    fi
}

kernel_cache_clean(){
    local kernel_cache_dir="${MAIN_DIR}/.kernel"
    
    if [ ! -d "$kernel_cache_dir" ]; then
        log_warning "Kernel cache directory does not exist"
        return 0
    fi
    
    local cache_files
    cache_files=$(find "$kernel_cache_dir" -name "*.tar.xz" 2>/dev/null | wc -l)
    
    if [ "$cache_files" -eq 0 ]; then
        log_info "No cached kernel files to remove"
        return 0
    fi
    
    log_warning "This will remove all cached kernel files"
    find "$kernel_cache_dir" -name "*.tar.xz" -exec basename {} \; | while read -r kernel_file; do
        local size
        size=$(du -h "$kernel_cache_dir/$kernel_file" | cut -f1)
        log_warning "  $kernel_file ($size)"
    done
    
    read -p "Are you sure? (y/N): " -n 1 -r
    echo
    
    if [[ $REPLY =~ ^[Yy]$ ]]; then
        rm -rf "$kernel_cache_dir"
        log_success "Kernel cache cleared"
    else
        log_info "Cache cleaning cancelled"
    fi
}

# =============================================================================
# COMMAND PROCESSING
# =============================================================================

usage() {
    echo "Usage: $0 <config.yaml> [OPTION]"
    echo ""
    echo "Available options:"
    echo "  -a |    --all         Complete setup"
    echo "  -k |    --kernel      Kernel setup only"
    echo "  -r |    --rootfs      Root filesystem setup only" 
    echo "  -v |    --vm          Start VM"
    echo "  -s |    --status      Show system status"
    echo "  -c |    --clean       Clean VM data"
    echo ""
    echo "Network Options:"
    echo "  -n |    --network     Network setup only"
    echo "  -t |    --teardown    Remove network"
}

case "${1:-}" in
    -a|--all)
        log_info "=== COMPLETE VM SETUP ==="
        kernel_setup && \
        rootfs_setup && \
        rootfs_config && \
        bridges_setup && \
        vm_start
        ;;
    
    -k|--kernel)
        log_info "=== KERNEL SETUP ==="
        kernel_setup
        ;;
    
    -r|--rootfs)
        log_info "=== ROOT FILESYSTEM SETUP ==="
        rootfs_setup && rootfs_config
        ;;    
    -v|--vm)
        log_info "=== STARTING VM ==="
        bridges_setup && vm_start
        ;;
    
    -s|--status)
        log_info "=== SYSTEM STATUS ==="
        echo ""
        kernel_cache_status
        echo ""
        kernel_status
        echo ""
        rootfs_status  
        echo ""
        vm_status
        echo ""
        bridges_status
        ;;
    
    --clean)
        log_info "=== CLEANING VM ==="
        vm_clean
        ;;
    
    --cache)
        log_info "=== KERNEL CACHE STATUS ==="
        kernel_cache_status
        ;;
    
    --cache-clean)
        log_info "=== CLEANING KERNEL CACHE ==="
        kernel_cache_clean
        ;;
    -n|--network)
        log_info "=== NETWORK STATUS ==="
        bridges_setup
        ;;
    -t|--teardown)
        log_info "=== NETWORK TEARDOWN ==="
        bridges_teardown
        ;;
    
    *)
        usage
        ;;
esac

exit 0
