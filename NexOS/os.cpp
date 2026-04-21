// ============================================================
//  NexOS — Main OS Process  (os.cpp)
//
//  Compile:
//    g++ os.cpp -o NexOS -lraylib -lGL -lm -lpthread -ldl -lrt -lX11 -Iinclude
//
//  Run:
//    ./NexOS <RAM_MB> <HDD_MB> <CORES>
//    ./NexOS 2048 262144 8
//
//  Member 1 owns this file.
// ============================================================

#include "raylib.h"
#include "raymath.h"
#include "include/theme.h"
#include "include/resources.h"
#include "include/ipc.h"

#include <unistd.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <sys/wait.h>
#include <semaphore.h>
#include <signal.h>
#include <pthread.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <string>
#include <vector>
#include <queue>

// ============================================================
//  Screen dimensions
// ============================================================
#define SCREEN_W   1280
#define SCREEN_H    720

// ============================================================
//  App descriptor — one per launchable app
// ============================================================
struct AppInfo {
    const char* name;       // display name
    const char* binary;     // path to compiled binary
    int  ram_mb;            // RAM cost
    int  hdd_mb;            // HDD cost
    int  priority;          // PRIORITY_*
    int  queue_level;       // 0=system 1=user
    Color iconColor;        // icon accent color
    const char* iconLabel;  // 1-2 char symbol shown on icon
};

// ============================================================
//  All 15 launchable apps
// ============================================================
static AppInfo APPS[] = {
    { "Notepad",      "apps/notepad",       50,  10, PRIORITY_NORMAL, 1, NEON_CYAN,   "N"  },
    { "Calculator",   "apps/calculator",    30,   5, PRIORITY_NORMAL, 1, NEON_GOLD,   "C"  },
    { "File Manager", "apps/file_manager",  60,  20, PRIORITY_NORMAL, 1, NEON_GREEN,  "FM" },
    { "Sys Monitor",  "apps/system_monitor",40,   5, PRIORITY_NORMAL, 1, NEON_CYAN,   "SM" },
    { "Clock",        "apps/clock",         20,   1, PRIORITY_HIGH,   0, NEON_GOLD,   "CL" },
    { "Minesweeper",  "apps/minesweeper",   70,  10, PRIORITY_LOW,    1, NEON_GREEN,  "MS" },
    { "Snake",        "apps/snake",         60,  10, PRIORITY_LOW,    1, NEON_GREEN,  "SN" },
    { "Tetris",       "apps/tetris",        80,  10, PRIORITY_LOW,    1, NEON_PINK,   "T"  },
    { "Paint",        "apps/paint",         90,  50, PRIORITY_NORMAL, 1, NEON_PURPLE, "PT" },
    { "Sched. Vis",   "apps/scheduler_vis", 40,   5, PRIORITY_HIGH,   0, NEON_CYAN,   "SV" },
    { "LAN Chat",     "apps/lan_chat",      50,  10, PRIORITY_NORMAL, 1, NEON_PINK,   "LC" },
    { "NexOS Shell",  "apps/nexos_shell",   60,  10, PRIORITY_NORMAL, 1, NEON_GREEN,  "SH" },
    { "Process Vis",  "apps/process_vis",   40,   5, PRIORITY_NORMAL, 1, NEON_PURPLE, "PV" },
    { "Alarm",        "apps/alarm",         20,   5, PRIORITY_HIGH,   0, NEON_ORANGE, "AL" },
    { "Encryption",   "apps/encryption",    50,  20, PRIORITY_NORMAL, 1, NEON_GOLD,   "EN" },
};
static const int APP_COUNT = 15;

// ============================================================
//  Running process tracker (in the OS, not in shared memory)
// ============================================================
struct RunningApp {
    int   appIndex;   // index into APPS[]
    pid_t pid;
    bool  minimized;
};
static std::vector<RunningApp> runningApps;

// ============================================================
//  Global pointers to shared memory + IPC
// ============================================================
static OSResources* sharedRes = nullptr;
static int          mqid      = -1;
static sem_t*       shm_sem   = nullptr;
static int          shmid     = -1;

// ============================================================
//  System log
// ============================================================
static FILE* logFile = nullptr;

static void Log(const char* fmt, ...)
{
    if (!logFile || !sharedRes || !sharedRes->logging_enabled) return;
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char ts[20];
    strftime(ts, sizeof(ts), "%H:%M:%S", t);
    fprintf(logFile, "[%s] ", ts);
    va_list args;
    va_start(args, fmt);
    vfprintf(logFile, fmt, args);
    va_end(args);
    fprintf(logFile, "\n");
    fflush(logFile);
}

