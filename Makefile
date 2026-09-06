# Makefile for Arches-Server
# x86 (Docker) only. The Docker image is built once via `make docker`.

.DEFAULT_GOAL := help

.PHONY: build test run clean docker shell fmt format lint cppcheck help

# Parallel jobs (override with make JOBS=N)
JOBS := $(shell nproc)

DOCKER_IMAGE := arches-dev

# Mount the *parent* Arches/ directory so sibling repos (e.g. Arches-HAL) are
# visible inside the container at /workspace.
Arches_ROOT := $(shell dirname $(realpath $(CURDIR)))
DOCKER_SRC_DIR := /workspace/Arches-Server
DOCKER_HAL_DIR := /workspace/Arches-HAL
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

run: build ## Build then run arches-server (x86, in Docker)
	@echo "Running arches-server in Docker (x86)..."
	docker run --rm -t --init --platform linux/amd64 \
		-p 50051:50051 \
		-v $(Arches_ROOT):/workspace \
		-v ccache-arches-server:/home/arches/.ccache \
		$(DOCKER_IMAGE) bash -c "cd $(DOCKER_SRC_DIR) && \
			export LD_LIBRARY_PATH=$(DOCKER_SRC_DIR)/$(BUILD_DIR)/lib:$(DOCKER_SRC_DIR)/$(BUILD_DIR)/arches_hal_build/lib && \
			mkdir -p $(BUILD_DIR)/run && \
			ARCHES_DB_PATH=$(DOCKER_SRC_DIR)/$(BUILD_DIR)/run/server.db \
			ARCHES_REGISTRY_PATH=$(DOCKER_HAL_DIR)/protos/registry/modules.yaml \
			$(DOCKER_SRC_DIR)/$(BUILD_DIR)/bin/arches-server"

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

# The gate (.claude/validate.sh) invokes `make cppcheck`, which did not exist
# here -- Make answered "No rule to make target 'cppcheck'", exit 2, and the
# gate recorded a FAIL. So Arches-Server's gate has been red for a missing
# alias rather than for anything wrong with the code. Arches-HAL spells the
# same target `cppcheck`; this repo spelled it `lint`. Both names now work.
# Gate scripts are human-owned, so the fix belongs here. See Task.md G-11.
cppcheck: lint ## Alias for lint — the name .claude/validate.sh calls

lint: ## Run cppcheck static analysis (uses compile_commands.json)
	@if [ ! -f $(BUILD_DIR)/compile_commands.json ]; then \
		echo "No $(BUILD_DIR)/compile_commands.json — run 'make build' first."; \
		exit 1; \
	fi
	@echo "Running cppcheck (remapping container paths to host)..."
	@sed -e 's|/workspace/Arches-Server|$(CURDIR)|g' \
		-e 's|/workspace/Arches-HAL|$(Arches_ROOT)/Arches-HAL|g' \
		$(BUILD_DIR)/compile_commands.json > $(BUILD_DIR)/compile_commands.host.json
	cppcheck --project=$(BUILD_DIR)/compile_commands.host.json \
		--enable=warning,style,performance,portability --inline-suppr \
		--suppress='*:*/arches_hal_build/protos/generated/*' \
		-j$(JOBS) 2>&1 | tee cppcheck.log

# =============================================================================
# Help
# =============================================================================

help: ## Show this help
	@printf '\n  \033[1m%s\033[0m\n' 'Arches-Server (x86, Docker)'
	@printf '  Usage: make <target>\n\n'
	@awk 'BEGIN {FS = ":.*?## "} /^[a-zA-Z0-9_.\/-]+:.*?## / { printf "    \033[36m%-24s\033[0m %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@printf '\n'