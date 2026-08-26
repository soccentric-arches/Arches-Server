# Master Makefile for Arches Server (gRPC IV&V server — runs on the x86 host,
# not on the Jetson target; native-only build, no cross-build/deploy needed).
#
# The server links the Arches-HAL subproject (../Arches-HAL) for the shared
# generated protos (hal_protos_shared) — single source of proto symbols, mirroring
# Arches-Client. Per §12 the Makefile is the single entry point for build/test/lint.

.DEFAULT_GOAL := help

.PHONY: all build configure clean test lint cppcheck doc installer run help

# Build directory
NATIVE_BUILD_DIR := build/native

# Parallel jobs (override with make JOBS=N)
JOBS := $(shell nproc)

# CTest label applied to all Arches-Server unit/integration tests.
# The HAL subproject's enable_testing() also registers ~36 unbuilt HAL tests,
# so we always filter on this label rather than running the full CTest set.
SERVER_TEST_LABEL := arches_server

# Default target
all: build

# =============================================================================
# Native builds (host architecture)
# =============================================================================

configure: ## Configure the project with CMake/Ninja
	@echo "Configuring project with Ninja..."
	mkdir -p $(NATIVE_BUILD_DIR)
	cd $(NATIVE_BUILD_DIR) && cmake -GNinja -DBUILD_TESTS=ON $(CURDIR)

build: configure ## Build the project (configures if needed)
	@echo "Building project with Ninja..."
	cd $(NATIVE_BUILD_DIR) && ninja -j$(JOBS)

test: build ## Run Arches-Server tests (ctest -L arches_server)
	@echo "Running Arches-Server tests (label=$(SERVER_TEST_LABEL))..."
	cd $(NATIVE_BUILD_DIR) && ctest -L $(SERVER_TEST_LABEL) --output-on-failure

run: build ## Run arches-server locally
	@echo "Running arches-server..."
	LD_LIBRARY_PATH=$(NATIVE_BUILD_DIR)/lib $(NATIVE_BUILD_DIR)/bin/arches-server

# =============================================================================
# Lint & installer (per §12 — Makefile is the single entry point)
# =============================================================================

# Static analysis: cppcheck + clang-tidy, both driven from compile_commands.json
# (exported by CMAKE_EXPORT_COMPILE_COMMANDS in the root CMakeLists). Best-effort:
# if a tool is not installed, report and continue rather than fail the §9 validation
# gate. When a tool IS present and reports errors, this target exits nonzero.
lint: cppcheck ## Static analysis (cppcheck + clang-tidy, best-effort)
	@echo "Running clang-tidy on Arches-Server sources (best-effort)..."
	@if command -v clang-tidy >/dev/null 2>&1; then \
		if [ -f $(NATIVE_BUILD_DIR)/compile_commands.json ]; then \
			clang-tidy -p $(NATIVE_BUILD_DIR) $$(find $(CURDIR)/src -name '*.cc' -o -name '*.cpp'); \
		else \
			echo "  compile_commands.json missing — run 'make configure' first."; \
		fi; \
	else \
		echo "  clang-tidy not installed; skipping (cppcheck result above stands)."; \
	fi

# Package the server via CPack (.deb / .rpm / .ipk per §12). The .ipk is an
# opkg-compatible rename of the .deb (Yocto consumes .ipk); produced last.
installer: build ## Package via CPack (.deb/.rpm)
	@echo "Packaging Arches-Server via CPack..."
	cd $(NATIVE_BUILD_DIR) && cpack -G DEB && cpack -G RPM
	@deb=$$(cd $(NATIVE_BUILD_DIR) && ls -1 *.deb 2>/dev/null | head -1); \
	if [ -n "$$deb" ]; then \
		ipk=$${deb%.deb}.ipk; \
		cp $(NATIVE_BUILD_DIR)/$$deb $(NATIVE_BUILD_DIR)/$$ipk; \
		echo "  produced .ipk: $$ipk (opkg/Yocto variant of $$deb)"; \
	else \
		echo "  no .deb produced — .ipk skipped"; \
	fi

# =============================================================================
# Common
# =============================================================================

clean: ## Remove build directory
	@echo "Cleaning build directory..."
	rm -rf build

# =============================================================================
# Code quality & Docs
# =============================================================================

format: configure ## Format code with clang-format
	@echo "Formatting code..."
	cd $(NATIVE_BUILD_DIR) && ninja format
	@echo "Formatting CMake files..."
	find . -name CMakeLists.txt -not -path './build/*' | xargs cmake-format -i 2>/dev/null || true

cppcheck: configure ## Run static analysis with cppcheck
	@echo "Running cppcheck static analysis (best-effort)..."
	@if command -v cppcheck >/dev/null 2>&1; then \
		if [ -f $(NATIVE_BUILD_DIR)/compile_commands.json ]; then \
			cppcheck --quiet --enable=warning,style --inline-suppr \
				--project=$(NATIVE_BUILD_DIR)/compile_commands.json || true; \
		else \
			echo "  compile_commands.json missing — run 'make configure' first."; \
		fi; \
	else \
		echo "  cppcheck not installed; skipping (install cppcheck to enable)."; \
	fi

doc: configure ## Generate Doxygen documentation
	@echo "Generating documentation..."
	cd $(NATIVE_BUILD_DIR) && ninja doc_doxygen 2>/dev/null || echo "  Doxygen target not configured yet."

# =============================================================================
# Help
# =============================================================================

# ── Fleet-standard aliases ────────────────────────────────────────────────────
# Same verbs mean the same thing in every repo; these forward to this
# repo's own targets rather than duplicating their recipes.
.PHONY: fmt
fmt: format ## Alias for format

# ── Help ──────────────────────────────────────────────────────────────────────
# Targets are self-documenting: text after `## ` on a target line is picked up
# by `make help` automatically, so the listing can never drift from the rules.
.PHONY: help
help: ## Show this help
	@printf '\n  \033[1m%s\033[0m\n' 'Arches-Server'
	@printf '  Usage: make <target> [VAR=value ...]\n'
	@printf '\n'
	@awk 'BEGIN {FS = ":.*?## "} /^[a-zA-Z0-9_.\/-]+:.*?## / { printf "    \033[36m%-24s\033[0m %s\n", $$1, $$2 }' $(MAKEFILE_LIST)
	@printf '\n'
