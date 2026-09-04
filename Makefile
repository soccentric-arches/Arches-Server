# Makefile for Arches-Server
# x86 (Docker) only. The Docker image is built once via `make docker`.

.DEFAULT_GOAL := help

.PHONY: build test clean docker shell fmt format lint help

# Parallel jobs (override with make JOBS=N)
JOBS := $(shell nproc)

DOCKER_IMAGE := arches-dev

# Mount the *parent* Arches/ directory so sibling repos (e.g. Arches-HAL) are
# visible inside the container at /workspace.
Arches_ROOT := $(shell dirname $(realpath $(CURDIR)))
DOCKER_SRC_DIR := /workspace/Arches-Server
BUILD_DIR      := build/docker

DOCKER_RUN := docker run --rm -t --init --platform linux/amd64 \
	-v $(Arches_ROOT):/workspace \
	-v ccache-arches-server:/home/arches/.ccache \
	$(DOCKER_IMAGE) bash -c

# =============================================================================
# Docker image (x86)
# =============================================================================

docker: ## Build the Docker image (x86) if it does not exist
	@if ! docker image inspect $(DOCKER_IMAGE) >/dev/null 2>&1; then \
		echo "Building Docker image $(DOCKER_IMAGE) (x86 Ubuntu)..."; \
		make -C docker build-server IMAGE_NAME=$(DOCKER_IMAGE); \
	else \
		echo "Docker image $(DOCKER_IMAGE) already exists."; \
	fi

# =============================================================================
# Build & test (Docker, x86)
# =============================================================================

build: docker ## Build (x86, in Docker)
	@echo "Building in Docker (x86)..."
	$(DOCKER_RUN) "if [ ! -f $(DOCKER_SRC_DIR)/$(BUILD_DIR)/build.ninja ]; then \
		mkdir -p $(DOCKER_SRC_DIR)/$(BUILD_DIR) && \
		cd $(DOCKER_SRC_DIR)/$(BUILD_DIR) && \
		cmake -GNinja -DBUILD_TESTS=ON $(DOCKER_SRC_DIR); \
	fi && cd $(DOCKER_SRC_DIR)/$(BUILD_DIR) && ninja -j$(JOBS)"

test: build ## Run unit tests (x86, in Docker)
	@echo "Running unit tests in Docker (x86)..."
	$(DOCKER_RUN) "cd $(DOCKER_SRC_DIR)/$(BUILD_DIR) && ctest -L arches_server --output-on-failure"

shell: docker ## Open a shell in the build container (x86)
	@echo "Starting Docker shell (x86)..."
	docker run -it --rm --platform linux/amd64 \
		-v $(Arches_ROOT):/workspace \
		-v ccache-arches-server:/home/arches/.ccache \
		$(DOCKER_IMAGE) bash

clean: ## Remove the build directory
	@echo "Cleaning build directory..."
	rm -rf build

# =============================================================================
# Code quality (host tools; run `make build` first so compile_commands.json
# exists — the x86 image has no cppcheck/clang-format, the host does)
# =============================================================================

format: ## Run clang-format on src/ and tests/
	@echo "Running clang-format..."
	@find src tests -type f \( -name '*.cc' -o -name '*.h' \) -print0 | \
		xargs -0 clang-format -i --style=file

fmt: format ## Alias for format

lint: ## Run cppcheck static analysis (uses compile_commands.json)
	@if [ ! -f $(BUILD_DIR)/compile_commands.json ]; then \
		echo "No $(BUILD_DIR)/compile_commands.json — run 'make build' first."; \
		exit 1; \
	fi
	@echo "Running cppcheck..."
	cppcheck --project=$(BUILD_DIR)/compile_commands.json \
		--enable=warning,style,performance,portability --inline-suppr \
		-j$(JOBS) 2>&1 | tee cppcheck.log

# =============================================================================
# Help
# =============================================================================

help: ## Show this help
	@printf '\n  \033[1m%s\033[0m\n' 'Arches-Server (x86, Docker)'
	@printf '  Usage: make <target>\n\n'
	@awk 'BEGIN {FS = ":.*?## "} /^[a-zA-Z0-9_.\/-]+:.*?## / { printf "    \033[36m%-24s\033[0m %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@printf '\n'