// ============================================================
//  Background thread: handle incoming resource requests
// ============================================================
static void* ResourceManagerThread(void*)
{
    while (true) {
        if (!sharedRes || sharedRes->shutdown_requested) break;

        ResourceRequest req;
        memset(&req, 0, sizeof(req));
        // Non-blocking check; sleep briefly if nothing there
        if (msgrcv(mqid, &req, sizeof(req) - sizeof(long), 1, IPC_NOWAIT) < 0) {
            usleep(50000); // 50ms
            continue;
        }

        ResourceReply reply;
        memset(&reply, 0, sizeof(reply));
        reply.mtype = (long)req.pid;

        sem_wait(shm_sem);

        int freeRam = sharedRes->total_ram_mb - sharedRes->used_ram_mb;
        int freeHdd = sharedRes->total_hdd_mb - sharedRes->used_hdd_mb;

        if (req.ram_needed_mb <= freeRam && req.hdd_needed_mb <= freeHdd) {
            // Grant
            sharedRes->used_ram_mb += req.ram_needed_mb;
            sharedRes->used_hdd_mb += req.hdd_needed_mb;

            // Add to PCB table
            if (sharedRes->process_count < MAX_PROCESSES) {
                PCB& p = sharedRes->processes[sharedRes->process_count++];
                p.pid         = req.pid;
                p.state       = STATE_READY;
                p.priority    = req.priority;
                p.ram_mb      = req.ram_needed_mb;
                p.hdd_mb      = req.hdd_needed_mb;
                p.queue_level = req.queue_level;
                p.start_time  = time(nullptr);
                p.ready_since = time(nullptr);
                p.wait_time_sec = 0;
                p.is_minimized  = false;
                strncpy(p.name, req.app_name, 31);
            }

            reply.granted = true;
            Log("Process '%s' (PID:%d) started. RAM:%dMB HDD:%dMB granted.",
                req.app_name, req.pid, req.ram_needed_mb, req.hdd_needed_mb);
        } else {
            reply.granted = false;
            if (req.ram_needed_mb > freeRam)
                strncpy(reply.reason, "Insufficient RAM", 63);
            else
                strncpy(reply.reason, "Insufficient HDD space", 63);
            Log("Process '%s' (PID:%d) DENIED — %s.",
                req.app_name, req.pid, reply.reason);
        }

        sem_post(shm_sem);
        msgsnd(mqid, &reply, sizeof(reply) - sizeof(long), 0);
    }
    return nullptr;
}

// ============================================================
//  Background thread: multi-level queue scheduler
//  Level 0 (system/daemons) → FCFS
//  Level 1 (user apps)      → Round Robin (2s quantum)
// ============================================================
static void* SchedulerThread(void*)
{
    const int QUANTUM_SEC = 2;
    while (true) {
        if (!sharedRes || sharedRes->shutdown_requested) break;
        usleep(500000); // check every 0.5s

        sem_wait(shm_sem);

        // Simple simulation: mark READY processes as RUNNING
        // up to total_cores limit. In a real OS this would
        // context-switch; here we visualise the states.
        int running = 0;
        for (int i = 0; i < sharedRes->process_count; i++)
            if (sharedRes->processes[i].state == STATE_RUNNING) running++;

        for (int i = 0; i < sharedRes->process_count && running < sharedRes->total_cores; i++) {
            PCB& p = sharedRes->processes[i];
            if (p.state == STATE_READY) {
                p.state = STATE_RUNNING;
                running++;
            }
        }

        sem_post(shm_sem);
    }
    return nullptr;
}

// ============================================================
//  Background thread: priority aging
//  Every 5 seconds, boost priority of processes waiting > 10s
// ============================================================
static void* AgingThread(void*)
{
    while (true) {
        if (!sharedRes || sharedRes->shutdown_requested) break;
        sleep(5);

        sem_wait(shm_sem);
        time_t now = time(nullptr);
        for (int i = 0; i < sharedRes->process_count; i++) {
            PCB& p = sharedRes->processes[i];
            if (p.state == STATE_READY) {
                int waited = (int)(now - p.ready_since);
                if (waited > 10 && p.priority > PRIORITY_HIGH) {
                    p.priority--;
                    p.wait_time_sec += waited;
                    Log("Aging: PID %d '%s' priority boosted to %d (waited %ds).",
                        p.pid, p.name, p.priority, waited);
                }
            }
        }
        sem_post(shm_sem);
    }
    return nullptr;
}

