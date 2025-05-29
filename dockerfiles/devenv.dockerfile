# Stage 1: Base system packages and build dependencies
FROM ubuntu:22.04 AS machnet_build_base

# Fixes QEMU-based builds so they don't emulate an x86-64-v1 CPU
ENV QEMU_CPU max

# Set timezone and configure apt
ARG timezone
RUN ln -snf /usr/share/zoneinfo/${timezone} /etc/localtime && \
    echo ${timezone} > /etc/timezone && \
    echo 'APT::Install-Suggests "0";' >> /etc/apt/apt.conf.d/00-docker && \
    echo 'APT::Install-Recommends "0";' >> /etc/apt/apt.conf.d/00-docker

# Update and install dependencies
RUN apt-get update && \
    apt-get install --no-install-recommends -y \
        git \
        curl \ 
        clangd clang-format clang-tidy \
        build-essential cmake meson pkg-config libudev-dev \
        libnl-3-dev libnl-route-3-dev python3-dev \
        python3-docutils python3-pyelftools libnuma-dev \
        ca-certificates autoconf \
        libhugetlbfs-dev pciutils libunwind-dev uuid-dev nlohmann-json3-dev \
        sudo vim libgflags-dev build-essential wget lsb-release software-properties-common gnupg && \
    rm -rf /var/lib/apt/lists/*

# Create user and add to sudo group
RUN useradd -m -s /bin/bash vj2267 && \
    echo "vj2267 ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers

# Switch to vj2267 user for Rust installation
USER vj2267
WORKDIR /home/vj2267

# Install Rust and development tools for vj2267
RUN curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y && \
    . $HOME/.cargo/env && \
    rustup component add rustfmt clippy rust-analyzer && \
    rustup target add x86_64-unknown-none && \
    cargo install cargo-watch cargo-edit cargo-expand just

# Add cargo to PATH in .bashrc
RUN echo 'export PATH="$HOME/.cargo/bin:$PATH"' >> $HOME/.bashrc && \
    echo 'source "$HOME/.cargo/env"' >> $HOME/.bashrc

# Switch back to root for system-level operations
USER root

# Remove conflicting packages
RUN apt-get update && \
    apt-get --purge -y remove rdma-core librdmacm1 ibverbs-providers libibverbs-dev libibverbs1 && \
    rm -rf /var/lib/apt/lists/*

# Install clang
RUN wget https://apt.llvm.org/llvm.sh && \
    chmod +x ./llvm.sh && \
    ./llvm.sh 17 all && \
    ln -s /usr/lib/llvm-17/bin/clang-cl /usr/bin/clang-cl && \
    ln -s /usr/lib/llvm-17/bin/llvm-lib /usr/bin/llvm-lib && \
    ln -s /usr/lib/llvm-17/bin/lld-link /usr/bin/lld-link && \
    ln -s /usr/lib/llvm-17/bin/llvm-ml /usr/bin/llvm-ml && \
    ln -s /usr/lib/llvm-17/bin/ld.lld /usr/bin/ld.lld && \
    ln -s /usr/lib/llvm-17/bin/clang /usr/bin/clang

# Set working directory
WORKDIR /home/vj2267

# Stage 2: RDMA Core build
FROM machnet_build_base AS rdma_core_build

# Set env variable for rdma-core
ENV RDMA_CORE /home/vj2267/rdma-core

# Build rdma-core
RUN git clone -b 'stable-v52' --single-branch --depth 1 https://github.com/linux-rdma/rdma-core.git ${RDMA_CORE} && \
    cd ${RDMA_CORE} && \
    mkdir build && \
    cd build && \
    cmake -GNinja -DNO_PYVERBS=1 -DNO_MAN_PAGES=1 ../ && \
    ninja install

# ldconfig to update the dynamic linker run-time bindings
RUN echo /usr/local/lib64 > /etc/ld.so.conf.d/usr_local.conf && ldconfig

# Stage 3: DPDK build
FROM rdma_core_build AS dpdk_build

# Set env variable for DPDK
ENV RTE_SDK /home/vj2267/dpdk

# Parts of DPDK that aren't needed for Machnet are disabled to help with the container size
ENV DPDK_DISABLED_APPS dumpcap,graph,pdump,proc-info,test-acl,test-bbdev,test-cmdline,test-compress-perf,test-crypto-perf,test-dma-perf,test-eventdev,test-fib,test-flow-perf,test-gpudev,test-mldev,test-pipeline,test-regex,test-sad,test-security-perf
ENV DPDK_DISABLED_DRIVER_GROUPS raw/*,crypto/*,baseband/*,dma/*,event/*,regex/*,ml/*,gpu/*,vdpa/*,compress/*
ENV DPDK_DISABLED_COMMON_DRIVERS common/qat,common/octeontx,common/octeontx2,common/cnxk,common/dpaax
# probably the only safe bus driver to disable
ENV DPDK_DISABLED_BUS_DRIVERS bus/ifpga
# PMDs which don't meet the minimum requirements for Machnet
ENV DPDK_DISABLED_NIC_DRIVERS net/softnic,net/tap,net/af_packet,net/af_xdp,net/avp,net/bnx2x,net/memif,net/nfb,net/octeon_ep,net/pcap,net/ring,net/tap

# Additional drivers to disable. Intended to allow disabling drivers not needed in your environment to save on image size. This needs to end with a comma.
ARG DPDK_ADDITIONAL_DISABLED_DRIVERS

ENV DPDK_DISABLED_DRIVERS ${DPDK_ADDITIONAL_DISABLED_DRIVERS}${DPDK_DISABLED_DRIVER_GROUPS},${DPDK_DISABLED_COMMON_DRIVERS},${DPDK_DISABLED_BUS_DRIVERS},${DPDK_DISABLED_NIC_DRIVERS}

# Enabling a driver wins over disabling a driver, so if you the user disagree with any of our decisions add a comma delimited list of drivers to re-enable.
# For example, Marvell OcteonTX2 would be enabled by passing '--build-arg="DPDK_ENABLED_DRIVERS=common/octeontx2"' to the build command
ARG DPDK_ENABLED_DRIVERS

# Set the DPDK platform for ARM SOCs
ARG DPDK_PLATFORM=generic

# Additional Meson defines
ARG DPDK_EXTRA_MESON_DEFINES

# Preset to build DPDK with. Defaults to release mode with debug info.
ARG DPDK_MESON_BUILD_PRESET=debugoptimized

# Build DPDK
RUN git clone --depth 1 --branch 'v23.11' https://github.com/DPDK/dpdk.git ${RTE_SDK} && \
    cd ${RTE_SDK} && \
    meson setup build --buildtype=${DPDK_MESON_BUILD_PRESET} -Dexamples='' -Dplatform=${DPDK_PLATFORM} -Denable_kmods=false -Dtests=false -Ddisable_apps=${DPDK_DISABLED_APPS} -Ddisable_drivers=${DPDK_DISABLED_DRIVERS} -Denable_drivers='${DPDK_ENABLED_DRIVERS}' ${DPDK_EXTRA_MESON_DEFINES} && \
    ninja -C build install && \
    cd /

# Stage 4: Development environment (for devcontainer use)
FROM dpdk_build AS machnet_dev

# Set ownership of all files to vj2267
RUN chown -R vj2267:vj2267 /home/vj2267

# Switch to vj2267 user
USER vj2267
WORKDIR /home/vj2267/machnet
ENTRYPOINT ["/bin/bash"]