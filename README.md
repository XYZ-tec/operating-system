# NexOS
A simulated desktop operating system built in C++. NexOS demonstrates core OS concepts — process management, resource allocation, IPC, scheduling, and deadlock detection — through a fully interactive graphical environment powered by [raylib](https://www.raylib.com/).
---
## Table of Contents
- [Overview](#overview)
- [Features](#features)
- [Architecture](#architecture)
- [Project Structure](#project-structure)
- [Prerequisites](#prerequisites)
- [Build Instructions](#build-instructions)
- [Running NexOS](#running-nexos)
- [Built-in Applications](#built-in-applications)
- [Kernel Mode](#kernel-mode)
- [IPC System](#ipc-system)
- [Adding a New App](#adding-a-new-app)
- [Logging](#logging)
---
## Overview
NexOS launches as a standard Linux process and presents a full cyberpunk-themed desktop. Each application is a separate child process; the OS kernel forks and exec's them on demand, manages their RAM and HDD quotas, schedules them across virtual CPU cores, and tears them down cleanly on exit or shutdown.
---
## Features
### Desktop Environment
- **Hardware Configuration Screen** — configure virtual RAM (MB), HDD (MB), and CPU core count before boot
- **Left Dock** — quick-launch sidebar with app icons and running indicators
- **Bottom Taskbar** — NexOS logo, live search bar, running-app pills, RAM/HDD progress bars, real-time clock, and Kernel Mode badge
- **App Search Overlay** — press the search button or type to filter and launch any app instantly
- **Cyberpunk grid background** with neon glow UI elements
### Process & Resource Management
- **Resource Manager Thread** — receives app resource requests via POSIX message queue; grants or denies based on available RAM and HDD
- **Multi-Level Queue Scheduler** — promotes ready processes to running state across virtual CPU cores, respecting priority and queue level (system vs. user)
- **Priority Aging Thread** — automatically promotes long-waiting processes to prevent starvation (every 5 s; threshold 10 s)
- **Deadlock Detector Thread** — flags a deadlock when ≥ 2 processes are blocked and RAM utilisation exceeds 90 % (checks every 10 s)
- **Process Control Block (PCB)** table shared in POSIX shared memory — up to 32 concurrent processes
### App Lifecycle
- Each app is **fork + exec'd** as a child process
- Apps communicate with the OS via **POSIX shared memory** and **message queues**
- Minimize/restore uses **SIGSTOP / SIGCONT**
- Termination uses **SIGTERM**; the OS reaps children with `waitpid`
---
## Architecture
```
┌─────────────────────────────────────────────────┐
│                   NexOS (os.cpp)                │
│                                                 │
│  ┌───────────────┐  ┌──────────────────────┐   │
│  │  Desktop UI   │  │  Background Threads  │   │
│  │  (raylib)     │  │  ResourceManager     │   │
│  │               │  │  Scheduler           │   │
│  │  Dock         │  │  AgingThread         │   │
│  │  Taskbar      │  │  DeadlockDetector    │   │
│  │  Search       │  └──────────────────────┘   │
│  │  KernelMode   │                             │
│  └───────────────┘                             │
│          │  fork/exec                          │
│          ▼                                     │
│  ┌─────────────────────────────────────────┐   │
│  │           Child App Processes           │   │
│  │  (Paint, Notepad, Shell, Tetris, …)     │   │
│  └─────────────────────────────────────────┘   │
│          │                                     │
│  ┌───────┴──────────────────────────────────┐  │
│  │         Shared IPC Layer                 │  │
│  │  POSIX Shared Memory  (SHM_KEY 0x4E584F53)│  │
│  │  POSIX Message Queue  (MSG_KEY 0x4E584F54)│  │
│  │  POSIX Semaphore      (/nexos_shm_sem)   │  │
│  └──────────────────────────────────────────┘  │
└─────────────────────────────────────────────────┘
```
### Key Header Files
| File | Purpose |
|------|---------|
| `include/resources.h` | `PCB`, `OSResources`, `ResourceRequest`, `ResourceReply` structs; process state and priority constants |
| `include/ipc.h` | Helper functions every app uses: `GetSharedResources()`, `RequestResources()`, `ReleaseResources()` |
| `include/theme.h` | All colors, font sizes, and inline UI helpers (`DrawGlowRect`, `DrawButton`, `DrawProgressBar`, `DrawCyberpunkGrid`) |
---
## Project Structure
```
operating-system/
└── NexOS/
    ├── os.cpp               # Main OS process: desktop, threads, IPC setup
    ├── Makefile             # Build system
    ├── .env                 # Environment / log reference
    ├── include/
    │   ├── resources.h      # Shared data structures
    │   ├── ipc.h            # IPC helpers for apps
    │   └── theme.h          # UI theme constants and drawing helpers
    ├── apps/
    │   ├── app_template.cpp # Starter template for new apps
    │   ├── alarm.cpp
    │   ├── brickbreaker.cpp
    │   ├── calculator.cpp
    │   ├── calendar.cpp
    │   ├── chat.cpp
    │   ├── clock.cpp
    │   ├── file_manager.cpp
    │   ├── nexos_shell.cpp
    │   ├── notepad.cpp
    │   ├── paint.cpp
    │   ├── songplayer.cpp
    │   ├── tetris.cpp
    │   ├── visualization.cpp
    │   └── weather.cpp
    ├── assets/
    │   ├── icons/           # PNG icons for each app
    │   └── songs/           # Audio files for Song Player (mp3/ogg/wav)
    └── hdd/                 # Persistent app state (alarm, calendar, clock saves)
```
---
## Prerequisites
| Dependency | Notes |
|------------|-------|
| `g++` ≥ 9 | C++17 support required |
| `raylib` | Graphics, audio, and input library |
| `libGL`, `libX11` | Standard on Linux desktop systems |
| `libpthread`, `libdl`, `librt`, `libm` | Standard POSIX/math libraries |
**Install raylib on Ubuntu/Debian:**
```bash
sudo apt install libraylib-dev
# or build from source: https://github.com/raysan5/raylib
```
---
## Build Instructions
```bash
cd NexOS
# Create required directories (first time only)
make setup
# Build the OS and all apps
make
# Build a single app
make apps/notepad
make apps/paint
```
The compiler invocation is:
```
g++ -std=c++17 -Wall -O2 -Iinclude <source.cpp> -o <binary> \
    -lraylib -lGL -lm -lpthread -ldl -lrt -lX11
```
---
## Running NexOS
```bash
# Default: 2048 MB RAM, 262144 MB HDD, 8 CPU cores
make run
# Custom hardware
./NexOS <RAM_MB> <HDD_MB> <CPU_cores>
# Example: 4096 MB RAM, 512 GB HDD, 16 cores
./NexOS 4096 524288 16
```
On startup, a **Hardware Configuration Screen** is shown where you can set or confirm these values before the desktop loads. Valid ranges:
| Parameter | Min | Max |
|-----------|-----|-----|
| RAM | 256 MB | 65 536 MB |
| HDD | 1 024 MB | 1 048 576 MB |
| CPU Cores | 1 | 64 |
---
## Built-in Applications
| App | RAM | HDD | Priority | Description |
|-----|-----|-----|----------|-------------|
| **Paint** | 90 MB | 50 MB | Normal | Pixel art and drawing tool. Tools: pencil, eraser, fill, line, rectangle, ellipse, eyedropper. 32-color palette, custom color picker, up to 4 layers, 20-level undo, canvas resize, zoom/pan, save to `hdd/` |
| **Calculator** | 30 MB | 5 MB | Normal | Standard and scientific calculator with expression evaluation |
| **Notepad** | 50 MB | 10 MB | Normal | Text editor with file open/save |
| **Tetris** | 45 MB | 5 MB | Low | Classic Tetris with cyberpunk skin (10 × 20 board, all 7 tetrominoes) |
| **Brick Breaker** | 70 MB | 10 MB | Low | Brick breaker with 3 lives, power-ups (wide paddle, multi-ball, slow ball, laser, sticky), and procedural audio |
| **Chat** | 50 MB | 10 MB | Normal | LAN-style chat application |
| **Shell** | 60 MB | 10 MB | Normal | Terminal emulator with cyberpunk styling; runs shell commands as child processes |
| **Song Player** | 40 MB | 20 MB | Normal | Audio player reading from `assets/songs/`. Supports mp3/ogg/wav, cover art, progress seek, volume, shuffle, repeat, and animated visualizer |
| **Alarm** | 20 MB | 1 MB | **High** | Alarm clock with day-of-week scheduling; state persisted to `hdd/alarm_state.txt` |
| **Weather** | 30 MB | 5 MB | Normal | Simulated weather with multi-city support, 7-day forecast, hourly breakdown, and animated visuals |
| **File Manager** | 60 MB | 30 MB | Normal | File browser with sidebar panel, toolbar, status bar, and file preview |
| **Calendar** | 20 MB | 1 MB | **High** | Monthly calendar view with event creation; state persisted to `hdd/calendar_state.txt` |
| **Clock** | 20 MB | 1 MB | Normal | Analog/digital clock; state persisted to `hdd/clock_state.txt` |
| **Visualization** | — | — | — | Kernel process visualizer; launched from the Kernel Mode panel |
> Apps with **High** priority (Alarm, Calendar) are placed in the system queue (queue level 0) and are scheduled before user-level apps.
---
## Kernel Mode
Access the Kernel Mode panel by clicking the **KERNEL** button on the taskbar.
- **Password:** `abcd123`
- Shows a live PCB table with PID, name, state, priority, RAM, and HDD for every running process
- Live **RAM** and **CPU utilization** mini-graphs (sampled every second, last 30 values)
- **Launch Full View** button — opens the standalone `apps/visualization` kernel visualizer
- Deadlock alert displayed when the OS detects a potential deadlock
When Kernel Mode is active the taskbar displays a purple **KERNEL** badge.
---
## IPC System
All inter-process communication uses standard POSIX primitives:
### Shared Memory (`OSResources`)
- Key: `0x4E584F53` ("NXOS")
- Contains: hardware specs, current RAM/HDD/core usage, PCB table (up to 32 entries), system flags (kernel mode, shutdown, deadlock)
- Protected by the named semaphore `/nexos_shm_sem`
### Message Queue (`ResourceRequest` / `ResourceReply`)
- Key: `0x4E584F54` ("NXOT")
- App → OS: `ResourceRequest` (type 1) — app name, PID, RAM/HDD needed, priority, queue level
- OS → App: `ResourceReply` (type = requesting PID) — `granted` flag and reason string
### App IPC Flow
```
App start:
  RequestResources(name, ramMB, hddMB, priority, queueLevel)
    → msgsnd(type=1) ──► ResourceManagerThread
    ◄── msgrcv(type=pid)  reply.granted ?
App exit:
  ReleaseResources(name, ramMB, hddMB)
    → directly updates OSResources via shared memory (semaphore-protected)
    → removes PCB entry
```
---
## Adding a New App
1. Copy the template:
   ```bash
   cp apps/app_template.cpp apps/myapp.cpp
   ```
2. Edit `apps/myapp.cpp`:
   - Set `APP_NAME`, `RAM_MB`, `HDD_MB`, `WIN_W`, `WIN_H`
   - Implement `DrawAppContent(Rectangle content)` with your app logic
3. Register it in `os.cpp` — add an entry to the `APPS[]` array:
   ```cpp
   { "My App", "apps/myapp", RAM_MB, HDD_MB, PRIORITY_NORMAL, 1, NEON_CYAN, "assets/icons/myapp.png" }
   ```
   Update `APP_COUNT` accordingly.
4. Add the app to `Makefile`'s `APPS` list:
   ```makefile
   APPS = \
     ...
     apps/myapp
   ```
5. Build:
   ```bash
   make apps/myapp
   make          # rebuild OS to include the new app entry
   ```
The `RequestResources` / `ReleaseResources` calls in `main()` are already handled by the template — the OS will automatically track your app's RAM and HDD usage.
---
## Logging
The OS writes a timestamped log to `logs/nexos.log`. Log entries include:
- Boot / shutdown events with hardware configuration
- App launch (PID, RAM/HDD granted)
- App exit (PID)
- Resource grant/deny decisions
- Priority aging events
- Kernel Mode lock/unlock
- Deadlock detection events
Logging can be toggled via the `logging_enabled` flag in `OSResources`. Log output format:
```
[HH:MM:SS] <message>
```