// ============================================================
//  Background thread: deadlock detection (resource alloc graph)
//  Simplified: detect if two processes are each waiting for
//  the other's resource (circular wait simulation).
// ============================================================
static void* DeadlockThread(void*)
{
    while (true) {
        if (!sharedRes || sharedRes->shutdown_requested) break;
        sleep(10);

        sem_wait(shm_sem);
        // Simplified detection: if total used RAM > 90% and
        // 2+ processes are BLOCKED, flag potential deadlock.
        int blocked = 0;
        for (int i = 0; i < sharedRes->process_count; i++)
            if (sharedRes->processes[i].state == STATE_BLOCKED) blocked++;

        float ramUsage = sharedRes->total_ram_mb > 0
            ? (float)sharedRes->used_ram_mb / sharedRes->total_ram_mb
            : 0;

        if (blocked >= 2 && ramUsage > 0.90f) {
            sharedRes->deadlock_detected = true;
            strncpy(sharedRes->deadlock_msg,
                    "Possible deadlock: 2+ blocked processes, RAM >90%", 127);
            Log("DEADLOCK WARNING: %s", sharedRes->deadlock_msg);
        } else {
            sharedRes->deadlock_detected = false;
        }
        sem_post(shm_sem);
    }
    return nullptr;
}

// ============================================================
//  Reap zombie child processes without blocking
// ============================================================
static void ReapChildren()
{
    int status;
    pid_t pid;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        // Remove from our running list
        for (auto it = runningApps.begin(); it != runningApps.end(); ++it) {
            if (it->pid == pid) {
                Log("Process '%s' (PID:%d) exited.",
                    APPS[it->appIndex].name, pid);
                runningApps.erase(it);
                break;
            }
        }
    }
}

// ============================================================
//  Launch an app: fork → exec the binary
// ============================================================
static void LaunchApp(int appIndex)
{
    if (!sharedRes) return;

    // Check RAM first (rough check — ResourceManager confirms)
    int freeRam = sharedRes->total_ram_mb - sharedRes->used_ram_mb;
    if (APPS[appIndex].ram_mb > freeRam) {
        Log("Cannot launch '%s': insufficient RAM.", APPS[appIndex].name);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return; }

    if (pid == 0) {
        // Child: exec the app binary
        execl(APPS[appIndex].binary,
              APPS[appIndex].binary, nullptr);
        // If exec fails:
        fprintf(stderr, "Failed to exec %s\n", APPS[appIndex].binary);
        exit(1);
    } else {
        // Parent: record the running app
        RunningApp ra;
        ra.appIndex  = appIndex;
        ra.pid       = pid;
        ra.minimized = false;
        runningApps.push_back(ra);
        Log("Launched '%s' (PID:%d).", APPS[appIndex].name, pid);
    }
}

// ============================================================
//  Kill / close a running app
// ============================================================
static void KillApp(pid_t pid)
{
    kill(pid, SIGTERM);
    Log("Killed PID %d from kernel mode.", pid);
}

// ============================================================
//  Minimize / restore a running app
// ============================================================
static void MinimizeApp(pid_t pid, bool minimize)
{
    kill(pid, minimize ? SIGSTOP : SIGCONT);
    for (auto& ra : runningApps)
        if (ra.pid == pid) { ra.minimized = minimize; break; }

    // Update PCB
    if (sharedRes) {
        sem_wait(shm_sem);
        for (int i = 0; i < sharedRes->process_count; i++) {
            if (sharedRes->processes[i].pid == pid) {
                sharedRes->processes[i].state =
                    minimize ? STATE_BLOCKED : STATE_READY;
                sharedRes->processes[i].is_minimized = minimize;
                break;
            }
        }
        sem_post(shm_sem);
    }
}

