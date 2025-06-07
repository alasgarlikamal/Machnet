#!/bin/bash

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

# Function to print status messages
print_status() {
    echo -e "${GREEN}[+]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[!]${NC} $1"
}

print_error() {
    echo -e "${RED}[-]${NC} $1"
}

# Function to check if a command exists
command_exists() {
    command -v "$1" >/dev/null 2>&1
}

# Function to install system packages
install_system_packages() {
    print_status "Installing system packages..."

    sudo apt-get update && \
    sudo apt-get install --no-install-recommends -y \
        git \
        curl \
        clangd clang-format clang-tidy \
        build-essential cmake meson pkg-config libudev-dev \
        libnl-3-dev libnl-route-3-dev python3-dev \
        python3-docutils python3-pyelftools libnuma-dev \
        ca-certificates autoconf \
        libhugetlbfs-dev pciutils libunwind-dev uuid-dev nlohmann-json3-dev \
        sudo vim libgflags-dev

    # Remove conflicting packages
    sudo apt-get --purge -y remove rdma-core librdmacm1 ibverbs-providers libibverbs-dev libibverbs1
}

# Function to install Rust and development tools
install_rust() {
    print_status "Installing Rust and development tools..."

    if ! command_exists rustup; then
        curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y
        source "$HOME/.cargo/env"
    fi

    rustup component add rustfmt clippy rust-analyzer
    cargo install cargo-watch cargo-edit cargo-expand

    # Add cargo to PATH in .bashrc if not already present
    if ! grep -q "export PATH=\"\$HOME/.cargo/bin:\$PATH\"" "$HOME/.bashrc"; then
        echo 'export PATH="$HOME/.cargo/bin:$PATH"' >> "$HOME/.bashrc"
        echo 'source "$HOME/.cargo/env"' >> "$HOME/.bashrc"
    fi
}

# Function to build and install rdma-core
install_rdma_core() {
    print_status "Building and installing rdma-core..."

    RDMA_CORE="$HOME/rdma-core"

    if [ ! -d "$RDMA_CORE" ]; then
        git clone -b 'stable-v52' --single-branch --depth 1 https://github.com/linux-rdma/rdma-core.git "$RDMA_CORE"
    fi

    cd "$RDMA_CORE"
    mkdir -p build
    cd build
    cmake -GNinja -DNO_PYVERBS=1 -DNO_MAN_PAGES=1 ../
    sudo ninja install

    # Update dynamic linker run-time bindings
    echo /usr/local/lib64 | sudo tee /etc/ld.so.conf.d/usr_local.conf
    sudo ldconfig
}

# Function to build and install DPDK
install_dpdk() {
    print_status "Building and installing DPDK..."

    RTE_SDK="$HOME/dpdk"

    # DPDK configuration
    export DPDK_DISABLED_APPS="dumpcap,graph,pdump,proc-info,test-acl,test-bbdev,test-cmdline,test-compress-perf,test-crypto-perf,test-dma-perf,test-eventdev,test-fib,test-flow-perf,test-gpudev,test-mldev,test-pipeline,test-regex,test-sad,test-security-perf"
    export DPDK_DISABLED_DRIVER_GROUPS="raw/*,crypto/*,baseband/*,dma/*,event/*,regex/*,ml/*,gpu/*,vdpa/*,compress/*"
    export DPDK_DISABLED_COMMON_DRIVERS="common/qat,common/octeontx,common/octeontx2,common/cnxk,common/dpaax"
    export DPDK_DISABLED_BUS_DRIVERS="bus/ifpga"
    export DPDK_DISABLED_NIC_DRIVERS="net/softnic,net/tap,net/af_packet,net/af_xdp,net/avp,net/bnx2x,net/memif,net/nfb,net/octeon_ep,net/pcap,net/ring,net/tap"

    if [ ! -d "$RTE_SDK" ]; then
        git clone --depth 1 --branch 'v23.11' https://github.com/DPDK/dpdk.git "$RTE_SDK"
    fi

    cd "$RTE_SDK"
    meson setup build --buildtype=debugoptimized \
        -Dexamples='' \
        -Dplatform=generic \
        -Denable_kmods=false \
        -Dtests=false \
        -Ddisable_apps=${DPDK_DISABLED_APPS} \
        -Ddisable_drivers=${DPDK_DISABLED_DRIVER_GROUPS},${DPDK_DISABLED_COMMON_DRIVERS},${DPDK_DISABLED_BUS_DRIVERS},${DPDK_DISABLED_NIC_DRIVERS}

    sudo ninja -C build install
}

# Main function
main() {
    print_status "Starting Machnet prerequisites installation..."

    # Check if running as root
    if [ "$EUID" -eq 0 ]; then
        print_error "Please do not run this script as root"
        exit 1
    fi

    # Install system packages
    install_system_packages

    # Install Rust and development tools
    install_rust

    # Install rdma-core
    install_rdma_core

    # Install DPDK
    install_dpdk

    print_status "Installation completed successfully!"
}

# Run main function
main
