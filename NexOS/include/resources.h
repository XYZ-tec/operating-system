#pragma once
#include <time.h>

// ============================================================
//  NexOS — Shared Resource Structs
//  These structs live in POSIX shared memory so every child
//  process can read/write system resources safely.
// ============================================================

#define MAX_PROCESSES   32
#define SHM_KEY         0x4E584F53   // "NXOS" in hex
#define MSG_KEY         0x4E584F54   // "NXOT" in hex

// --- Process states -----------------------------------------
#define STATE_NEW       0
#define STATE_READY     1
#define STATE_RUNNING   2
#define STATE_BLOCKED   3
#define STATE_TERMINATED 4

// --- Process priorities -------------------------------------
#define PRIORITY_LOW    3
#define PRIORITY_NORMAL 2
#define PRIORITY_HIGH   1

// ============================================================
//  Process Control Block — one per running app
// ============================================================
struct PCB {
    int     pid;                // process ID
    char    name[32];           // app name e.g. "Notepad"
    int     state;              // STATE_* constant above
    int     priority;           // 1=high 2=normal 3=low
    int     ram_mb;             // RAM this process was granted
    int     hdd_mb;             // HDD this process was granted
    time_t  start_time;         // when process was created
    time_t  ready_since;        // when it last entered ready queue
    int     wait_time_sec;      // total time spent waiting (for aging)
    int     queue_level;        // 0=system/daemon  1=user app
    bool    is_minimized;       // minimized on desktop
};

// ============================================================
//  OS Resource Block — one global instance in shared memory
// ============================================================
struct OSResources {
    // Hardware specs (set at boot, never change)
    int  total_ram_mb;
    int  total_hdd_mb;
    int  total_cores;

    // Current usage (updated by OS as apps open/close)
    int  used_ram_mb;
    int  used_hdd_mb;
    int  used_cores;

    // Process table
    PCB  processes[MAX_PROCESSES];
    int  process_count;

    // System state
    bool kernel_mode;
    bool shutdown_requested;

    // Deadlock flag
    bool deadlock_detected;
    char deadlock_msg[128];

    // Log toggle
    bool logging_enabled;
};

// ============================================================
//  IPC Message structs — app sends request, OS replies
// ============================================================

// App → OS: "I need this much memory to start"
struct ResourceRequest {
    long mtype;          // MUST be 1 for requests
    pid_t pid;
    char  app_name[32];
    int   ram_needed_mb;
    int   hdd_needed_mb;
    int   priority;      // PRIORITY_* constant
    int   queue_level;   // 0=system 1=user
};

// OS → App: "granted or denied"
struct ResourceReply {
    long mtype;          // set to requesting pid so only that app gets it
    bool granted;
    char reason[64];     // e.g. "Insufficient RAM"
};