// ============================================================
//  BOOT ANIMATION
//  Draws the animated boot screen, blocks until done.
// ============================================================
static void RunBootAnimation()
{
    float progress = 0.0f;
    float alpha    = 0.0f;
    int   frame    = 0;

    const char* bootLines[] = {
        "Initializing hardware resources...",
        "Loading kernel modules...",
        "Starting IPC message queues...",
        "Mounting virtual file system...",
        "Starting background daemons...",
        "NexOS ready.",
    };
    int lineCount  = 6;
    int shownLines = 0;

    while (!WindowShouldClose() && progress < 1.0f) {
        progress += 0.004f;
        alpha    = fminf(alpha + 0.02f, 1.0f);
        frame++;

        if (frame % 40 == 0 && shownLines < lineCount) shownLines++;

        BeginDrawing();
        ClearBackground(BG_DEEP);
        DrawCyberpunkGrid(SCREEN_W, SCREEN_H);

        // --- OS name (large, centered, fades in) ---
        const char* osName = "NexOS";
        int nameSize = 80;
        int nameW = MeasureText(osName, nameSize);
        Color nameColor = { 0, 255, 200, (unsigned char)(int)(alpha * 255) };
        DrawText(osName, (SCREEN_W - nameW) / 2, 200, nameSize, nameColor);

        // Tagline
        const char* tag = "A Multi-Process Operating System";
        int tagW = MeasureText(tag, FONT_NORMAL);
        Color tagColor = { 140, 60, 220, (unsigned char)(int)(alpha * 200) };
        DrawText(tag, (SCREEN_W - tagW) / 2, 295, FONT_NORMAL, tagColor);

        // --- Loading bar ---
        int barX = 340, barY = 370, barW = 600, barH = 6;
        DrawRectangle(barX, barY, barW, barH, BG_PANEL);
        DrawRectangle(barX, barY, (int)(barW * progress), barH, NEON_CYAN);
        // Glow on tip
        int tip = barX + (int)(barW * progress);
        DrawRectangle(tip - 6, barY - 4, 12, barH + 8,
                      { 0, 255, 200, 80 });

        // Percentage
        char pct[8]; sprintf(pct, "%d%%", (int)(progress * 100));
        DrawText(pct, barX + barW + 12, barY - 4, FONT_SMALL, TEXT_MUTED);

        // --- Boot log lines ---
        for (int i = 0; i < shownLines; i++) {
            Color lc = (i == shownLines - 1) ? NEON_CYAN : TEXT_MUTED;
            DrawText(bootLines[i], 340, 410 + i * 22, FONT_SMALL, lc);
        }

        // Flicker cursor on last line
        if (shownLines > 0 && (frame / 15) % 2 == 0) {
            int lw = MeasureText(bootLines[shownLines-1], FONT_SMALL);
            DrawText("_", 340 + lw + 4, 410 + (shownLines-1)*22,
                     FONT_SMALL, NEON_CYAN);
        }

        EndDrawing();
    }
}

// ============================================================
//  DESKTOP ICON DRAWING
// ============================================================
static const int ICONS_PER_ROW  = 5;
static const int ICON_START_X   = 60;
static const int ICON_START_Y   = 60;

static Rectangle GetIconRect(int index)
{
    int col = index % ICONS_PER_ROW;
    int row = index / ICONS_PER_ROW;
    int x   = ICON_START_X + col * (ICON_SIZE + ICON_GAP);
    int y   = ICON_START_Y + row * (ICON_SIZE + ICON_GAP + 20);
    return { (float)x, (float)y, (float)ICON_SIZE, (float)ICON_SIZE };
}

static int hoveredIcon = -1;

static void DrawDesktopIcons()
{
    Vector2 mouse = GetMousePosition();
    hoveredIcon = -1;

    for (int i = 0; i < APP_COUNT; i++) {
        Rectangle r = GetIconRect(i);
        bool hovered = CheckCollisionPointRec(mouse, r);
        if (hovered) hoveredIcon = i;

        // Icon background
        Color bg = hovered ? BG_HOVER : BG_ICON;
        DrawRectangleRec(r, bg);

        // Neon border (glow on hover)
        if (hovered)
            DrawGlowRect(r, APPS[i].iconColor, 4);
        else
            DrawRectangleLinesEx(r, 1.0f,
                { APPS[i].iconColor.r, APPS[i].iconColor.g,
                  APPS[i].iconColor.b, 80 });

        // Icon label (symbol)
        int sw = MeasureText(APPS[i].iconLabel, FONT_LARGE);
        DrawText(APPS[i].iconLabel,
                 (int)(r.x + (r.width  - sw) / 2),
                 (int)(r.y + (r.height - FONT_LARGE) / 2 - 8),
                 FONT_LARGE, APPS[i].iconColor);

        // App name below icon
        int nw = MeasureText(APPS[i].name, FONT_TINY);
        DrawText(APPS[i].name,
                 (int)(r.x + (r.width - nw) / 2),
                 (int)(r.y + r.height + 4),
                 FONT_TINY, hovered ? TEXT_PRIMARY : TEXT_MUTED);

        // Running indicator dot
        bool isRunning = false;
        for (auto& ra : runningApps)
            if (ra.appIndex == i) { isRunning = true; break; }
        if (isRunning)
            DrawCircle((int)(r.x + r.width / 2),
                       (int)(r.y + r.height + 18), 3, NEON_CYAN);
    }
}

