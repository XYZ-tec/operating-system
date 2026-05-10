# ============================================================
#  RayVerve Makefile
#  Usage:
#    make          -> build everything
#    make run      -> build + run with default hardware
#    make clean    -> remove all compiled binaries
#    make apps/notepad  -> build one app only
# ============================================================

CXX      = g++
CXXFLAGS = -std=c++17 -Wall -O2 -Iinclude
LDFLAGS  = -lraylib -lGL -lm -lpthread -ldl -lrt -lX11

APPS = \
  apps/alarm \
  apps/app_template \
  apps/brickbreaker \
  apps/browser \
  apps/calculator \
  apps/calendar \
  apps/chat \
  apps/clock \
  apps/file_manager \
  apps/nexos_shell \
  apps/notepad \
  apps/paint \
  apps/songplayer \
  apps/tetris \
  apps/visualization \
  apps/weather

# Default: build OS + all apps
all: RayVerve $(APPS)

# Main OS binary
RayVerve: os.cpp include/theme.h include/resources.h include/ipc.h
	$(CXX) $(CXXFLAGS) os.cpp -o RayVerve $(LDFLAGS)
	@echo "[OK] RayVerve compiled"

# Each app binary (pattern rule)
apps/%: apps/%.cpp include/theme.h include/resources.h include/ipc.h
	$(CXX) $(CXXFLAGS) $< -o $@ $(LDFLAGS)
	@echo "[OK] $@ compiled"

# Run with default hardware: 2048MB RAM, 256GB HDD, 8 cores
run: RayVerve
	./RayVerve 2048 262144 8

# Clean all binaries
clean:
	rm -f RayVerve $(APPS)
	@echo "[OK] Cleaned"

# Create required directories if missing
setup:
	mkdir -p apps include logs hdd
	@echo "[OK] Directories created"

.PHONY: all run clean setup
