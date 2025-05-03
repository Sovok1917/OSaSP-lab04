# Makefile for Producer/Consumer Demo using System V IPC

# Compiler and base flags
CC = gcc
# Base flags adhere to requirements: C11, pedantic, common warnings, POSIX.1-2008
BASE_CFLAGS = -std=c11 -pedantic -W -Wall -Wextra \
              -Wmissing-prototypes -Wstrict-prototypes \
              -D_POSIX_C_SOURCE=200809L
# Optional allowed flags (uncomment if needed during development)
# BASE_CFLAGS += -Wno-unused-parameter -Wno-unused-variable

# Linker flags (add -lm if math needed, etc.)
LDFLAGS =

# Directories
SRC_DIR = src
BUILD_DIR = build
DEBUG_DIR = $(BUILD_DIR)/debug
RELEASE_DIR = $(BUILD_DIR)/release

# --- Configuration: Default to Debug ---
CURRENT_MODE = debug
CFLAGS = $(BASE_CFLAGS) -g3 -ggdb # Debugging symbols
OUT_DIR = $(DEBUG_DIR)

# --- Configuration: Adjust for Release Mode ---
# Override defaults if MODE=release is passed via command line (e.g., make MODE=release ...)
ifeq ($(MODE), release)
  CURRENT_MODE = release
  CFLAGS = $(BASE_CFLAGS) -O2 # Optimization level 2
  CFLAGS += -Werror # Treat warnings as errors in release
  OUT_DIR = $(RELEASE_DIR)
endif

# Ensure output directories exist before compiling/linking
# Using .SECONDEXPANSION allows OUT_DIR to be evaluated correctly per target
.SECONDEXPANSION:
$(shell mkdir -p $(DEBUG_DIR) $(RELEASE_DIR))

# Source files
SRCS = $(SRC_DIR)/main.c $(SRC_DIR)/ipc_manager.c $(SRC_DIR)/producer.c $(SRC_DIR)/consumer.c $(SRC_DIR)/utils.c

# Object files (paths automatically use the correct OUT_DIR based on MODE)
OBJS = $(patsubst $(SRC_DIR)/%.c, $(OUT_DIR)/%.o, $(SRCS))

# Executable (paths automatically use the correct OUT_DIR based on MODE)
TARGET_NAME = prod_cons_ipc
TARGET = $(OUT_DIR)/$(TARGET_NAME)


# Phony targets (targets that don't represent files)
.PHONY: all clean run run-release debug-build release-build help

# Default target: build debug version
all: debug-build

# Help target - Explains usage and run targets clearly
help:
	@echo "Makefile Usage:"
	@echo "  make                Build debug version (default, same as make debug-build)"
	@echo "  make debug-build    Build debug version into $(DEBUG_DIR)"
	@echo "  make release-build  Build release version into $(RELEASE_DIR)"
	@echo "                      (Warnings will be treated as errors: CFLAGS += -Werror)"
	@echo "  make run            Build and run DEBUG version."
	@echo "  make run-release    Build and run RELEASE version."
	@echo "  make clean          Remove all build artifacts (rm -rf $(BUILD_DIR))"
	@echo "  make help           Show this help message"


# --- Build Targets ---

# Target to build the debug version
# Sets MODE=debug explicitly for dependencies and ensures correct OUT_DIR
debug-build: MODE=debug
debug-build: $$(TARGET) # Use $$ to delay expansion until this rule runs
	@echo "Debug build complete in $(DEBUG_DIR)"

# Target to build the release version
# Sets MODE=release explicitly for dependencies and ensures correct OUT_DIR
release-build: MODE=release
release-build: $$(TARGET) # Use $$ to delay expansion until this rule runs
	@echo "Release build complete in $(RELEASE_DIR)"


# --- Compilation and Linking Rules ---

# Link object files to create the executable
# Depends on the specific object files in the correct OUT_DIR
$(TARGET): $(OBJS)
	@echo "Linking $@..."
	$(CC) $(CFLAGS) $(OBJS) -o $@ $(LDFLAGS)

# Compile source files into object files (Pattern Rule)
# Places object files in the correct OUT_DIR based on the MODE set by the build target
# Depends on the source file and ensures the output directory exists
$(OUT_DIR)/%.o: $(SRC_DIR)/%.c | $$(@D)/.
	@echo "Compiling $< -> $@..."
	$(CC) $(CFLAGS) -c $< -o $@

# Rule to ensure the output directory exists before trying to put files in it
# Used as an order-only prerequisite | $$(@D)/. in the compile rule
%/.:
	@mkdir -p $(@)


# --- Execution Targets ---

# Run the debug version (depends on debug-build)
run: debug-build
	@echo "Running DEBUG version $(TARGET)..."
	$(TARGET)

# Run the release version (depends on release-build)
run-release: release-build
	@echo "Running RELEASE version $(TARGET)..."
	$(TARGET)


# --- Clean Target ---

# Clean up all build artifacts
clean:
	@echo "Cleaning build directories..."
	rm -rf $(BUILD_DIR)
