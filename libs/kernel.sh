#!/bin/bash


# KERNEL MANAGEMENT FUNCTIONS
# =============================================================================

kernel_setup(){
    local kernel_version
    kernel_version=$(parse_yaml "$CONFIG_FILE" "kernel.version")
    
    
    # Create shared kernel cache directory
    local kernel_cache_dir="${MAIN_DIR}/.kernel"
    mkdir -p "$kernel_cache_dir"
    
    cd "$VM_DIR" || { log_error "Failed to change to VM directory"; return 1; }
    log_info "Setting up kernel in: $VM_DIR"
    
    # Check if we have kernel folder
    if [ -d "linux-$kernel_version" ]; then
        log_info "Linux kernel source already exists."
        return 0
    fi

    # Check if kernel source exists in cache first
    local kernel_archive="$kernel_cache_dir/linux-$kernel_version.tar.xz"
    if [ ! -f "$kernel_archive" ]; then
        log_info "Downloading Linux kernel source to cache..."
        if ! wget -O "$kernel_archive" "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$kernel_version.tar.xz"; then
            log_error "Failed to download kernel source"
            return 1
        fi
        log_success "Kernel source cached in: $kernel_cache_dir"
    else
        log_info "Using cached kernel source: $kernel_archive"
    fi

    # Extract kernel source from cache
    if [ -f "$kernel_archive" ] && [ ! -d "linux-$kernel_version" ]; then
        log_info "Extracting kernel source from cache..."
        if ! tar -xf "$kernel_archive"; then
            log_error "Failed to extract kernel source"
            return 1
        fi
    fi

    # Configure and compile kernel
    cd "linux-$kernel_version" || { log_error "Failed to change to kernel source directory"; return 1; }
    
    log_info "Creating default kernel configuration..."
    make defconfig
    
    # Enable kernel config options from YAML
    log_info "Enabling kernel configuration options..."
    while IFS= read -r config_option; do
        if [ -n "$config_option" ]; then
            log_info "Enabling: $config_option"
            scripts/config --enable "$config_option"
        fi
    done < <(get_config_array "$CONFIG_FILE" "config_options")
    
    log_info "Starting kernel compilation (this may take a while)..."
    if ! make -j"$(nproc)"; then
        log_error "Kernel compilation failed"
        return 1
    fi
    
    log_success "Kernel compilation completed"
    return 0
}

kernel_clean(){
    local kernel_version
    kernel_version=$(parse_yaml "$CONFIG_FILE" "kernel.version")
    
    cd "$VM_DIR" || { log_error "Failed to change to VM directory"; return 1; }
    
    if [ -d "linux-$kernel_version" ]; then
        log_info "Cleaning kernel build directory..."
        rm -rf "linux-$kernel_version"
        log_success "Kernel build directory removed"
    fi
    
    if [ -f "linux-$kernel_version.tar.xz" ]; then
        log_info "Removing kernel source archive..."
        rm -f "linux-$kernel_version.tar.xz"
        log_success "Kernel source archive removed"
    fi
}

kernel_status(){
    local kernel_version
    kernel_version=$(parse_yaml "$CONFIG_FILE" "kernel.version")
    
    cd "$VM_DIR" || { log_error "Failed to change to VM directory"; return 1; }
    
    log_info "Kernel Status for $VM_NAME:"
    
    if [ -f "linux-$kernel_version.tar.xz" ]; then
        local size
        size=$(du -h "linux-$kernel_version.tar.xz" | cut -f1)
        log_info "  Source archive: linux-$kernel_version.tar.xz ($size)"
    else
        log_warning "  Source archive: Not downloaded"
    fi
    
    if [ -d "linux-$kernel_version" ]; then
        local build_size
        build_size=$(du -sh "linux-$kernel_version" | cut -f1)
        log_info "  Build directory: linux-$kernel_version ($build_size)"
        
        if [ -f "linux-$kernel_version/arch/x86/boot/bzImage" ]; then
            local image_size
            image_size=$(du -h "linux-$kernel_version/arch/x86/boot/bzImage" | cut -f1)
            log_success "  Kernel image: bzImage ($image_size) - Ready"
        else
            log_warning "  Kernel image: Not built"
        fi
    else
        log_warning "  Build directory: Not extracted"
    fi
}