// ============================================================
//  TASKBAR
// ============================================================
static bool kernelModeActive = false;

static void DrawTaskbar(float ramFrac, float hddFrac)
{
    int y = SCREEN_H - TASKBAR_H;

    // Background
    DrawRectangle(0, y, SCREEN_W, TASKBAR_H, BG_TASKBAR);
    DrawLine(0, y, SCREEN_W, y, NEON_CYAN);

    // OS logo
    DrawText("NexOS", 14, y + 13, FONT_NORMAL, NEON_CYAN);
    DrawLine(80, y + 6, 80, y + TASKBAR_H - 6, BORDER_DIM);

    // Running app pills
    int pillX = 90;
    for (auto& ra : runningApps) {
        const char* name = APPS[ra.appIndex].name;
        int pw = MeasureText(name, FONT_SMALL) + 20;
        Rectangle pill = { (float)pillX, (float)(y + 8),
                           (float)pw, (float)(TASKBAR_H - 16) };
        Color pillBg = ra.minimized ? BG_PANEL : BG_HOVER;
        DrawRectangleRec(pill, pillBg);
        DrawRectangleLinesEx(pill, 1.0f,
            ra.minimized ? BORDER_DIM : NEON_CYAN);
        DrawText(name, pillX + 10, y + 14, FONT_SMALL,
                 ra.minimized ? TEXT_MUTED : TEXT_PRIMARY);

        // Click: restore if minimized
        if (CheckCollisionPointRec(GetMousePosition(), pill)
            && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (ra.minimized) MinimizeApp(ra.pid, false);
        }
        pillX += pw + 6;
    }

    // Right side: RAM bar, HDD bar, clock, kernel mode
    int rx = SCREEN_W - 360;

    // RAM
    DrawText("RAM", rx, y + 9, FONT_TINY, TEXT_MUTED);
    DrawProgressBar({ (float)(rx + 35), (float)(y + 11), 80, 10 },
                    ramFrac, NEON_GOLD, BG_PANEL);
    char ramTxt[16];
    sprintf(ramTxt, "%d%%", (int)(ramFrac * 100));
    DrawText(ramTxt, rx + 120, y + 9, FONT_TINY, TEXT_MUTED);

    // HDD
    DrawText("HDD", rx + 150, y + 9, FONT_TINY, TEXT_MUTED);
    DrawProgressBar({ (float)(rx + 185), (float)(y + 11), 60, 10 },
                    hddFrac, NEON_PURPLE, BG_PANEL);

    // Clock
    time_t now = time(nullptr);
    struct tm* t = localtime(&now);
    char clk[12]; strftime(clk, sizeof(clk), "%H:%M:%S", t);
    DrawText(clk, rx + 260, y + 13, FONT_SMALL, NEON_CYAN);

    // Kernel mode indicator
    if (kernelModeActive) {
        DrawRectangle(rx - 90, y + 8, 82, TASKBAR_H - 16,
                      { 140, 60, 220, 60 });
        DrawText("KERNEL", rx - 84, y + 14, FONT_TINY, NEON_PURPLE);
    }
}

// ============================================================
//  KERNEL MODE SCREEN
// ============================================================
static bool showKernelMode = false;
static char kernelPassword[32] = "";
static bool kernelUnlocked = false;
static bool typingPassword = false;

