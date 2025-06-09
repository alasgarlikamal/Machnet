# Machnet Makefile
# This makefile is used for building Machnet and its components, including Docker containers and the shim library

SHELL=/bin/bash -e -o pipefail

# Shim library configuration
SHIM_SRC_DIR=src/ext
SHIM_LIB=libmachnet_shim.so
SHIM_INSTALL_DIR?=/usr/lib
SHIM_DEPS=libgflags-dev
SHIM_SRC_FILES=$(shell find $(SHIM_SRC_DIR) -type f -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" -o -name "Makefile")

# Docker build configuration
BUILD_COMMAND=docker buildx bake -f docker-bake.hcl
GET_BUILDX_INFO_COMMAND=$(BUILD_COMMAND) --print
BUILD_TARGETS_COMMAND=xargs $(BUILD_COMMAND)
GET_TARGETS_FOR_ARCH_CMD=python3 $(CURDIR)/dockerfiles/get_targets_for_arch.py

# By default, load into the local docker registry, can be overriden with --push for production builds
BUILD_COMMAND_EXTRA_ARGS=--load

# Build directories for different build types
DEBUG_BUILD_DIR=debug_build
RELEASE_BUILD_DIR=release_build

# Source directories to track for changes
SRC_DIRS=src dockerfiles examples
SRC_FILES=$(shell find $(SRC_DIRS) -type f -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" -o -name "CMakeLists.txt" -o -name "*.cmake")

# Build artifacts paths
DEBUG_BINARY=$(DEBUG_BUILD_DIR)/src/apps/machnet/machnet
RELEASE_BINARY=$(RELEASE_BUILD_DIR)/src/apps/machnet/machnet
MSG_GEN_BINARY=$(RELEASE_BUILD_DIR)/src/apps/msg_gen/msg_gen

# Hugepage configuration
HUGEPAGE_SIZE=2048
HUGEPAGE_COUNT=1024
HUGEPAGE_PATH=/sys/devices/system/node/node*/hugepages/hugepages-$(HUGEPAGE_SIZE)kB/nr_hugepages

# Configuration file path
CONFIG_FILE?=src/apps/machnet/config.json

# Default IPs for msg_gen
SERVER_IP?=10.10.1.1
CLIENT_IP?=10.10.1.2

# Define all phony targets
.PHONY: all_containers x86_containers arm_containers debug release clean run_machnet setup_hugepages run_msg_gen_server_cpp run_msg_gen_client_cpp shim check_shim_deps build_shim help git_submodules

# Default target is help
.DEFAULT_GOAL := help

# Help target to display available targets and their descriptions
help:
	@echo "Available targets:"
	@echo "  help                    - Display this help message"
	@echo "  all_containers          - Build all container variants"
	@echo "  x86_containers          - Build containers for x86 architecture"
	@echo "  arm_containers          - Build containers for ARM architecture"
	@echo "  native_containers       - Build containers for the current system architecture"
	@echo "  debug                   - Build Machnet in debug mode"
	@echo "  release                 - Build Machnet in release mode"
	@echo "  clean                   - Remove all build artifacts"
	@echo "  setup_hugepages         - Configure system hugepages"
	@echo "  run_machnet             - Run Machnet with the specified config"
	@echo "  run_msg_gen_server_cpp  - Run msg_gen server"
	@echo "  run_msg_gen_client_cpp  - Run msg_gen client"
	@echo "  shim                    - Build and install the Machnet shim library (requires sudo)"
	@echo "  git_submodules          - Initialize and update git submodules"
	@echo ""
	@echo "Variables that can be overridden:"
	@echo "  SHIM_INSTALL_DIR        - Directory to install shim library (default: /usr/lib)"
	@echo "  CONFIG_FILE             - Path to Machnet config file"
	@echo "  SERVER_IP               - IP address for msg_gen server"
	@echo "  CLIENT_IP               - IP address for msg_gen client"

# Git submodules target
git_submodules:
	@echo "Initializing and updating git submodules..."
	git submodule update --init --recursive

# Users likely want to get containers that work on the current system,
# so that is the default.
default_containers: native_containers

all_containers:
	$(BUILD_COMMAND) $(BUILD_COMMAND_EXTRA_ARGS)

x86_containers:
	$(GET_BUILDX_INFO_COMMAND) | $(GET_TARGETS_FOR_ARCH_CMD) --arch x86 | $(BUILD_TARGETS_COMMAND) $(BUILD_COMMAND_EXTRA_ARGS)

arm_containers:
	$(GET_BUILDX_INFO_COMMAND) | $(GET_TARGETS_FOR_ARCH_CMD) --arch arm | $(BUILD_TARGETS_COMMAND) $(BUILD_COMMAND_EXTRA_ARGS)

native_containers:
	$(GET_BUILDX_INFO_COMMAND) | $(GET_TARGETS_FOR_ARCH_CMD) --arch native | $(BUILD_TARGETS_COMMAND) $(BUILD_COMMAND_EXTRA_ARGS)

# Build targets for Machnet
debug: $(DEBUG_BINARY)

release: $(RELEASE_BINARY)

# Debug build rules
$(DEBUG_BUILD_DIR)/CMakeCache.txt: $(SRC_FILES)
	@mkdir -p $(DEBUG_BUILD_DIR)
	cd $(DEBUG_BUILD_DIR) && cmake -DCMAKE_BUILD_TYPE=Debug -GNinja ..

$(DEBUG_BINARY): git_submodules $(DEBUG_BUILD_DIR)/CMakeCache.txt
	cd $(DEBUG_BUILD_DIR) && ninja

# Release build rules
$(RELEASE_BUILD_DIR)/CMakeCache.txt: $(SRC_FILES)
	@mkdir -p $(RELEASE_BUILD_DIR)
	cd $(RELEASE_BUILD_DIR) && cmake -DCMAKE_BUILD_TYPE=Release -GNinja ..

$(RELEASE_BINARY): git_submodules $(RELEASE_BUILD_DIR)/CMakeCache.txt
	cd $(RELEASE_BUILD_DIR) && ninja

# Hugepage setup
setup_hugepages:
	@echo "Checking hugepages..."
	@if [ $$(cat $(HUGEPAGE_PATH) | head -n1) -lt $(HUGEPAGE_COUNT) ]; then \
		echo "Setting up hugepages..."; \
		echo $(HUGEPAGE_COUNT) | sudo tee $(HUGEPAGE_PATH) > /dev/null; \
		echo "Hugepages configured."; \
	else \
		echo "Hugepages already configured."; \
	fi

# Run Machnet
run_machnet: setup_hugepages $(RELEASE_BINARY)
	@echo "Starting Machnet with config: $(CONFIG_FILE)..."
	sudo GLOG_logtostderr=1 $(RELEASE_BINARY) -config_json $(CONFIG_FILE)

# Run msg_gen server
run_msg_gen_server_cpp: setup_hugepages $(RELEASE_BINARY)
	@echo "Starting msg_gen server on IP: $(SERVER_IP)..."
	sudo GLOG_logtostderr=1 $(MSG_GEN_BINARY) --local_ip $(SERVER_IP)

# Run msg_gen client
run_msg_gen_client_cpp: setup_hugepages $(RELEASE_BINARY)
	@echo "Starting msg_gen client on IP: $(CLIENT_IP) connecting to server: $(SERVER_IP)..."
	sudo GLOG_logtostderr=1 $(MSG_GEN_BINARY) --local_ip $(CLIENT_IP) --remote_ip $(SERVER_IP)

# Shim library targets
check_shim_deps:
	@echo "Checking shim dependencies..."
	@for dep in $(SHIM_DEPS); do \
		if ! dpkg -l | grep -q "^ii  $$dep "; then \
			echo "Installing $$dep..."; \
			sudo apt-get update -y && sudo apt-get install -y $$dep; \
		fi \
	done

build_shim: check_shim_deps
	@echo "Building Machnet shim library..."
	cd $(SHIM_SRC_DIR) && make clean && make
	@if [ ! -f "$(SHIM_SRC_DIR)/$(SHIM_LIB)" ]; then \
		echo "Error: Building Machnet shim library failed. Please check the build process."; \
		exit 1; \
	fi
	cp $(SHIM_SRC_DIR)/$(SHIM_LIB) $(CURDIR)/
	@echo "Installing Machnet shim library to $(SHIM_INSTALL_DIR)..."
	@if [ ! -w "$(SHIM_INSTALL_DIR)" ]; then \
		echo "Error: No write permission to $(SHIM_INSTALL_DIR). Please run with sudo."; \
		exit 1; \
	fi
	cp $(CURDIR)/$(SHIM_LIB) $(SHIM_INSTALL_DIR)/
	ldconfig
	@echo "Machnet shim library installed successfully."

shim: build_shim

clean: clean_shim
	rm -rf $(DEBUG_BUILD_DIR) $(RELEASE_BUILD_DIR)

clean_shim:
	@if [ -d "$(SHIM_SRC_DIR)" ]; then \
		cd $(SHIM_SRC_DIR) && make clean; \
	fi
	rm -f $(CURDIR)/$(SHIM_LIB)
