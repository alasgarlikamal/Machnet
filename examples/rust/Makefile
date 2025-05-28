# Rust project Makefile
# This Makefile provides common development commands for the Rust project

# Variables
CARGO = cargo
BINARY_NAME = msg_gen
BUILD_TYPE ?= release
DEBUG_BINARY = target/debug/$(BINARY_NAME)
RELEASE_BINARY = target/release/$(BINARY_NAME)

# Source files
SRC_DIR = src
SRC_FILES = $(shell find $(SRC_DIR) -name '*.rs')
CARGO_TOML = Cargo.toml

# Default parameters
LOCAL_IP ?= 10.10.1.1
LOCAL_PORT ?= 8888
REMOTE_IP ?= 10.10.1.2
REMOTE_PORT ?= 8888
MSG_SIZE ?= 64
MSG_WINDOW ?= 8
MSG_NR ?= 18446744073709551615  # u64::MAX
VERIFY ?= false

# Set binary path based on build type
ifeq ($(BUILD_TYPE),debug)
    BINARY = $(DEBUG_BINARY)
    CARGO_FLAGS = 
else
    BINARY = $(RELEASE_BINARY)
    CARGO_FLAGS = --release
endif

# Default target
.PHONY: all
all: build

# Build target with dependencies
.PHONY: build
build: $(BINARY)

# Binary targets with dependencies
$(DEBUG_BINARY): $(SRC_FILES) $(CARGO_TOML)
	$(CARGO) build
	@echo "Debug build completed: $@"

$(RELEASE_BINARY): $(SRC_FILES) $(CARGO_TOML)
	$(CARGO) build --release
	@echo "Release build completed: $@"

# Run targets
.PHONY: run_server_rust run_client_rust
run_server_rust: $(BINARY)
	$(BINARY) --local-ip $(LOCAL_IP) --local-port $(LOCAL_PORT) --msg-size $(MSG_SIZE) --msg-window $(MSG_WINDOW) --msg-nr $(MSG_NR) $(if $(VERIFY),--verify,)

run_client_rust: $(BINARY)
	$(BINARY) --local-ip $(LOCAL_IP) --local-port $(LOCAL_PORT) --remote-ip $(REMOTE_IP) --remote-port $(REMOTE_PORT) --msg-size $(MSG_SIZE) --msg-window $(MSG_WINDOW) --msg-nr $(MSG_NR) $(if $(VERIFY),--verify,)

# Development targets
.PHONY: test check clean
test:
	$(CARGO) test

check:
	$(CARGO) check
	$(CARGO) clippy

clean:
	$(CARGO) clean

# Help target
.PHONY: help
help:
	@echo "Available targets:"
	@echo "  all                  - Build $(BUILD_TYPE) version (default)"
	@echo "  build                - Build $(BUILD_TYPE) version"
	@echo "  run_server_rust      - Run in server mode ($(BUILD_TYPE))"
	@echo "  run_client_rust      - Run in client mode ($(BUILD_TYPE))"
	@echo "  test                 - Run tests"
	@echo "  check                - Run cargo check and clippy"
	@echo "  clean                - Clean build artifacts"
	@echo "  help                 - Show this help message"
	@echo ""
	@echo "Variables:"
	@echo "  BUILD_TYPE           - Set to 'debug' or 'release' (default: release)"
	@echo "                         Example: make BUILD_TYPE=debug run_server_rust"
	@echo ""
	@echo "  LOCAL_IP            - Local IP address (default: 10.10.1.1)"
	@echo "  LOCAL_PORT          - Local port number (default: 8888)"
	@echo "  REMOTE_IP           - Remote IP address (default: 10.10.1.2)"
	@echo "  REMOTE_PORT         - Remote port number (default: 8888)"
	@echo "  MSG_SIZE            - Size of messages in bytes (default: 64)"
	@echo "  MSG_WINDOW          - Max messages in flight (default: 8)"
	@echo "  MSG_NR              - Number of messages to send (default: u64::MAX)"
	@echo "  VERIFY              - Verify payload of received messages (default: false)"
	@echo ""
	@echo "Example:"
	@echo "  make run_server_rust LOCAL_IP=10.10.1.1 LOCAL_PORT=1111 MSG_SIZE=64"
	@echo "  make run_client_rust LOCAL_IP=10.10.1.2 LOCAL_PORT=1111 REMOTE_IP=10.10.1.1 REMOTE_PORT=1111 MSG_SIZE=64" 