static void DrawKernelMode()
{
    if (!showKernelMode) return;

    // Dim overlay
    DrawRectangle(0, 0, SCREEN_W, SCREEN_H, { 0, 0, 0, 180 });

    int pw = 700, ph = 460;
    int px = (SCREEN_W - pw) / 2, py = (SCREEN_H - ph) / 2;
    Rectangle panel = { (float)px, (float)py, (float)pw, (float)ph };

    DrawRectangleRec(panel, BG_PANEL);
    DrawGlowRect(panel, NEON_PURPLE, 6);

    // Title
    DrawText("KERNEL MODE", px + 20, py + 14, FONT_LARGE, NEON_PURPLE);
    DrawLine(px, py + 40, px + pw, py + 40, BORDER_DIM);

    if (!kernelUnlocked) {
        // Password prompt
        DrawText("Enter kernel password:", px + 200, py + 100,
                 FONT_NORMAL, TEXT_PRIMARY);

        // Password box
        Rectangle passBox = { (float)(px+160), (float)(py+135), 380, 36 };
        DrawRectangleRec(passBox, BG_DEEP);
        DrawRectangleLinesEx(passBox, 1.5f, typingPassword ? NEON_PURPLE : BORDER_DIM);

        // Show asterisks
        std::string stars(strlen(kernelPassword), '*');
        DrawText(stars.c_str(), px + 170, py + 145, FONT_NORMAL, TEXT_PRIMARY);

        // Cursor
        if (typingPassword && (int)(GetTime() * 2) % 2 == 0) {
            int sw = MeasureText(stars.c_str(), FONT_NORMAL);
            DrawText("|", px + 170 + sw + 2, py + 145, FONT_NORMAL, NEON_PURPLE);
        }

        if (DrawButton({ (float)(px+250), (float)(py+195), 200, 36 },
                       "UNLOCK", BG_HOVER, NEON_PURPLE, FONT_SMALL)) {
            typingPassword = false;
            // Password is "nexos" (changeable)
            if (strcmp(kernelPassword, "nexos") == 0) {
                kernelUnlocked = true;
                kernelModeActive = true;
                Log("Kernel mode unlocked.");
            } else {
                memset(kernelPassword, 0, sizeof(kernelPassword));
            }
        }

        // Click password box to type
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            Rectangle pb = { (float)(px+160), (float)(py+135), 380, 36 };
            typingPassword = CheckCollisionPointRec(GetMousePosition(), pb);
        }

        // Handle typing
        if (typingPassword) {
            int key = GetCharPressed();
            while (key > 0) {
                int len = strlen(kernelPassword);
                if (len < 31 && key >= 32 && key <= 125) {
                    kernelPassword[len]   = (char)key;
                    kernelPassword[len+1] = '\0';
                }
                key = GetCharPressed();
            }
            if (IsKeyPressed(KEY_BACKSPACE) && strlen(kernelPassword) > 0)
                kernelPassword[strlen(kernelPassword)-1] = '\0';
        }

    } else {
        // PCB Table
        DrawText("PID", px + 20,  py + 55, FONT_SMALL, TEXT_MUTED);
        DrawText("NAME",px + 90,  py + 55, FONT_SMALL, TEXT_MUTED);
        DrawText("STATE",px+240,  py + 55, FONT_SMALL, TEXT_MUTED);
        DrawText("RAM", px + 340, py + 55, FONT_SMALL, TEXT_MUTED);
        DrawText("PRI", px + 420, py + 55, FONT_SMALL, TEXT_MUTED);
        DrawLine(px+10, py+72, px+pw-10, py+72, BORDER_DIM);

        const char* stateNames[] = {"NEW","READY","RUNNING","BLOCKED","DONE"};
        Color stateColors[] = {TEXT_MUTED, NEON_GOLD, NEON_GREEN, NEON_PINK, TEXT_DIM};

        if (sharedRes) {
            sem_wait(shm_sem);
            int row = 0;
            for (int i = 0; i < sharedRes->process_count && row < 10; i++, row++) {
                PCB& p = sharedRes->processes[i];
                int ry = py + 80 + row * 26;
                Color rowBg = (row % 2 == 0) ? BG_DEEP : BG_PANEL;
                DrawRectangle(px + 10, ry - 2, pw - 20, 24, rowBg);

                char pidStr[16], ramStr[16], priStr[8];
                sprintf(pidStr, "%d", p.pid);
                sprintf(ramStr, "%dMB", p.ram_mb);
                sprintf(priStr, "%d", p.priority);

                DrawText(pidStr, px + 20, ry, FONT_SMALL, TEXT_PRIMARY);
                DrawText(p.name, px + 90,  ry, FONT_SMALL, TEXT_PRIMARY);
                int si = p.state < 5 ? p.state : 0;
                DrawText(stateNames[si], px+240, ry, FONT_SMALL, stateColors[si]);
                DrawText(ramStr, px + 340, ry, FONT_SMALL, TEXT_MUTED);
                DrawText(priStr, px + 420, ry, FONT_SMALL, TEXT_MUTED);

                // Kill button
                Rectangle killBtn = { (float)(px+500), (float)(ry-1), 60, 20 };
                if (DrawButton(killBtn, "KILL", BG_DEEP, NEON_PINK, FONT_TINY))
                    KillApp(p.pid);
            }
            if (sharedRes->process_count == 0)
                DrawText("No processes running.", px+220, py+160, FONT_NORMAL, TEXT_MUTED);

            // Deadlock warning
            if (sharedRes->deadlock_detected) {
                DrawRectangle(px+10, py+ph-60, pw-20, 30, { 255, 45, 120, 40 });
                DrawText(sharedRes->deadlock_msg, px+20, py+ph-52, FONT_TINY, NEON_PINK);
            }
            sem_post(shm_sem);
        }

        // Lock button
        if (DrawButton({ (float)(px+pw-130), (float)(py+ph-45), 120, 30 },
                       "LOCK KERNEL", BG_DEEP, NEON_PURPLE, FONT_TINY)) {
            kernelUnlocked  = false;
            kernelModeActive = false;
            memset(kernelPassword, 0, sizeof(kernelPassword));
            Log("Kernel mode locked.");
        }
    }

    // Close button
    if (DrawButton({ (float)(px+pw-40), (float)(py+8), 28, 22 },
                   "X", BG_DEEP, NEON_PINK, FONT_SMALL)) {
        showKernelMode  = false;
        kernelUnlocked  = false;
        kernelModeActive = false;
        memset(kernelPassword, 0, sizeof(kernelPassword));
    }
}

