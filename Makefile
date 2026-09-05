PRESET ?= linux-debug
BIN    := build/$(PRESET)/bin

# Device extensions create_device requires. `caps` reports each one so a failure to create a
# device can be attributed to the driver rather than to the build.
REQUIRED_EXTENSIONS := \
	VK_EXT_descriptor_heap \
	VK_KHR_device_address_commands \
	VK_KHR_shader_untyped_pointers \
	VK_EXT_mesh_shader \
	VK_KHR_swapchain


# Colors
BOLD   := \033[1m
RESET  := \033[0m
GREEN  := \033[32m
CYAN   := \033[36m
YELLOW := \033[33m


.DEFAULT_GOAL := help

## help: Show available commands
.PHONY: help
help:
	@printf "\n$(BOLD)Available commands:$(RESET)\n\n"
	@grep -E '^## ' Makefile | \
		sed -E 's/^## ([^:]+):[[:space:]]*(.*)/\1:\2/' | \
		awk -F: '{printf "$(CYAN)%-15s$(RESET) %s\n", $$1, $$2}'
	@printf "\n"

## all: build
.PHONY: all
all: build

## build: build with PRESET
.PHONY: build
build: | build/$(PRESET)
	cmake --build build/$(PRESET)

.PHONY: build/$(PRESET)
build/$(PRESET):
	cmake --preset $(PRESET)

## caps: report whether this machine's driver exposes the required extensions
.PHONY: caps
caps:
	@command -v vulkaninfo >/dev/null || { printf "$(YELLOW)vulkaninfo not found; install vulkan-tools$(RESET)\n"; exit 1; }
	@reported=$$(vulkaninfo 2>/dev/null | grep -oE '^[[:space:]]+VK_[A-Za-z0-9_]+' | tr -d ' \t' | sort -u); \
	for extension in $(REQUIRED_EXTENSIONS); do \
		if printf '%s\n' "$$reported" | grep -qx "$$extension"; then \
			printf "$(GREEN)%-34s present$(RESET)\n" "$$extension"; \
		else \
			printf "$(YELLOW)%-34s MISSING$(RESET)\n" "$$extension"; \
		fi; \
	done

## clean: remove all files in the build directory
.PHONY: clean
clean:
	rm -rf build/$(PRESET)

## configure: configure the C++ project
.PHONY: configure
configure:
	cmake --preset $(PRESET)

## rebuild: clean, build
.PHONY: rebuild
rebuild: clean build

## run: print this machine's device capabilities
.PHONY: run
run: build
	$(BIN)/example_device_info

## test: build and run the test suite
.PHONY: test
test: build
	ctest --test-dir build/$(PRESET) --output-on-failure
