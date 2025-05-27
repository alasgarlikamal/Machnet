# This makefile is primarily used for building docker containers

SHELL=/bin/bash -e -o pipefail

BUILD_COMMAND=docker buildx bake -f docker-bake.hcl
GET_BUILDX_INFO_COMMAND=$(BUILD_COMMAND) --print
BUILD_TARGETS_COMMAND=xargs $(BUILD_COMMAND)
GET_TARGETS_FOR_ARCH_CMD=python3 $(CURDIR)/dockerfiles/get_targets_for_arch.py

# By default, load into the local docker registry, can be overriden with --push for production builds
BUILD_COMMAND_EXTRA_ARGS=--load

# Build directories
DEBUG_BUILD_DIR=debug_build
RELEASE_BUILD_DIR=release_build

# Source directories to track
SRC_DIRS=src dockerfiles examples
SRC_FILES=$(shell find $(SRC_DIRS) -type f -name "*.c" -o -name "*.cpp" -o -name "*.h" -o -name "*.hpp" -o -name "CMakeLists.txt" -o -name "*.cmake")

# Build artifacts
DEBUG_BINARY=$(DEBUG_BUILD_DIR)/src/apps/machnet/machnet
RELEASE_BINARY=$(RELEASE_BUILD_DIR)/src/apps/machnet/machnet

# Hugepage settings
HUGEPAGE_SIZE=2048
HUGEPAGE_COUNT=1024
HUGEPAGE_PATH=/sys/devices/system/node/node*/hugepages/hugepages-$(HUGEPAGE_SIZE)kB/nr_hugepages

# Config file
CONFIG_FILE?=src/apps/machnet/config.json

.PHONY: all_containers x86_containers arm_containers debug release clean run_machnet setup_hugepages

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

$(DEBUG_BINARY): $(DEBUG_BUILD_DIR)/CMakeCache.txt
	cd $(DEBUG_BUILD_DIR) && ninja

# Release build rules
$(RELEASE_BUILD_DIR)/CMakeCache.txt: $(SRC_FILES)
	@mkdir -p $(RELEASE_BUILD_DIR)
	cd $(RELEASE_BUILD_DIR) && cmake -DCMAKE_BUILD_TYPE=Release -GNinja ..

$(RELEASE_BINARY): $(RELEASE_BUILD_DIR)/CMakeCache.txt
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

clean:
	rm -rf $(DEBUG_BUILD_DIR) $(RELEASE_BUILD_DIR)