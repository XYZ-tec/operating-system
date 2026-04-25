#pragma once
#include <sys/ipc.h>
#include <sys/msg.h>
#include <sys/shm.h>
#include <semaphore.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <cstring>
#include "resources.h"

// ============================================================
//  NexOS — IPC Helpers
//  Every app #includes this to:
//   1. Get a pointer to the shared OSResources block
//   2. Send a ResourceRequest and wait for ResourceReply
//   3. Notify OS on clean exit so RAM/HDD is freed
// ============================================================

#define SEM_NAME "/nexos_shm_sem"

// ============================================================
//  Get pointer to the shared OSResources block.
//  Returns nullptr if the OS hasn't created it yet.
// ============================================================
static inline OSResources* GetSharedResources()
{
    int shmid = shmget(SHM_KEY, sizeof(OSResources), 0666);
    if (shmid < 0) return nullptr;
    void* ptr = shmat(shmid, nullptr, 0);
    if (ptr == (void*)-1) return nullptr;
    return (OSResources*)ptr;
}

// ============================================================
//  Get the IPC message queue ID.
//  Returns -1 on failure.
// ============================================================
static inline int GetMsgQueue()
{
    return msgget(MSG_KEY, 0666);
}

// ============================================================
//  Lock / unlock shared memory (semaphore wrappers)
// ============================================================
static inline sem_t* OpenSem()
{
    return sem_open(SEM_NAME, 0);
}

static inline void SemLock(sem_t* s)   { if (s) sem_wait(s); }
static inline void SemUnlock(sem_t* s) { if (s) sem_post(s); }

// ============================================================
//  RequestResources — call this at the START of every app.
//
//  Sends a ResourceRequest to the OS and blocks until reply.
//  Returns true if granted, false if denied.
//
//  Usage:
//    if (!RequestResources("Notepad", 50, 10, PRIORITY_NORMAL, 1))
//        return 1; // OS denied — not enough RAM
// ============================================================
static inline bool RequestResources(const char* appName,
                                     int ramMB, int hddMB,
                                     int priority, int queueLevel)
{
    int mqid = GetMsgQueue();
    if (mqid < 0) {
        fprintf(stderr, "[%s] Cannot connect to OS message queue.\n", appName);
        return false;
    }

    // Build and send request
    ResourceRequest req;
    memset(&req, 0, sizeof(req));
    req.mtype          = 1;                 // all requests use type 1
    req.pid            = getpid();
    req.ram_needed_mb  = ramMB;
    req.hdd_needed_mb  = hddMB;
    req.priority       = priority;
    req.queue_level    = queueLevel;
    strncpy(req.app_name, appName, 31);

    if (msgsnd(mqid, &req, sizeof(req) - sizeof(long), 0) < 0) {
        perror("msgsnd");
        return false;
    }

    // Wait for reply addressed to our PID
    ResourceReply reply;
    memset(&reply, 0, sizeof(reply));
    if (msgrcv(mqid, &reply, sizeof(reply) - sizeof(long),
               (long)getpid(), 0) < 0) {
        perror("msgrcv");
        return false;
    }

    if (!reply.granted)
        fprintf(stderr, "[%s] Resource denied: %s\n", appName, reply.reason);

    return reply.granted;
}

// ============================================================
//  ReleaseResources — call this when the app is about to exit.
//  Sends a termination notice so the OS can free RAM/HDD.
// ============================================================
static inline void ReleaseResources(const char* appName,
                                     int ramMB, int hddMB)
{
    OSResources* res = GetSharedResources();
    if (!res) return;

    sem_t* sem = OpenSem();
    SemLock(sem);

    // Free memory
    res->used_ram_mb -= ramMB;
    res->used_hdd_mb -= hddMB;
    if (res->used_ram_mb < 0) res->used_ram_mb = 0;
    if (res->used_hdd_mb < 0) res->used_hdd_mb = 0;

    // Remove from PCB table
    for (int i = 0; i < res->process_count; i++) {
        if (res->processes[i].pid == getpid()) {
            // Shift remaining entries left
            for (int j = i; j < res->process_count - 1; j++)
                res->processes[j] = res->processes[j + 1];
            res->process_count--;
            break;
        }
    }

    SemUnlock(sem);
    sem_close(sem);
    shmdt(res);
}
