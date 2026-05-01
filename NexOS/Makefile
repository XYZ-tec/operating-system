# ============================================================
#  NexOS Makefile
#  Usage:
#    make          → build everything
#    make run      → build + run with default hardware
#    make clean    → remove all compiled binaries
#    make apps/notepad  → build one app only
# ============================================================

CXX      = g++
CXXFLAGS = -std=c++17 -Wall -O2 -Iinclude
LDFLAGS  = -lraylib -lGL -lm -lpthread -ldl -lrt -lX11

APPS = \
  apps/weather

# Default: build OS + all apps
all: NexOS $(APPS)

# Main OS binary
NexOS: os.cpp include/theme.h include/resources.h include/ipc.h
	$(CXX) $(CXXFLAGS) os.cpp -o NexOS $(LDFLAGS)
	@echo "✓ NexOS compiled"

# Each app binary (pattern rule)
apps/%: apps/%.cpp include/theme.h include/resources.h include/ipc.h
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)
	@echo "✓ $@ compiled"

# Run with default hardware: 2048MB RAM, 256GB HDD, 8 cores
run: NexOS
	./NexOS 2048 262144 8

# Clean all binaries
clean:
	rm -f NexOS $(APPS)
	@echo "✓ Cleaned"

# Create required directories if missing
setup:
	mkdir -p apps include logs hdd
	@echo "✓ Directories created"

.PHONY: all run clean setup
