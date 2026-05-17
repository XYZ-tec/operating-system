<div align="center">

<br/>

```
  ██████╗  █████╗ ██╗   ██╗██╗   ██╗███████╗██████╗ ██╗   ██╗███████╗
  ██╔══██╗██╔══██╗╚██╗ ██╔╝██║   ██║██╔════╝██╔══██╗██║   ██║██╔════╝
██████╔╝███████║ ╚████╔╝ ██║   ██║█████╗  ██████╔╝██║   ██║█████╗
██╔══██╗██╔══██║  ╚██╔╝  ╚██╗ ██╔╝██╔══╝  ██╔══██╗╚██╗ ██╔╝██╔══╝
  ██║  ██║██║  ██║   ██║    ╚████╔╝ ███████╗██║  ██║ ╚████╔╝ ███████╗
  ╚═╝  ╚═╝╚═╝  ╚═╝   ╚═╝     ╚═══╝  ╚══════╝╚═╝  ╚═╝  ╚═══╝  ╚══════╝
```

**A fully simulated desktop operating system — built in C++17 with raylib**

</div>

---

## What is RayVerve?

**RayVerve** is a multi-process desktop operating system simulator written in **C++17**. It uses **raylib 5.5** for all graphics rendering and real **POSIX IPC** — shared memory, message queues, and named semaphores — for communication between a live kernel and independently forked application processes.

This is not a toy abstraction. The kernel, scheduler, resource manager, and deadlock detector all run as real background POSIX threads. Each of the 13 built-in apps is a genuine `fork()` + `exec()` child process that negotiates RAM and HDD quotas from the kernel's IPC layer before it is permitted to open its window.


---

## Desktop

<div align="center">
<img width="1280" height="800" alt="image" src="https://github.com/user-attachments/assets/1b4e9d43-0db4-4bc1-9125-cef31fa9cb19" />

<br/><sub>The RayVerve desktop — 13-app dock, live RAM/HDD taskbar, real-time clock, anime wallpaper</sub>
</div>

---

## Table of Contents

