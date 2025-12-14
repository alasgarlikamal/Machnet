#!/bin/bash

# List of package to install on all machines
PACKAGES=( htop build-essential exuberant-ctags mosh cmake \
	silversearcher-ag pkg-config libelf-dev libdw-dev gcc-multilib python3 \
	python3-pip python3-venv libpcap-dev libpci-dev libnuma-dev flex bison \
	libslang2-dev libcap-dev libssl-dev libncurses-dev jq meson ninja-build \
	python3-pyelftools libyaml-dev libcsv-dev nlohmann-json3-dev gcc g++ \
	doxygen graphviz libhugetlbfs-dev libnl-3-dev libnl-route-3-dev \
	uuid-dev git-lfs libbfd-dev libbinutils gettext libtraceevent-dev \
	libzstd-dev libunwind-dev libreadline-dev numactl neovim )

install_all_package() {
	sudo apt update
	sudo apt install -y "${PACKAGES[@]}"
	pip install scapy flask
	# install linxu tools
	sudo apt install -y "linux-tools-$(uname -r)"
}

configure_dev_env() {
	# Install tmux-resurrect
	if [ ! -d $HOME/dev ]; then mkdir $HOME/dev; fi

	# Configure NET_IFACE
	EXPERIMENT_IP_RANGE="192.168"
	tmp_ifaces_info=( $(ip -json addr | jq '.[] | [.ifname, .ifindex, .addr_info[].local] | join("|")' | grep $EXPERIMENT_IP_RANGE) )
	if [ ${#tmp_ifaces_info[@]} -eq 1 ]; then
		iface_name=$(echo ${tmp_ifaces_info[0]} | tr -d '"' | cut -f 1 -d '|')
		# iface_index=$(echo ${tmp_ifaces_info[0]} | tr -d '"' | cut -f 2 -d '|')
		echo "export NET_IFACE=\"$iface_name\"" | tee -a $HOME/.bashrc
		sudo lshw | grep -C 2 $iface_name | grep pci | cut -d '@' -f 2 | xargs -I{} echo "export NET_PCI="{} | tee -a $HOME/.bashrc
		# echo "export NET_IFINDEX=$iface_index"
		# echo "export NET_PCI_ADDR=$iface_index"
	else
		echo Multiple interfaces with IP in experiment range found!
	fi
}

prepare_base_env() {
	install_all_package
	configure_dev_env
}

install_rdma_core() {
	mkdir -p $HOME/dev
	RDMA_CORE=$HOME/dev/rdma-core
	git clone -b 'stable-v40' --single-branch --depth 1 https://github.com/linux-rdma/rdma-core.git ${RDMA_CORE}
	cd ${RDMA_CORE}
	mkdir -p build && cd build
	cmake -GNinja -DNO_PYVERBS=1 -DNO_MAN_PAGES=1 ..
	sudo ninja install # as root
	sudo ldconfig
}

install_dpdk() {
	# DEPS
	# GRUB
	grub='GRUB_CMDLINE_LINUX_DEFAULT="default_hugepagesz=1G hugepagesz=1G hugepages=8 preempt=none"'
	echo $grub | sudo tee -a /etc/default/grub
	sudo update-grub
	# INSTALL DIR
	cd $HOME
	mkdir -p $HOME/dev/
	# DPDK
	cd $HOME/dev/
	wget https://fast.dpdk.org/rel/dpdk-23.11.tar.xz
	tar -xf ./dpdk-23.11.tar.xz
	cd dpdk-23.11/
	meson build/
	cd build/
	ninja
	sudo meson install
	sudo ldconfig
}

install_machnet() {
	# Install and set gcc-10 and g++-10 as the default compiler.
	sudo apt install -y gcc-10 g++-10 libgtest-dev # libgflags-dev
	sudo apt purge -y libgflags-dev
	sudo update-alternatives --install /usr/bin/gcc gcc /usr/bin/gcc-10 100 \
		--slave /usr/bin/g++ g++ /usr/bin/g++-10 \
		--slave /usr/bin/gcov gcov /usr/bin/gcov-10
	sudo apt-get --purge -y remove rdma-core librdmacm1 ibverbs-providers libibverbs-dev libibverbs1
	install_rdma_core
	install_dpdk

	cd $HOME/dev/
	git clone https://github.com/microsoft/machnet.git
	cd machnet
	git submodule update --init --recursive
	mkdir build && cd build && cmake -DCMAKE_BUILD_TYPE=Release -GNinja ../ && ninja
}

main() {
	prepare_base_env
	install_machnet
	echo Done
}

main

