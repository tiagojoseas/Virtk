#!/bin/bash


# KERNEL MANAGEMENT FUNCTIONS
# =============================================================================
choose_arch_defconfig() {
    local machine=$1
    case "$machine" in
        "raspberrypi4"|"rpi4"|"raspi4"|"raspberrypi4b"|"rpi4b")
            arch="arm64"
            cross_compile="aarch64-linux-gnu-"
            defconfig="bcm2711_defconfig"
            ;;
        *)
            arch="x86_64"
            defconfig="defconfig"
            ;;
    esac
}

handle_existing_kernel() {
    local kernel_version=$1
    if [ -d "linux-$kernel_version" ]; then
        log_warning "Kernel $kernel_version já existe. O que pretende fazer?"
        echo "0) Sair sem alterações"
        echo "1) Recompilar kernel existente"
        echo "2) Remover e voltar a descarregar"
        read -rp "Opção (0..2): " option
        case $option in
            0) log_info "A sair."; return 1 ;;
            1) log_info "A recompilar kernel existente" ;;
            2) log_info "A remover kernel existente"; rm -rf "linux-$kernel_version" ;;
            *) log_error "Opção inválida."; return 2 ;;
        esac
    fi
    return 0
}

fetch_rpi_kernel() {
    local kernel_version=$1 cache_dir=$2
    local rpi_branch="rpi-${kernel_version}"
    local repo_url="https://github.com/raspberrypi/linux.git"
    local archive="$cache_dir/linux-$kernel_version.tar.xz"
    local rpi_dir="$cache_dir/linux-$kernel_version"

    # Se não existe o tar.xz na cache, clona e compacta
    if [ ! -f "$archive" ]; then
        git ls-remote --exit-code --heads "$repo_url" "$rpi_branch" >/dev/null \
            || { log_error "Branch '$rpi_branch' não existe em $repo_url"; exit 1; }

        log_info "A clonar kernel do Raspberry Pi..."
        git clone --depth 1 --branch "$rpi_branch" "$repo_url" "$rpi_dir" \
            || { log_error "Clone falhou"; return 1; }

        log_info "A compactar kernel clonado em cache..."
        tar -cJf "$archive" -C "$cache_dir" "linux-$kernel_version" \
            || { log_error "Falha ao compactar kernel"; rm -rf "$rpi_dir"; return 1; }

        rm -rf "$rpi_dir"
        log_success "Kernel clonado e compactado em cache: $archive"
    else
        log_info "A usar kernel em cache: $archive"
    fi

    # Extrai para uso local se necessário
    if [ ! -d "linux-$kernel_version" ]; then
        log_info "A extrair kernel..."
        tar -xf "$archive" || { log_error "Extração falhou"; return 1; }
    fi
}

fetch_generic_kernel() {
    local kernel_version=$1 cache_dir=$2
    local archive="$cache_dir/linux-$kernel_version.tar.xz"

    if [ ! -f "$archive" ]; then
        log_info "A descarregar kernel..."
        wget -O "$archive" "https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-$kernel_version.tar.xz" \
            || { log_error "Download falhou"; return 1; }
        log_success "Kernel guardado em cache"
    else
        log_info "A usar kernel em cache: $archive"
    fi

    [ -d "linux-$kernel_version" ] || {
        log_info "A extrair kernel..."
        tar -xf "$archive" || { log_error "Extração falhou"; return 1; }
    }
}

apply_patches_if_needed() {
    local kernel_version=$1
    if [ "$VM_NAME" = "client" ]; then
        log_info "A copiar patches..."
        cp -r "/home/tiago/xlayer-scheduler/scheduler/" "linux-$kernel_version/net/mptcp/" \
            && log_success "Patches aplicados"
    fi
}