// ============================================================
//  SHUTDOWN SCREEN
// ============================================================
static void RunShutdownAnimation()
{
    float alpha = 0.0f;
    while (!WindowShouldClose() && alpha < 1.0f) {
        alpha += 0.015f;
        BeginDrawing();
        ClearBackground(BG_DEEP);
        DrawCyberpunkGrid(SCREEN_W, SCREEN_H);
        const char* msg = "Shutting down NexOS...";
        int mw = MeasureText(msg, FONT_LARGE);
        Color mc = { 0, 255, 200, (unsigned char)(int)((1.0f - alpha * 0.5f) * 255) };
        DrawText(msg, (SCREEN_W - mw)/2, SCREEN_H/2 - 30, FONT_LARGE, mc);
        const char* bye = "Goodbye.";
        int bw = MeasureText(bye, FONT_NORMAL);
        Color bc = { 140, 60, 220, (unsigned char)(int)(alpha * 200) };
        DrawText(bye, (SCREEN_W - bw)/2, SCREEN_H/2 + 20, FONT_NORMAL, bc);
        EndDrawing();
    }
}

// ============================================================
//  Shutdown: kill all children, free IPC, cleanup
// ============================================================
static void Shutdown()
{
    Log("NexOS shutting down. Terminating all processes.");

    // Signal all children
    for (auto& ra : runningApps)
        kill(ra.pid, SIGTERM);

    // Wait for them
    for (auto& ra : runningApps)
        waitpid(ra.pid, nullptr, 0);

    // Tell background threads to stop
    if (sharedRes) sharedRes->shutdown_requested = true;
    sleep(1);

    // Cleanup IPC
    if (shm_sem) { sem_close(shm_sem); sem_unlink(SEM_NAME); }
    if (shmid >= 0) shmctl(shmid, IPC_RMID, nullptr);
    if (mqid  >= 0) msgctl(mqid,  IPC_RMID, nullptr);
    if (logFile)    fclose(logFile);
}

