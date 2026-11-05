# SPDX-License-Identifier: GPL-2.0-or-later
# Copyright (C) Opinsys Oy 2026

CMAKE ?= cmake
BUILDDIR ?= build

.PHONY: default help all setup check ci format package container-build publish clean

default: help

help: ## Show this help
	@awk -F ':[^#]*## ?' '/^[a-z_-]+:[^#]*##/{printf "  make %-20s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

setup: ## Configure the build directory
	$(CMAKE) -S . -B $(BUILDDIR)

all: setup ## Build the project
	$(CMAKE) --build $(BUILDDIR)

check: all ## Run checks

ci: ## Run the local CI script
	./scripts/ci.sh

format: ## Reformat the sources in place
	find src -name '*.cc' -o -name '*.h' | xargs clang-format -i

package: ## Package multi-architecture AppImages and npm binaries
	./scripts/package-all.sh

container-build: ## Build OCI container image
	CONTAINER="$(CONTAINER)" ./scripts/build.sh

publish: ## Publish a GitHub release
	./scripts/publish.sh "$(VERSION)"

clean: ## Remove build artifacts
	$(RM) -r $(BUILDDIR)