- [Architecture](#architecture)
- [Kernel Internals](#kernel-internals)
- [Boot Sequence](#boot-sequence)
- [Application Showcase](#application-showcase)
- [Technology Stack](#technology-stack)
- [IPC Reference](#ipc-reference)
- [Building & Running](#building--running)
- [Project Structure](#project-structure)
- [OS Concepts Demonstrated](#os-concepts-demonstrated)
- [Adding a New App](#adding-a-new-app)

---

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                      RayVerve Kernel (os.cpp)                │
│                                                              │
│   ┌────────────────┐    ┌──────────────────────────────┐     │
│   │   Desktop UI   │    │      Background Threads      │     │
│   │   (raylib)     │    │                              │     │
│   │                │    │  ● ResourceManagerThread     │     │
│   │  ● Dock        │    │  ● SchedulerThread (500ms)   │     │
│   │  ● Taskbar     │    │  ● AgingThread     (5s)      │     │
│   │  ● Search      │    │  ● DeadlockThread  (10s)     │     │
│   │  ● KernelPanel │    │                              │     │
│   └────────────────┘    └──────────────────────────────┘     │
│              │  fork() + exec()                              │
│              ▼                                               │
│   ┌──────────────────────────────────────────────────────┐   │
│   │              Child App Processes                     │   │
│   │   paint · calculator · notepad · tetris · shell …    │   │
│   └──────────────────────────────────────────────────────┘   │
│              │                                               │
│   ┌──────────▼───────────────────────────────────────────┐   │
│   │                   Shared IPC Layer                   │   │
│   │  Shared Memory  (SHM_KEY  0x4E584F53)  — PCB table   │   │
│   │  Message Queue  (MSG_KEY  0x4E584F54)  — resource RPC│   │
│   │  Named Semaphore (/nexos_shm_sem)      — mutex       │   │
│   └──────────────────────────────────────────────────────┘   │
└──────────────────────────────────────────────────────────────┘
```

---

## Kernel Internals

### Resource Manager Thread
Listens on a POSIX message queue. When any app starts, it sends a `ResourceRequest` (app name, PID, RAM/HDD needed, priority). The kernel checks the shared `OSResources` struct under a named semaphore, grants or denies the request, and replies with a `ResourceReply`. Successful grants update the PCB table and deduct from the shared resource pool.

### Scheduler Thread — Multi-Level Queue
Runs every **500 ms**. Processes in `STATE_READY` are promoted to `STATE_RUNNING` up to the configured CPU core limit. Queue level 0 (system/high priority) is always drained before queue level 1 (user/normal apps).

### Aging Thread
Runs every **5 seconds**. Any process waiting in `STATE_READY` for more than 10 seconds has its priority boosted by one level toward `PRIORITY_HIGH`, preventing indefinite starvation.

### Deadlock Detection Thread
Runs every **10 seconds**. Raises a deadlock flag when: `blocked_processes ≥ 2` **AND** `ram_utilisation > 90%`. The alert is displayed live in the Kernel Mode panel.

### Kernel Mode Panel
Press **`K`** to open the live kernel inspector:
- Full PCB table (PID, name, state, priority, RAM, HDD, wait time)
- Real-time RAM and HDD usage bars
- Deadlock alert banner when triggered

<div align="center">
  <img width="452" height="271" alt="image" src="https://github.com/user-attachments/assets/a45f15ec-4729-4ea3-8f70-26b1cb5a5461" />

</div>

---

## Boot Sequence

<div align="center">
<img width="452" height="271" alt="image" src="https://github.com/user-attachments/assets/7550c397-296f-4432-b3c4-069bfcd41867" />

<br/><sub>Hardware Configuration — configure virtual RAM, HDD, and CPU cores before booting</sub>
</div>

At startup the user sets the virtual machine's resources. These are validated and passed to the kernel's shared memory pool, which all apps later draw from.

| Parameter | Min | Max |
|-----------|-----|-----|
| RAM | 256 MB | 65,536 MB |
| Hard Drive | 1,024 MB | 1,048,576 MB |
| CPU Cores | 1 | 64 |

---

## Application Showcase

### Shell
**RAM:** 60 MB · **Priority:** Normal

Custom terminal emulator with a Unicode block-art **RAYVERVE** splash banner (rendered using DejaVu Sans Bold with a custom codepoint range covering U+2500–U+259F). Commands: `ls`, `cat`, `echo`, `mkdir`, `rm`, `cp`, `mv`, `pwd`, `clear`, `help`. Tab-completion, command history (↑/↓), and Ctrl+L to clear.

---

### Paint
**RAM:** 90 MB · **Priority:** Normal

Full pixel-art drawing application. 640×480 canvas, 8 tools (pencil, eraser, flood fill, line, rectangle, ellipse, eyedropper, select), 32-colour palette, custom colour picker, 4 layers, 20-level undo, zoom/pan, and save to `hdd/` as BMP or PNG.

<div align="center">
<img width="452" height="271" alt="image" src="https://github.com/user-attachments/assets/597edde4-6f05-41d1-b808-e027c1990561" />

</div>

---

### Notepad
**RAM:** 50 MB · **Priority:** Normal

Code-editor-grade text editor — line numbers, blinking cursor, selection highlight, line highlight, Find/Replace (`Ctrl+F`), file open/save (`Ctrl+O` / `Ctrl+S`), multi-file (`Ctrl+N`), live word/char count, and a background auto-save `pthread` running every 60 seconds.

<div align="center">
<img width="452" height="271" alt="image" src="https://github.com/user-attachments/assets/a5aa20ec-be7e-47f6-a4d5-44d775622b67" />

</div>

---

### Calculator
**RAM:** 30 MB · **Priority:** Normal

Scientific calculator with a full expression evaluator written from scratch in C++. Supports operator precedence, parentheses, and the functions `sin`, `cos`, `tan`, `sqrt`, `log`, `abs`, `ceil`, `floor`. Toggle **Advanced** mode for function shortcut buttons.

<div align="center">
<img width="452" height="271" alt="image" src="https://github.com/user-attachments/assets/a516a74b-ab54-4982-bdc9-11653615d06e" />

</div>

---

### Tetris
**RAM:** 45 MB · **Priority:** Low

Complete Tetris — 10×20 board, ghost piece, hold piece, 3-piece next queue, level progression, line-clear scoring with Tetris bonus, and a neon cyberpunk colour scheme per tetromino type.

<div align="center">
<img width="452" height="359" alt="image" src="https://github.com/user-attachments/assets/b517c53f-9072-453e-89e6-519280e821b2" />

</div>

---

### Brick Breaker
**RAM:** 70 MB · **Priority:** Low

Arkanoid-style game with a 12×7 brick grid, star-field background, and 3 lives. Five power-up types drop from broken bricks: **Wide Paddle**, **Multi-Ball**, **Slow Ball**, **Laser**, and **Sticky Paddle**.

<div align="center">
<img width="452" height="271" alt="image" src="https://github.com/user-attachments/assets/07a605bd-a875-4984-a436-05c379f036ee" />

</div>

---

### Weather
**RAM:** 30 MB · **Priority:** Normal

Multi-city simulated weather with procedurally generated data and hand-drawn condition icons (sun, clouds, rain, storm, snow). Three tabs: **Current** (temp, feels-like, humidity, wind, UV index, visibility), **Hourly** (24 h breakdown), and **7-Day** forecast.

<div align="center">
<img width="452" height="271" alt="image" src="https://github.com/user-attachments/assets/74859da8-867c-4958-9ec1-11f751ca1d5c" />

</div>

---

### Calendar
**RAM:** 20 MB · **Priority:** High

Monthly calendar synced to real system time. Click any date to write and save a per-day note, persisted to `hdd/calendar_state.txt`. Today's date is highlighted. Navigation arrows step through months and years.

<div align="center">
<img width="452" height="271" alt="image" src="https://github.com/user-attachments/assets/b1eeecc7-4b72-447d-8692-5deef6c058b7" />

</div>

---

### Alarm / Clock
**RAM:** 20 MB · **Priority:** High

Four-tab time utility:
- **World** — live digital clock with configurable world time zones
- **Alarms** — set alarms with label, hour/minute, per-weekday repeat bitmask, snooze duration
- **Stopwatch** — start/stop/lap with full lap history
- **Timer** — configurable countdown

<div align="center">
<img width="452" height="271" alt="image" src="https://github.com/user-attachments/assets/6a059900-07c8-422c-a203-6f3f5996a8d1" />

</div>

---

### Song Player
**RAM:** 40 MB · **Priority:** Normal

Music player that scans `assets/songs/` for MP3, OGG, and WAV files. Displays album art (loads matching JPG/PNG cover), title, and artist. Controls: play/pause, prev/next, volume slider, shuffle, repeat. A live **38-bar frequency visualiser** pulses frame-by-frame with the audio output.

<div align="center">
<img width="452" height="271" alt="image" src="https://github.com/user-attachments/assets/f37096e0-d753-4f77-b239-d4a123814dbf" />

</div>

---

### File Manager
**RAM:** 60 MB · **Priority:** Normal

Full file manager for the virtual `hdd/` filesystem. Folder tree on the left, detail list (name, size, modified date, type) on the right. Operations: New File, New Folder, Copy, Cut, Paste, Rename, Delete, Info. Text file preview pane at the bottom.

<div align="center">
<img width="452" height="271" alt="image" src="https://github.com/user-attachments/assets/4ea07231-6b08-4f0c-a810-c78e202d610d" />

</div>

---

### Browser Launcher
**RAM:** 150 MB · **Priority:** Normal

Detects installed system browsers via `which` checks (Chromium, Chrome, Firefox, Brave, Edge, Opera, Vivaldi, Epiphany). Provides a URL bar, quick-access bookmarks (Google, YouTube, GitHub, Wikipedia, Reddit, Stack Overflow, HackerNews, DuckDuckGo), and a launch history log saved to `hdd/browser_history.txt`. Launches the detected browser binary with `fork()` + `execvp()`.



---

### Chat
**RAM:** 20 MB · **Priority:** Normal

The Chat app creates a direct TCP connection — no relay server needed. TCP peer-to-peer text chat. One user runs as **HOST** (binds port 9999), the other as **JOIN** (connects by IP). Works on LAN, ZeroTier/Tailscale VPN, or port-forwarded internet. A background `pthread` handles all socket I/O so the UI never blocks. Chat history saved to `hdd/chat_history.txt`. Supports up to 300 in-memory messages with timestamps and sender/receiver colour coding.


<div align="center">
<img width="452" height="271" alt="image" src="https://github.com/user-attachments/assets/0393bd07-6738-4867-bf51-af793b8a9af5" />

</div>

---

## Technology Stack

| Component | Technology |
|-----------|-----------|
| Language | C++17 |
| Graphics & Audio | raylib 5.5 (OpenGL 4.5 core profile) |
| IPC | POSIX Shared Memory · Message Queues · Named Semaphores |
| Threading | POSIX `pthread` (4 kernel background threads) |
| Font | DejaVu Sans Bold — custom codepoint array: ASCII + U+2500–U+259F |
| Rendering backend | Xvfb + llvmpipe Mesa software GL (headless, no GPU needed) |
| Build system | GNU Make + g++ |
| Platform | Linux (tested on NixOS / Replit) |

---

## IPC Reference

| Constant | Value | Purpose |
|----------|-------|---------|
| `SHM_KEY` | `0x4E584F53` | Shared memory — `OSResources` struct (PCB table, resource counters, flags) |
| `MSG_KEY` | `0x4E584F54` | Message queue — `ResourceRequest` / `ResourceReply` |
| Semaphore name | `/nexos_shm_sem` | Named POSIX semaphore — mutex on `OSResources` |
| `MAX_PROCESSES` | `32` | Maximum concurrent PCB entries |

### App IPC Flow

```
App startup:
  RequestResources(name, ram_mb, hdd_mb, priority, queue_level)
    → msgsnd(mtype=1)  ──────►  ResourceManagerThread
    ◄── msgrcv(mtype=pid)        reply.granted / reply.reason

App exit:
  ReleaseResources(name, ram_mb, hdd_mb)
    → sem_wait → update OSResources → remove PCB → sem_post
```

---

## Building & Running

## Prerequisites

| Dependency | Version | How to Get |
|---|---|---|
| `g++` | C++17 (≥ 9) | `sudo apt install build-essential` |
| `raylib` | 5.5 | `sudo apt install libraylib-dev` or [build from source](https://github.com/raysan5/raylib) |
| `libGL` | any | `sudo apt install libgl1-mesa-dev` |
| `libX11` | any | `sudo apt install libx11-dev` |
| `pkg-config` | any | `sudo apt install pkg-config` |
| `xvfb-run` | any | `sudo apt install xvfb` *(headless / VNC environments only)* |

---

## Build & Run

```bash
# Clone
git clone https://github.com/XYZ-tec/operating-system.git
cd operating-system/RayVerve

# Build the OS and all 14 apps
make all

# Run with default hardware: 2 GB RAM, 256 GB HDD, 8 CPU cores
./RayVerve 2048 262144 8

```
### Custom hardware at launch

```bash
./RayVerve <RAM_MB> <HDD_MB> <CPU_cores>

# Examples
./RayVerve 512 4096 2      # tight resources — see apps get denied
./RayVerve 8192 524288 16  # generous — run everything at once
```

A **Hardware Configuration Screen** also appears on startup so you can adjust these values before the desktop loads.

### Build a single app

```bash
make apps/chat
make apps/paint
```

### Clean all binaries

```bash
make clean
```

---
### Keyboard Shortcuts

| Key | Action |
|-----|--------|
| `K` | Open / close Kernel Mode panel |
| `ESC` | Graceful shutdown — terminates all child processes |
| Click icon | Launch app |
| Search bar | Type app name to quick-launch |

---

## Project Structure

```
.
├── RayVerve/
│   ├── os.cpp                    # Kernel — desktop, IPC setup, scheduler, all 4 threads
│   ├── Makefile
│   ├── apps/
│   │   ├── app_template.cpp      # Starter template for new apps
│   │   ├── paint.cpp
│   │   ├── calculator.cpp
│   │   ├── notepad.cpp
│   │   ├── tetris.cpp
│   │   ├── brickbreaker.cpp
│   │   ├── browser.cpp
│   │   ├── chat.cpp
│   │   ├── rayverve_shell.cpp
│   │   ├── songplayer.cpp
│   │   ├── alarm.cpp
│   │   ├── weather.cpp
│   │   ├── file_manager.cpp
│   │   └── calendar.cpp
│   ├── include/
│   │   ├── theme.h               # Colors, font sizes, DrawGlowRect, DrawButton helpers
│   │   ├── resources.h           # PCB, OSResources, ResourceRequest/Reply structs
│   │   └── ipc.h                 # RequestResources() / ReleaseResources() helpers
│   └── assets/
│       ├── fonts/                # DejaVu Sans Bold
│       ├── icons/                # PNG icons per app
│       └── songs/                # Audio files for Song Player (mp3/ogg/wav)
└── README.md
```

---

## OS Concepts Demonstrated

| Concept | Implementation |
|---------|---------------|
| **Process Management** | `fork()` + `exec()` per app; PCB table in shared memory |
| **Inter-Process Communication** | POSIX message queue for resource negotiation |
| **Mutual Exclusion** | Named POSIX semaphore protects the shared PCB table |
| **CPU Scheduling** | Two-level priority queue, 500 ms scheduler tick |
| **Priority Aging** | Wait-time monitoring; starved processes boosted every 5 s |
| **Deadlock Detection** | Monitors blocked-process count + RAM pressure every 10 s |
| **Resource Management** | Per-app RAM + HDD quota; granted at launch, released at exit |
| **Virtual Filesystem** | `hdd/` directory acts as the OS virtual disk |
| **Minimize / Restore** | `SIGSTOP` / `SIGCONT` sent to child PIDs |
| **Graceful Shutdown** | `SIGTERM` broadcast; OS reaps children with `waitpid` |

---

## Adding a New App

1. Copy the template:
   ```bash
   cp RayVerve/apps/app_template.cpp RayVerve/apps/myapp.cpp
   ```

2. Set the constants at the top of your file:
   ```cpp
   #define APP_NAME  "My App"
   #define RAM_MB    40
   #define HDD_MB    10
   #define WIN_W     800
   #define WIN_H     600
   ```

3. Register it in `os.cpp` — add a row to `APPS[]`:
   ```cpp
   { "My App", "apps/myapp", RAM_MB, HDD_MB, PRIORITY_NORMAL, 1, NEON_CYAN, "assets/icons/myapp.png" }
   ```
   Increment `APP_COUNT` accordingly.

4. Add it to `Makefile`'s `APPS` list, then build:
   ```bash
   make apps/myapp
   make   # rebuild kernel to pick up the new entry
   ```

The `RequestResources()` / `ReleaseResources()` calls in the template's `main()` handle all IPC registration automatically.

---

## Logging

The OS writes a timestamped log to `logs/nexos.log`. Entries cover:

- Boot / shutdown events with hardware configuration
- Resource grant / deny decisions (app name, PID, RAM, HDD)
- Priority aging events
- Deadlock detection events
- App launch and exit (PID)

Log format: `[HH:MM:SS] <message>`

Logging is toggled by the `logging_enabled` flag in `OSResources` (default: on).

---

<div align="center">

Built with raylib · C++17 · POSIX IPC

</div>