// ============================================================
//  MAIN
// ============================================================
int main(int argc, char* argv[])
{
    // --- Parse hardware arguments ---------------------------
    int ramMB  = 2048;
    int hddMB  = 262144;
    int cores  = 8;
    if (argc >= 4) {
        ramMB = atoi(argv[1]);
        hddMB = atoi(argv[2]);
        cores = atoi(argv[3]);
    } else {
        printf("Usage: ./NexOS <RAM_MB> <HDD_MB> <CORES>\n");
        printf("Example: ./NexOS 2048 262144 8\n");
        printf("Using defaults: 2048MB RAM, 256GB HDD, 8 cores\n");
    }

    // --- Open log file --------------------------------------
    logFile = fopen("logs/nexos.log", "a");

    // --- Init shared memory ---------------------------------
    shmid = shmget(SHM_KEY, sizeof(OSResources),
                   IPC_CREAT | IPC_EXCL | 0666);
    if (shmid < 0) {
        // Already exists — remove and recreate
        shmid = shmget(SHM_KEY, sizeof(OSResources), 0666);
        shmctl(shmid, IPC_RMID, nullptr);
        shmid = shmget(SHM_KEY, sizeof(OSResources),
                       IPC_CREAT | 0666);
    }
    sharedRes = (OSResources*)shmat(shmid, nullptr, 0);
    memset(sharedRes, 0, sizeof(OSResources));
    sharedRes->total_ram_mb    = ramMB;
    sharedRes->total_hdd_mb    = hddMB;
    sharedRes->total_cores     = cores;
    sharedRes->logging_enabled = true;
    sharedRes->shutdown_requested = false;

    // --- Init semaphore -------------------------------------
    sem_unlink(SEM_NAME);
    shm_sem = sem_open(SEM_NAME, O_CREAT | O_EXCL, 0666, 1);

    // --- Init message queue ---------------------------------
    msgctl(msgget(MSG_KEY, 0666), IPC_RMID, nullptr); // remove old
    mqid = msgget(MSG_KEY, IPC_CREAT | 0666);

    Log("NexOS booting. RAM:%dMB HDD:%dMB Cores:%d", ramMB, hddMB, cores);

    // --- Start background threads ---------------------------
    pthread_t tResource, tScheduler, tAging, tDeadlock;
    pthread_create(&tResource,  nullptr, ResourceManagerThread, nullptr);
    pthread_create(&tScheduler, nullptr, SchedulerThread,       nullptr);
    pthread_create(&tAging,     nullptr, AgingThread,           nullptr);
    pthread_create(&tDeadlock,  nullptr, DeadlockThread,        nullptr);

    // --- Init raylib window ---------------------------------
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(SCREEN_W, SCREEN_H, "NexOS");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL); // disable default ESC close

    // --- Boot animation -------------------------------------
    RunBootAnimation();

    // --- Auto-start system daemons (Clock, Alarm) -----------
    LaunchApp(4);  // Clock (index 4)

    // ============================================================
    //  MAIN LOOP
    // ============================================================
    bool running = true;
    while (!WindowShouldClose() && running) {

        // Reap any exited children
        ReapChildren();

        // --- Input handling ---------------------------------

        // K = kernel mode
        if (IsKeyPressed(KEY_K) && !showKernelMode) {
            showKernelMode = true;
            typingPassword = false;
        }

        // ESC = close kernel mode if open, else begin shutdown
        if (IsKeyPressed(KEY_ESCAPE)) {
            if (showKernelMode) {
                showKernelMode  = false;
                kernelUnlocked  = false;
                kernelModeActive = false;
            } else {
                running = false;
            }
        }

        // Double-click icon to launch
        if (!showKernelMode && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            static double lastClickTime = 0;
            static int    lastClickIcon = -1;
            double now = GetTime();

            if (hoveredIcon >= 0) {
                if (hoveredIcon == lastClickIcon &&
                    now - lastClickTime < 0.4) {
                    LaunchApp(hoveredIcon);
                    lastClickTime = 0;
                    lastClickIcon = -1;
                } else {
                    lastClickTime = now;
                    lastClickIcon = hoveredIcon;
                }
            }
        }

        // Read resource usage
        float ramFrac = 0, hddFrac = 0;
        if (sharedRes && sharedRes->total_ram_mb > 0) {
            ramFrac = (float)sharedRes->used_ram_mb / sharedRes->total_ram_mb;
            hddFrac = (float)sharedRes->used_hdd_mb / sharedRes->total_hdd_mb;
        }

        // --- Draw -------------------------------------------
        BeginDrawing();
        ClearBackground(BG_DEEP);

        DrawCyberpunkGrid(SCREEN_W, SCREEN_H);
        DrawDesktopIcons();
        DrawTaskbar(ramFrac, hddFrac);

        // Tooltip on hovered icon
        if (hoveredIcon >= 0) {
            Rectangle ir = GetIconRect(hoveredIcon);
            char tip[64];
            sprintf(tip, "RAM: %dMB  Double-click to open",
                    APPS[hoveredIcon].ram_mb);
            int tw = MeasureText(tip, FONT_TINY);
            DrawRectangle((int)ir.x - 4, (int)ir.y - 24,
                          tw + 8, 20, BG_PANEL);
            DrawText(tip, (int)ir.x, (int)ir.y - 20,
                     FONT_TINY, TEXT_MUTED);
        }

        // Kernel mode overlay
        if (showKernelMode) DrawKernelMode();

        // Press K hint (bottom right, small)
        DrawText("[K] Kernel Mode   [ESC] Shutdown",
                 SCREEN_W - 240, SCREEN_H - TASKBAR_H - 18,
                 FONT_TINY, TEXT_DIM);

        EndDrawing();
    }

    // --- Shutdown sequence ----------------------------------
    RunShutdownAnimation();
    Shutdown();
    CloseWindow();
    return 0;
}