configure_kernel() {
    local arch=$1 defconfig=$2 cross_compile=$3
    log_info "Criar configuração padrão (arch=$arch, defconfig=$defconfig)"

    if [ "$arch" = "x86_64" ]; then
        make defconfig
    elif [ "$arch" = "arm64" ]; then
        make ARCH=$arch CROSS_COMPILE=$cross_compile $defconfig
    fi

    # Activate additional config options from YAML Config
    while IFS= read -r config_option; do
        log_info "Applying config option: $config_option"
        [ -n "$config_option" ] && scripts/config --enable "$config_option"
    done < <(get_config_array "$CONFIG_FILE" "config_options")

    # Enable virtio configs for Raspberry Pi 4B
    if [ "$defconfig" = "bcm2711_defconfig" ]; then
        log_info "Enabling virtio block configs for rpi4b..."

        ENABLE_RPI_CONFIGS=(
            CONFIG_VIRTIO
            CONFIG_VIRTIO_PCI
            CONFIG_VIRTIO_BLK
            CONFIG_VIRTIO_MMIO
            CONFIG_VIRTIO_NET
            CONFIG_VIRTIO_VSOCKETS
            CONFIG_VIRTIO_BALLOON
            CONFIG_VIRTIO_CONSOLE
            CONFIG_VIRTIO_INPUT
            CONFIG_VIRTIO_MMIO_CMDLINE_DEVICES
            CONFIG_VIRTIO_IOMMU
            CONFIG_BALLOON_COMPACTION
            CONFIG_NET_9P_VIRTIO
            CONFIG_FW_LOADER_DEBUG
            CONFIG_SCSI_VIRTIO
            CONFIG_HW_RANDOM_VIRTIO
            CONFIG_SND_VIRTIO
            CONFIG_GDB_SCRIPTS
            CONFIG_DEBUG_EFI
            CONFIG_DEBUG_INFO_COMPRESSED_NONE 
        )


        for config in "${ENABLE_RPI_CONFIGS[@]}"; do
            scripts/config --enable "$config"
        done

        DISABLE_RPI_CONFIGS=( 
            CONFIG_VIRTIO_PCI_LEGACY
            CONFIG_GPIO_VIRTIO
            CONFIG_BT_VIRTIO
            CONFIG_TEST_DYNAMIC_DEBUG
        )
        
        for config in "${DISABLE_RPI_CONFIGS[@]}"; do
            scripts/config --disable "$config"
        done

    fi
  

}

compile_kernel() {
    local arch=$1 cross_compile=$2
    log_info "A compilar kernel..."
    if [ "$arch" = "x86_64" ]; then
        make -j"$(nproc)" || { log_error "Compilação falhou"; return 1; }
    else
        make ARCH=$arch CROSS_COMPILE=$cross_compile Image modules dtbs -j"$(nproc)" \
            || { log_error "Compilação falhou"; return 1; }
    fi
}

# === Função principal ===

kernel_setup() {
    local kernel_version machine arch defconfig cross_compile
    kernel_version=$(parse_yaml "$CONFIG_FILE" "kernel.version")
    machine=$(parse_yaml "$CONFIG_FILE" "kernel.machine")

    choose_arch_defconfig "$machine"

    local kernel_cache_dir="${MAIN_DIR}/.kernel"
    mkdir -p "$kernel_cache_dir"

    cd "$VM_DIR" || { log_error "Failed to change to VM directory"; return 1; }
    log_info "Setting up kernel in: $VM_DIR"

    handle_existing_kernel "$kernel_version" || return $?

    if [ "$arch" = "arm64" ]; then
        fetch_rpi_kernel "$kernel_version" "$kernel_cache_dir" || return 1
    else
        fetch_generic_kernel "$kernel_version" "$kernel_cache_dir" || return 1
    fi

    apply_patches_if_needed "$kernel_version"

    cd "linux-$kernel_version" || { log_error "Failed to enter kernel source"; return 1; }

    configure_kernel "$arch" "$defconfig" "$cross_compile"
    compile_kernel "$arch" "$cross_compile"

    log_success "Compilação concluída!"
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
    local kernel_version machine arch
    kernel_version=$(parse_yaml "$CONFIG_FILE" "kernel.version")
    machine=$(parse_yaml "$CONFIG_FILE" "kernel.machine")
    if [ -n "$machine" ]; then
        case "$machine" in
            "raspberrypi4"|"rpi4"|"raspberrypi4b"|"rpi4b")
                arch="arm64"
                ;;
            *)
                arch="x86_64"
                ;;
        esac
    else
        arch="x86_64"
    fi

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

        if [ "$arch" = "x86_64" ]; then
            if [ -f "linux-$kernel_version/arch/x86/boot/bzImage" ]; then
                local image_size
                image_size=$(du -h "linux-$kernel_version/arch/x86/boot/bzImage" | cut -f1)
                log_success "  Kernel image: bzImage ($image_size) - Ready"
            else
                log_warning "  Kernel image: Not built"
            fi
        else
            if [ -f "linux-$kernel_version/arch/$arch/boot/Image" ]; then
                local image_size
                image_size=$(du -h "linux-$kernel_version/arch/$arch/boot/Image" | cut -f1)
                log_success "  Kernel image: Image ($image_size) - Ready"
            else
                log_warning "  Kernel image: Not built"
            fi
        fi
    else
        log_warning "  Build directory: Not extracted"
    fi
}