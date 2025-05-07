# Makefile for Parent/Child Process Interaction Demo

# Compiler and base flags
CC = gcc
# Base flags adhere to requirements: C11, pedantic, common warnings, POSIX.1-2008
# Add stricter prototype checks. Remove _GNU_SOURCE unless proven necessary.
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

# Source files for each program
PARENT_SRC = $(SRC_DIR)/parent.c
CHILD_SRC = $(SRC_DIR)/child.c

# Object files (paths automatically use the correct OUT_DIR based on MODE)
PARENT_OBJ = $(OUT_DIR)/parent.o
CHILD_OBJ = $(OUT_DIR)/child.o

# Executables (paths automatically use the correct OUT_DIR based on MODE)
PARENT_PROG = $(OUT_DIR)/parent
CHILD_PROG = $(OUT_DIR)/child


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
	@echo "                      Sets CHILD_PATH environment variable for the parent process,"
	@echo "                      so it can find the child executable in '$(DEBUG_DIR)'."
	@echo "  make run-release    Build and run RELEASE version."
	@echo "                      Sets CHILD_PATH environment variable for the parent process,"
	@echo "                      so it can find the child executable in '$(RELEASE_DIR)'."
	@echo "  make clean          Remove all build artifacts (rm -rf $(BUILD_DIR))"
	@echo "  make help           Show this help message"


# --- Build Targets ---

# Target to build the debug version
# Sets MODE=debug explicitly for dependencies and ensures correct OUT_DIR
debug-build: MODE=debug
debug-build: $$(PARENT_PROG) $$(CHILD_PROG) # Use $$ to delay expansion until this rule runs
	@echo "Debug build complete in $(DEBUG_DIR)"

# Target to build the release version
# Sets MODE=release explicitly for dependencies and ensures correct OUT_DIR
release-build: MODE=release
release-build: $$(PARENT_PROG) $$(CHILD_PROG) # Use $$ to delay expansion until this rule runs
	@echo "Release build complete in $(RELEASE_DIR)"


# --- Compilation and Linking Rules ---

# Link parent object file to create the parent executable
# Depends on the specific object file in the correct OUT_DIR
$(PARENT_PROG): $(PARENT_OBJ)
	@echo "Linking $@..."
	$(CC) $(CFLAGS) $(PARENT_OBJ) -o $@ $(LDFLAGS)

# Link child object file to create the child executable
# Depends on the specific object file in the correct OUT_DIR
$(CHILD_PROG): $(CHILD_OBJ)
	@echo "Linking $@..."
	$(CC) $(CFLAGS) $(CHILD_OBJ) -o $@ $(LDFLAGS)

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

# Run the debug version (depends on debug-build, sets CHILD_PATH using env)
run: debug-build
	@echo "Running DEBUG version $(PARENT_PROG)..."
	@echo " CHILD_PATH will be set to: '$(abspath $(DEBUG_DIR))'"
	@# Use env to set CHILD_PATH for the parent process execution.
	@# Use abspath to ensure the parent gets a full, unambiguous path.
	env CHILD_PATH='$(abspath $(DEBUG_DIR))' $(PARENT_PROG)

# Run the release version (depends on release-build, sets CHILD_PATH using env)
run-release: release-build
	@echo "Running RELEASE version $(PARENT_PROG)..."
	@echo " CHILD_PATH will be set to: '$(abspath $(RELEASE_DIR))'"
	env CHILD_PATH='$(abspath $(RELEASE_DIR))' $(PARENT_PROG)


# --- Clean Target ---

# Clean up all build artifacts
clean:
	@echo "Cleaning build directories..."
	rm -rf $(BUILD_DIR)
