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
#include <stdarg.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

// ============================================================
//  Global font — loaded once, used everywhere
// ============================================================
static Font gFont;
static bool gFontLoaded = false;

// Wrapper: draw text with our custom font
static void DrawT(const char* text, int x, int y, int size, Color color)
{
    if (gFontLoaded)
        DrawTextEx(gFont, text, {(float)x,(float)y}, (float)size, 1.5f, color);
    else
        DrawText(text, x, y, size, color);
}

static int MeasureT(const char* text, int size)
{
    if (gFontLoaded)
        return (int)MeasureTextEx(gFont, text, (float)size, 1.5f).x;
    return MeasureText(text, size);
}

// ============================================================
//  App descriptor
// ============================================================
struct AppInfo {
    const char* name;
    const char* binary;
    int  ram_mb, hdd_mb, priority, queue_level;
    Color fallbackColor;
    const char* iconFile;
};

// ============================================================
//  13 apps (Kernel Monitor removed from user desktop —
//  it lives inside Kernel Mode panel now)
// ============================================================
static AppInfo APPS[] = {
    { "Paint",         "apps/paint",         90, 50, PRIORITY_NORMAL, 1, NEON_PURPLE, "assets/icons/paint.png"        },
    { "Calculator",    "apps/calculator",    30,  5, PRIORITY_NORMAL, 1, NEON_GOLD,   "assets/icons/calculator.png"   },
    { "Notepad",       "apps/notepad",       50, 10, PRIORITY_NORMAL, 1, NEON_CYAN,   "assets/icons/notepad.png"      },
    { "Tetris",        "apps/tetris",        80, 10, PRIORITY_LOW,    1, NEON_PINK,   "assets/icons/tetris.png"       },
    { "Brick Breaker", "apps/brickbreaker",  70, 10, PRIORITY_LOW,    1, NEON_GREEN,  "assets/icons/brickbreaker.png" },
    { "Chat",          "apps/chat",          50, 10, PRIORITY_NORMAL, 1, NEON_CYAN,   "assets/icons/chat.png"         },
    { "Shell",         "apps/nexos_shell",   60, 10, PRIORITY_NORMAL, 1, NEON_GREEN,  "assets/icons/shell.png"        },
    { "Song Player",   "apps/songplayer",    40, 20, PRIORITY_NORMAL, 1, NEON_PURPLE, "assets/icons/songplayer.png"   },
    { "Alarm",         "apps/alarm",         20,  1, PRIORITY_HIGH,   0, NEON_GOLD,   "assets/icons/clock.png"        },
    { "Weather",       "apps/weather",       30,  5, PRIORITY_NORMAL, 1, NEON_CYAN,   "assets/icons/weather.png"      },
    { "File Manager",  "apps/file_manager",  60, 30, PRIORITY_NORMAL, 1, NEON_GOLD,   "assets/icons/file_manager.png" },
    { "Calendar",      "apps/alarm",         20,  1, PRIORITY_HIGH,   0, NEON_CYAN,   "assets/icons/calendar.png"     },
};
static const int APP_COUNT = 12;

static Texture2D iconTextures[12];
static bool      iconLoaded[12];

struct RunningApp { int appIndex; pid_t pid; bool minimized; };
static std::vector<RunningApp> runningApps;

static OSResources* sharedRes = nullptr;
static int          mqid      = -1;
static sem_t*       shm_sem   = nullptr;
static int          shmid     = -1;
static FILE*        logFile   = nullptr;

static void Log(const char* fmt, ...)
{
    if (!logFile || !sharedRes || !sharedRes->logging_enabled) return;
    time_t now = time(nullptr); struct tm* t = localtime(&now);
    char ts[20]; strftime(ts, sizeof(ts), "%H:%M:%S", t);
    fprintf(logFile, "[%s] ", ts);
    va_list a; va_start(a, fmt); vfprintf(logFile, fmt, a); va_end(a);
    fprintf(logFile, "\n"); fflush(logFile);
}

// ============================================================
//  Background threads
// ============================================================
static void* ResourceManagerThread(void*)
{
    while (true) {
        if (!sharedRes || sharedRes->shutdown_requested) break;
        ResourceRequest req; memset(&req, 0, sizeof(req));
        if (msgrcv(mqid, &req, sizeof(req)-sizeof(long), 1, IPC_NOWAIT) < 0) { usleep(50000); continue; }
        ResourceReply reply; memset(&reply, 0, sizeof(reply));
        reply.mtype = (long)req.pid;
        sem_wait(shm_sem);
        int fR = sharedRes->total_ram_mb - sharedRes->used_ram_mb;
        int fH = sharedRes->total_hdd_mb - sharedRes->used_hdd_mb;
        if (req.ram_needed_mb <= fR && req.hdd_needed_mb <= fH) {
            sharedRes->used_ram_mb += req.ram_needed_mb;
            sharedRes->used_hdd_mb += req.hdd_needed_mb;
            if (sharedRes->process_count < MAX_PROCESSES) {
                PCB& p = sharedRes->processes[sharedRes->process_count++];
                p.pid = req.pid; p.state = STATE_READY; p.priority = req.priority;
                p.ram_mb = req.ram_needed_mb; p.hdd_mb = req.hdd_needed_mb;
                p.queue_level = req.queue_level; p.start_time = time(nullptr);
                p.ready_since = time(nullptr); p.wait_time_sec = 0; p.is_minimized = false;
                strncpy(p.name, req.app_name, 31);
            }
            reply.granted = true;
            Log("'%s' (PID:%d) granted RAM:%dMB HDD:%dMB", req.app_name, req.pid, req.ram_needed_mb, req.hdd_needed_mb);
        } else {
            reply.granted = false;
            strncpy(reply.reason, req.ram_needed_mb > fR ? "Insufficient RAM" : "Insufficient HDD", 63);
            Log("'%s' DENIED: %s", req.app_name, reply.reason);
        }
        sem_post(shm_sem);
        msgsnd(mqid, &reply, sizeof(reply)-sizeof(long), 0);
    }
    return nullptr;
}

static void* SchedulerThread(void*)
{
    while (true) {
        if (!sharedRes || sharedRes->shutdown_requested) break;
        usleep(500000);
        sem_wait(shm_sem);
        int run = 0;
        for (int i = 0; i < sharedRes->process_count; i++)
            if (sharedRes->processes[i].state == STATE_RUNNING) run++;
        for (int lvl = 0; lvl <= 1; lvl++)
            for (int i = 0; i < sharedRes->process_count && run < sharedRes->total_cores; i++) {
                PCB& p = sharedRes->processes[i];
                if (p.state == STATE_READY && p.queue_level == lvl) { p.state = STATE_RUNNING; run++; }
            }
        sem_post(shm_sem);
    }
    return nullptr;
}

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
                int w = (int)(now - p.ready_since);
                if (w > 10 && p.priority > PRIORITY_HIGH) { p.priority--; Log("Aging PID:%d prio->%d", p.pid, p.priority); }
            }
        }
        sem_post(shm_sem);
    }
    return nullptr;
}

static void* DeadlockThread(void*)
{
    while (true) {
        if (!sharedRes || sharedRes->shutdown_requested) break;
        sleep(10);
        sem_wait(shm_sem);
        int bl = 0;
        for (int i = 0; i < sharedRes->process_count; i++)
            if (sharedRes->processes[i].state == STATE_BLOCKED) bl++;
        float ru = sharedRes->total_ram_mb > 0 ? (float)sharedRes->used_ram_mb / sharedRes->total_ram_mb : 0;
        if (bl >= 2 && ru > 0.9f) {
            sharedRes->deadlock_detected = true;
            strncpy(sharedRes->deadlock_msg, "Deadlock detected among processes", 127);
            Log("DEADLOCK DETECTED");
        } else sharedRes->deadlock_detected = false;
        sem_post(shm_sem);
    }
    return nullptr;
}

static void LaunchApp(int idx)
{
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return; }
    if (pid == 0) { execl(APPS[idx].binary, APPS[idx].binary, nullptr); fprintf(stderr,"exec failed: %s\n",APPS[idx].binary); exit(1); }
    else { RunningApp ra; ra.appIndex=idx; ra.pid=pid; ra.minimized=false; runningApps.push_back(ra); Log("Launched '%s' PID:%d",APPS[idx].name,pid); }
}
static void LaunchKernelVis() {
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) { execl("apps/visualization","apps/visualization",nullptr); exit(1); }
}
static void KillApp(pid_t pid) { kill(pid,SIGTERM); Log("Killed PID:%d",pid); }
static void MinimizeApp(pid_t pid, bool m) {
    kill(pid, m?SIGSTOP:SIGCONT);
    for (auto& ra:runningApps) if(ra.pid==pid){ra.minimized=m;break;}
    if (sharedRes) {
        sem_wait(shm_sem);
        for (int i=0;i<sharedRes->process_count;i++)
            if(sharedRes->processes[i].pid==pid){sharedRes->processes[i].state=m?STATE_BLOCKED:STATE_READY;break;}
        sem_post(shm_sem);
    }
}
static void ReapChildren() {
    int st; pid_t pid;
    while ((pid=waitpid(-1,&st,WNOHANG))>0)
        for (auto it=runningApps.begin();it!=runningApps.end();++it)
            if (it->pid==pid){Log("PID:%d exited",pid);runningApps.erase(it);break;}
}

// ============================================================
//  Draw icon helper
// ============================================================
static void DrawAppIcon(int idx, Rectangle r, bool hovered)
{
    if (iconLoaded[idx]) {
        Color tint = hovered ? WHITE : Color{210,210,210,255};
        float sc = fminf(r.width/iconTextures[idx].width, r.height/iconTextures[idx].height);
        float dw = iconTextures[idx].width*sc, dh = iconTextures[idx].height*sc;
        Rectangle dst={r.x+(r.width-dw)/2, r.y+(r.height-dh)/2, dw, dh};
        Rectangle src={0,0,(float)iconTextures[idx].width,(float)iconTextures[idx].height};
        DrawTexturePro(iconTextures[idx],src,dst,{0,0},0,tint);
    } else {
        DrawRectangleRec(r, hovered?BG_HOVER:BG_ICON);
        Color c = APPS[idx].fallbackColor;
        if (hovered) DrawGlowRect(r,c,4); else DrawRectangleLinesEx(r,1.0f,{c.r,c.g,c.b,80});
        char letter[2]={APPS[idx].name[0],'\0'};
        int fw = MeasureT(letter,26);
        DrawT(letter,(int)(r.x+(r.width-fw)/2),(int)(r.y+(r.height-26)/2),26,c);
    }
}

// ============================================================
//  HARDWARE INPUT SCREEN
//  Shows before boot. User types RAM, HDD, Cores.
//  Returns true when user clicks "Start NexOS".
// ============================================================
struct InputField {
    char  buf[16];
    int   len;
    bool  active;
    int   minVal, maxVal;
    const char* label;
    const char* unit;
    const char* hint;
};

static bool RunHardwareInputScreen(int& outRam, int& outHdd, int& outCores)
{
    InputField fields[3] = {
        {"2048", 4, false, 256,  65536, "RAM",        "MB",  "256 – 65536 MB"},
        {"262144",6,false,1024,1048576, "Hard Drive",  "MB",  "1024 MB – 1 TB"},
        {"8",    1, false, 1,    64,    "CPU Cores",   "cores","1 – 64"},
    };

    char errorMsg[64] = "";
    bool done = false;

    while (!WindowShouldClose() && !done) {
        int sw = GetScreenWidth(), sh = GetScreenHeight();

        // --- Input handling ---
        for (int fi = 0; fi < 3; fi++) {
            if (!fields[fi].active) continue;
            int k = GetCharPressed();
            while (k > 0) {
                if (k >= '0' && k <= '9' && fields[fi].len < 14) {
                    fields[fi].buf[fields[fi].len++] = (char)k;
                    fields[fi].buf[fields[fi].len]   = '\0';
                }
                k = GetCharPressed();
            }
            if (IsKeyPressed(KEY_BACKSPACE) && fields[fi].len > 0)
                fields[fi].buf[--fields[fi].len] = '\0';
            if (IsKeyPressed(KEY_TAB)) {
                fields[fi].active = false;
                fields[(fi+1)%3].active = true;
            }
        }

        // Click to activate field
        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            int pw=540, fh=52, startY=sh/2-100;
            for (int fi=0; fi<3; fi++) {
                Rectangle box={(float)((sw-pw)/2),(float)(startY+fi*80),(float)pw,(float)fh};
                bool clicked = CheckCollisionPointRec(GetMousePosition(),box);
                fields[fi].active = clicked;
            }
        }

        // Draw
        BeginDrawing();
        ClearBackground(BG_DEEP);
        DrawCyberpunkGrid(sw,sh);

        // Title
        const char* title = "NexOS Hardware Configuration";
        int tw = MeasureT(title, FONT_LARGE);
        DrawT(title,(sw-tw)/2, sh/2-220, FONT_LARGE, NEON_CYAN);

        const char* sub = "Configure your system resources before starting NexOS";
        int sw2 = MeasureT(sub, FONT_SMALL);
        DrawT(sub,(sw-sw2)/2, sh/2-185, FONT_SMALL, TEXT_MUTED);

        // Horizontal divider
        DrawLine((sw-540)/2, sh/2-165, (sw+540)/2, sh/2-165, BORDER_DIM);

        int startY = sh/2 - 140;
        int pw = 540;
        int px = (sw-pw)/2;

        for (int fi=0; fi<3; fi++) {
            int fy = startY + fi*80;

            // Label + hint
            DrawT(fields[fi].label, px, fy-2, FONT_SMALL, TEXT_MUTED);
            int lw = MeasureT(fields[fi].label, FONT_SMALL);
            char hint[64]; sprintf(hint,"  (%s)", fields[fi].hint);
            DrawT(hint, px+lw, fy-2, FONT_TINY, TEXT_DIM);

            // Input box
            Rectangle box={(float)px,(float)(fy+20),(float)pw,46};
            DrawRectangleRec(box, BG_PANEL);
            Color borderCol = fields[fi].active ? NEON_CYAN : BORDER_DIM;
            if (fields[fi].active) DrawGlowRect(box, NEON_CYAN, 3);
            else DrawRectangleLinesEx(box, 1.0f, BORDER_DIM);

            // Value text
            DrawT(fields[fi].buf, px+14, fy+32, FONT_NORMAL, TEXT_PRIMARY);

            // Blinking cursor
            if (fields[fi].active && (int)(GetTime()*2)%2==0) {
                int cw = MeasureT(fields[fi].buf, FONT_NORMAL);
                DrawT("|", px+16+cw, fy+32, FONT_NORMAL, NEON_CYAN);
            }

            // Unit label on right
            int uw = MeasureT(fields[fi].unit, FONT_SMALL);
            DrawT(fields[fi].unit, px+pw-uw-12, fy+34, FONT_SMALL, TEXT_MUTED);

            // Active left accent
            if (fields[fi].active)
                DrawRectangle(px, fy+20, 3, 46, NEON_CYAN);
        }

        // Error message
        if (errorMsg[0]) {
            int ew = MeasureT(errorMsg, FONT_SMALL);
            DrawT(errorMsg,(sw-ew)/2, startY+3*80+10, FONT_SMALL, NEON_PINK);
        }

        // Start button
        Rectangle startBtn={(float)((sw-220)/2),(float)(startY+3*80+50),220,46};
        bool hov = CheckCollisionPointRec(GetMousePosition(),startBtn);
        DrawRectangleRec(startBtn, hov?NEON_CYAN:BG_HOVER);
        DrawGlowRect(startBtn, NEON_CYAN, hov?5:2);
        const char* btnLabel="Start NexOS";
        int blw = MeasureT(btnLabel, FONT_NORMAL);
        DrawT(btnLabel,(int)(startBtn.x+(startBtn.width-blw)/2),
              (int)(startBtn.y+13), FONT_NORMAL, hov?BG_DEEP:NEON_CYAN);

        // Click start
        if (hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            // Validate
            int ram  = atoi(fields[0].buf);
            int hdd  = atoi(fields[1].buf);
            int cors = atoi(fields[2].buf);
            if (ram  < fields[0].minVal || ram  > fields[0].maxVal)
                sprintf(errorMsg,"RAM must be %d – %d MB", fields[0].minVal, fields[0].maxVal);
            else if (hdd < fields[1].minVal || hdd > fields[1].maxVal)
                sprintf(errorMsg,"HDD must be %d – %d MB", fields[1].minVal, fields[1].maxVal);
            else if (cors < fields[2].minVal || cors > fields[2].maxVal)
                sprintf(errorMsg,"Cores must be %d – %d", fields[2].minVal, fields[2].maxVal);
            else {
                outRam = ram; outHdd = hdd; outCores = cors;
                done = true;
            }
        }

        // Footer
        DrawT("TAB to switch fields  •  Click a field to select",
              px, startY+3*80+115, FONT_TINY, TEXT_DIM);

        EndDrawing();
    }
    return done;
}

// ============================================================
//  SEARCH BAR
// ============================================================
static bool searchOpen    = false;
static char searchText[64]= "";
static int  searchLen     = 0;
static int  searchHovered = -1;
static int  filteredIdx[13], filteredCount = 0;

static void UpdateSearch() {
    filteredCount = 0;
    std::string q(searchText);
    std::transform(q.begin(),q.end(),q.begin(),::tolower);
    for (int i=0;i<APP_COUNT;i++) {
        std::string n(APPS[i].name);
        std::transform(n.begin(),n.end(),n.begin(),::tolower);
        if (q.empty()||n.find(q)!=std::string::npos) filteredIdx[filteredCount++]=i;
    }
}

static void DrawSearchOverlay(int sw, int sh) {
    if (!searchOpen) return;
    DrawRectangle(0,0,sw,sh,{0,0,0,200});
    int pw=620, itemH=52, vis=filteredCount<8?filteredCount:8;
    int ph=66+vis*itemH+20, px=(sw-pw)/2, py=sh/2-ph/2-40;
    DrawRectangle(px,py,pw,ph,BG_PANEL);
    DrawGlowRect({(float)px,(float)py,(float)pw,(float)ph},NEON_CYAN,5);

    // Search box
    Rectangle box={(float)(px+16),(float)(py+14),(float)(pw-32),38};
    DrawRectangleRec(box,BG_DEEP);
    DrawRectangleLinesEx(box,1.5f,NEON_CYAN);
    DrawT("Search:", px+22, py+23, FONT_SMALL, TEXT_MUTED);
    int lw = MeasureT("Search:", FONT_SMALL);
    DrawT(searchText, px+28+lw, py+23, FONT_SMALL, TEXT_PRIMARY);
    if ((int)(GetTime()*2)%2==0) {
        int tw = MeasureT(searchText,FONT_SMALL);
        DrawT("|", px+30+lw+tw, py+23, FONT_SMALL, NEON_CYAN);
    }

    Vector2 mouse = GetMousePosition(); searchHovered=-1;
    for (int i=0;i<filteredCount&&i<8;i++) {
        int ai=filteredIdx[i], ry=py+66+i*itemH;
        Rectangle row={(float)(px+8),(float)ry,(float)(pw-16),(float)(itemH-4)};
        bool hov=CheckCollisionPointRec(mouse,row);
        if (hov) searchHovered=i;
        DrawRectangleRec(row,hov?BG_HOVER:BG_DEEP);
        if (hov) DrawRectangleLinesEx(row,1.0f,NEON_CYAN);
        Rectangle iconR={row.x+8,row.y+7,36,36};
        DrawAppIcon(ai,iconR,false);
        DrawT(APPS[ai].name,(int)row.x+52,ry+8,FONT_NORMAL,TEXT_PRIMARY);
        char info[32]; sprintf(info,"RAM: %dMB  HDD: %dMB",APPS[ai].ram_mb,APPS[ai].hdd_mb);
        DrawT(info,(int)row.x+52,ry+27,FONT_TINY,TEXT_MUTED);
        bool isRun=false;
        for (auto& ra:runningApps) if(ra.appIndex==ai){isRun=true;break;}
        if (isRun){DrawCircle((int)(row.x+pw-30),ry+itemH/2-2,5,NEON_GREEN);DrawT("running",(int)(row.x+pw-96),ry+14,FONT_TINY,NEON_GREEN);}
    }
    if (filteredCount==0) DrawT("No apps found.",(int)(pw/2+px-60),py+90,FONT_NORMAL,TEXT_MUTED);
    DrawT("ESC close  •  Enter or click to launch",px+18,py+ph-16,FONT_TINY,TEXT_DIM);
}

static void HandleSearchInput() {
    if (!searchOpen) return;
    int k=GetCharPressed();
    while(k>0){if(searchLen<62&&k>=32){searchText[searchLen++]=(char)k;searchText[searchLen]='\0';UpdateSearch();}k=GetCharPressed();}
    if (IsKeyPressed(KEY_BACKSPACE)&&searchLen>0){searchText[--searchLen]='\0';UpdateSearch();}
    if (IsKeyPressed(KEY_ESCAPE)){searchOpen=false;searchLen=0;searchText[0]='\0';UpdateSearch();return;}
    if (IsKeyPressed(KEY_ENTER)&&filteredCount>0){LaunchApp(filteredIdx[0]);searchOpen=false;searchLen=0;searchText[0]='\0';UpdateSearch();}
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)&&searchHovered>=0){LaunchApp(filteredIdx[searchHovered]);searchOpen=false;searchLen=0;searchText[0]='\0';UpdateSearch();}
}

// ============================================================
//  DOCK (left sidebar)
// ============================================================
static int hoveredDockIcon = -1;
static void DrawDock(int sh) {
    int iconSz=48, pad=8;
    int totalH=APP_COUNT*(iconSz+pad);
    int startY=(sh-TASKBAR_H-totalH)/2; if(startY<4)startY=4;
    DrawRectangle(0,0,66,sh-TASKBAR_H,{10,10,22,200});
    DrawLine(66,0,66,sh-TASKBAR_H,{0,255,200,22});
    Vector2 mouse=GetMousePosition(); hoveredDockIcon=-1;
    for (int i=0;i<APP_COUNT;i++) {
        int iy=startY+i*(iconSz+pad);
        Rectangle r={8,(float)iy,(float)iconSz,(float)iconSz};
        bool hov=CheckCollisionPointRec(mouse,r);
        if(hov)hoveredDockIcon=i;
        DrawAppIcon(i,r,hov);
        bool isRun=false;
        for(auto& ra:runningApps)if(ra.appIndex==i){isRun=true;break;}
        if(isRun) DrawCircle((int)(r.x+r.width/2),(int)(r.y+r.height+3),3,NEON_CYAN);
        if(hov){
            int tw=MeasureT(APPS[i].name,FONT_TINY);
            DrawRectangle(72,iy+iconSz/2-12,tw+12,22,BG_PANEL);
            DrawRectangleLinesEx({72,(float)(iy+iconSz/2-12),(float)(tw+12),22},1,NEON_CYAN);
            DrawT(APPS[i].name,76,iy+iconSz/2-8,FONT_TINY,TEXT_PRIMARY);
        }
    }
}

// ============================================================
//  TASKBAR
// ============================================================
static bool kernelModeActive=false;
static void DrawTaskbar(int sw,int sh,float ramFrac,float hddFrac,int ramMB,int hddMB) {
    int y=sh-TASKBAR_H;
    DrawRectangle(0,y,sw,TASKBAR_H,BG_TASKBAR);
    DrawLine(0,y,sw,y,NEON_CYAN);

    // Logo
    DrawT("NexOS",10,y+12,FONT_NORMAL,NEON_CYAN);

    // Search button
    Rectangle sb={86,(float)(y+8),160,(float)(TASKBAR_H-16)};
    bool shov=CheckCollisionPointRec(GetMousePosition(),sb);
    DrawRectangleRec(sb,shov?BG_HOVER:BG_ICON);
    DrawRectangleLinesEx(sb,1.0f,shov?NEON_CYAN:BORDER_DIM);
    DrawT("Search apps...",94,y+15,FONT_TINY,TEXT_MUTED);
    if(shov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){searchOpen=true;UpdateSearch();}

    // Running pills
    int pillX=256;
    for(auto& ra:runningApps){
        const char* nm=APPS[ra.appIndex].name;
        int pw=MeasureT(nm,FONT_TINY)+18;
        if(pillX+pw>sw-340)break;
        Rectangle pill={(float)pillX,(float)(y+9),(float)pw,(float)(TASKBAR_H-18)};
        DrawRectangleRec(pill,ra.minimized?BG_PANEL:BG_HOVER);
        DrawRectangleLinesEx(pill,1.0f,ra.minimized?BORDER_DIM:NEON_CYAN);
        DrawT(nm,pillX+8,y+15,FONT_TINY,ra.minimized?TEXT_MUTED:TEXT_PRIMARY);
        if(CheckCollisionPointRec(GetMousePosition(),pill)&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)&&ra.minimized)
            MinimizeApp(ra.pid,false);
        pillX+=pw+4;
    }

    // Right: RAM, HDD, Clock, Kernel indicator
    int rx=sw-350;
    // RAM
    DrawT("RAM",rx,y+8,FONT_TINY,TEXT_MUTED);
    DrawProgressBar({(float)(rx+34),(float)(y+11),80,9},ramFrac,NEON_GOLD,BG_PANEL);
    char rp[20]; sprintf(rp,"%dMB",(int)(ramFrac*ramMB)); DrawT(rp,rx+118,y+8,FONT_TINY,TEXT_MUTED);
    // HDD
    DrawT("HDD",rx+170,y+8,FONT_TINY,TEXT_MUTED);
    DrawProgressBar({(float)(rx+204),(float)(y+11),65,9},hddFrac,NEON_PURPLE,BG_PANEL);
    // Clock
    time_t now=time(nullptr); struct tm* t=localtime(&now);
    char clk[16]; strftime(clk,sizeof(clk),"%H:%M:%S",t);
    DrawT(clk,rx+278,y+12,FONT_NORMAL,NEON_CYAN);
    // Kernel badge
    if(kernelModeActive){DrawRectangle(rx-98,y+7,90,TASKBAR_H-14,{140,60,220,50});DrawT("KERNEL",rx-90,y+14,FONT_TINY,NEON_PURPLE);}
}

// ============================================================
//  KERNEL MODE OVERLAY
//  Password: abcd123
//  Includes live PCB table + Kernel Monitor launch button
//  + RAM/CPU live graphs
// ============================================================
static bool showKernelMode=false,kernelUnlocked=false,typingPass=false;
static char kernelPass[32]="";
static char wrongPassMsg[32]="";

// Simple history arrays for live graphs (last 30 samples)
static float ramHistory[30]={};
static float cpuHistory[30]={};
static int   histIdx=0;
static double lastHistUpdate=0;

static void UpdateKernelGraphs() {
    if(!sharedRes) return;
    double now=GetTime();
    if(now-lastHistUpdate<1.0) return;
    lastHistUpdate=now;
    sem_wait(shm_sem);
    float ru=sharedRes->total_ram_mb>0?(float)sharedRes->used_ram_mb/sharedRes->total_ram_mb:0;
    int run=0;
    for(int i=0;i<sharedRes->process_count;i++)
        if(sharedRes->processes[i].state==STATE_RUNNING)run++;
    float cu=sharedRes->total_cores>0?(float)run/sharedRes->total_cores:0;
    sem_post(shm_sem);
    ramHistory[histIdx]=ru;
    cpuHistory[histIdx]=cu;
    histIdx=(histIdx+1)%30;
}

static void DrawMiniGraph(int x,int y,int w,int h,float* history,int count,int head,Color lineColor,const char* label) {
    // Background
    DrawRectangle(x,y,w,h,BG_DEEP);
    DrawRectangleLinesEx({(float)x,(float)y,(float)w,(float)h},1.0f,BORDER_DIM);
    // Grid lines
    for(int i=1;i<4;i++) DrawLine(x,y+h*i/4,x+w,y+h*i/4,{40,40,80,120});
    // Plot line
    for(int i=0;i<count-1;i++){
        int ai=(head+i)%count, bi=(head+i+1)%count;
        float va=history[ai], vb=history[bi];
        int x0=x+i*(w/(count-1));
        int x1=x+(i+1)*(w/(count-1));
        int y0=y+h-(int)(va*h);
        int y1=y+h-(int)(vb*h);
        DrawLine(x0,y0,x1,y1,lineColor);
        DrawCircle(x1,y1,2,lineColor);
    }
    // Label
    DrawT(label,x+4,y+4,FONT_TINY,lineColor);
    // Current value
    float cur=history[(head+count-1)%count];
    char pct[12]; sprintf(pct,"%.0f%%",cur*100);
    int pw=MeasureT(pct,FONT_TINY);
    DrawT(pct,x+w-pw-4,y+4,FONT_TINY,lineColor);
}

static void DrawKernelMode(int sw,int sh)
{
    if(!showKernelMode) return;
    DrawRectangle(0,0,sw,sh,{0,0,0,190});

    int pw=780,ph=560,px=(sw-pw)/2,py=(sh-ph)/2;
    Rectangle panel={(float)px,(float)py,(float)pw,(float)ph};
    DrawRectangleRec(panel,BG_PANEL);
    DrawGlowRect(panel,NEON_PURPLE,6);

    // Title bar
    DrawRectangle(px,py,pw,36,BG_TITLEBAR);
    DrawT("KERNEL MODE",px+16,py+10,FONT_LARGE,NEON_PURPLE);
    DrawLine(px,py+36,px+pw,py+36,BORDER_DIM);

    // Close button
    if(DrawButton({(float)(px+pw-38),(float)(py+7),28,22},"X",BG_DEEP,NEON_PINK,FONT_SMALL))
        {showKernelMode=false;kernelUnlocked=false;kernelModeActive=false;memset(kernelPass,0,32);memset(wrongPassMsg,0,32);}

    if(!kernelUnlocked) {
        // ── PASSWORD SCREEN ──────────────────────────────────
        const char* prompt="Enter Kernel Password to Continue";
        int prw=MeasureT(prompt,FONT_NORMAL);
        DrawT(prompt,(sw-prw)/2,py+90,FONT_NORMAL,TEXT_PRIMARY);

        // Lock icon drawn as symbol
        DrawRectangle(sw/2-16,py+125,32,22,TEXT_MUTED);
        DrawRectangleLines(sw/2-20,py+143,40,28,TEXT_MUTED);
        DrawT("LOCKED",sw/2-MeasureT("LOCKED",FONT_TINY)/2,py+149,FONT_TINY,BG_PANEL);

        Rectangle pb={(float)((sw-360)/2),(float)(py+185),360,42};
        DrawRectangleRec(pb,BG_DEEP);
        DrawRectangleLinesEx(pb,1.5f,typingPass?NEON_PURPLE:BORDER_DIM);
        if(typingPass) DrawGlowRect(pb,NEON_PURPLE,3);

        std::string stars(strlen(kernelPass),'●');
        DrawT(stars.c_str(),(int)pb.x+14,(int)pb.y+12,FONT_NORMAL,TEXT_PRIMARY);
        if(typingPass&&(int)(GetTime()*2)%2==0){
            int sw2=MeasureT(stars.c_str(),FONT_NORMAL);
            DrawT("|",(int)pb.x+16+sw2,(int)pb.y+12,FONT_NORMAL,NEON_PURPLE);
        }

        DrawT("Click here to type password",(int)pb.x+14,py+234,FONT_TINY,TEXT_DIM);

        if(IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) typingPass=CheckCollisionPointRec(GetMousePosition(),pb);
        if(typingPass){
            int k=GetCharPressed();
            while(k>0){int l=strlen(kernelPass);if(l<31&&k>=32){kernelPass[l]=(char)k;kernelPass[l+1]='\0';}k=GetCharPressed();}
            if(IsKeyPressed(KEY_BACKSPACE)&&strlen(kernelPass)>0)kernelPass[strlen(kernelPass)-1]='\0';
            if(IsKeyPressed(KEY_ENTER)){
                if(strcmp(kernelPass,"abcd123")==0){kernelUnlocked=true;kernelModeActive=true;memset(wrongPassMsg,0,32);Log("Kernel mode unlocked");}
                else{memset(kernelPass,0,32);strcpy(wrongPassMsg,"Incorrect password. Try again.");}
            }
        }

        if(DrawButton({(float)((sw-200)/2),(float)(py+255),200,40},"UNLOCK",BG_HOVER,NEON_PURPLE,FONT_NORMAL)){
            typingPass=false;
            if(strcmp(kernelPass,"abcd123")==0){kernelUnlocked=true;kernelModeActive=true;memset(wrongPassMsg,0,32);Log("Kernel unlocked");}
            else{memset(kernelPass,0,32);strcpy(wrongPassMsg,"Incorrect password. Try again.");}
        }
        if(wrongPassMsg[0]){
            int ew=MeasureT(wrongPassMsg,FONT_SMALL);
            DrawT(wrongPassMsg,(sw-ew)/2,py+308,FONT_SMALL,NEON_PINK);
        }

    } else {
        // ── UNLOCKED: PCB TABLE + GRAPHS ────────────────────

        // Two-column layout:
        // Left: process table   Right: live graphs
        int tableW = pw-230, graphW=200, graphX=px+tableW+14;

        // --- PCB TABLE ---
        DrawT("PROCESS TABLE",px+12,py+46,FONT_SMALL,TEXT_MUTED);
        const char* cols[]={"PID","NAME","STATE","RAM","PRI","Q"};
        int cx[]={px+12,px+72,px+210,px+310,px+388,px+428};
        for(int i=0;i<6;i++) DrawT(cols[i],cx[i],py+66,FONT_TINY,TEXT_MUTED);
        DrawLine(px+8,py+82,px+tableW-4,py+82,BORDER_DIM);

        const char* stN[]={"NEW","READY","RUNNING","BLOCKED","DONE"};
        Color stC[]={TEXT_MUTED,NEON_GOLD,NEON_GREEN,NEON_PINK,TEXT_DIM};

        if(sharedRes){
            sem_wait(shm_sem);
            int maxRows=12;
            for(int i=0;i<sharedRes->process_count&&i<maxRows;i++){
                PCB& p=sharedRes->processes[i];
                int ry=py+90+i*28;
                Color rowColor = i%2==0 ? BG_DEEP : Color{22,22,46,255};
                DrawRectangle(px+8,ry-2,tableW-16,26,rowColor);
                char ps[12],rs[12],prs[4],qs[4];
                sprintf(ps,"%d",p.pid);sprintf(rs,"%dM",p.ram_mb);
                sprintf(prs,"%d",p.priority);sprintf(qs,"L%d",p.queue_level);
                DrawT(ps,cx[0],ry+4,FONT_TINY,TEXT_PRIMARY);
                // Truncate name if too long
                char shortName[16]; strncpy(shortName,p.name,15); shortName[15]='\0';
                DrawT(shortName,cx[1],ry+4,FONT_TINY,TEXT_PRIMARY);
                int si=p.state<5?p.state:0;
                DrawT(stN[si],cx[2],ry+4,FONT_TINY,stC[si]);
                DrawT(rs,cx[3],ry+4,FONT_TINY,TEXT_MUTED);
                DrawT(prs,cx[4],ry+4,FONT_TINY,TEXT_MUTED);
                DrawT(qs,cx[5],ry+4,FONT_TINY,TEXT_MUTED);
                // Kill btn
                if(DrawButton({(float)(px+tableW-60),(float)(ry+1),52,20},"KILL",BG_DEEP,NEON_PINK,FONT_TINY))
                    KillApp(p.pid);
            }
            if(sharedRes->process_count==0)
                DrawT("No processes running.",px+20,py+140,FONT_NORMAL,TEXT_MUTED);

            // Summary stats
            int statsY=py+ph-90;
            DrawLine(px+8,statsY,px+tableW-4,statsY,BORDER_DIM);
            char stat1[48],stat2[48],stat3[48];
            sprintf(stat1,"Processes: %d / %d",sharedRes->process_count,MAX_PROCESSES);
            sprintf(stat2,"RAM Used: %dMB / %dMB",sharedRes->used_ram_mb,sharedRes->total_ram_mb);
            sprintf(stat3,"Cores: %d total",sharedRes->total_cores);
            DrawT(stat1,px+12,statsY+8, FONT_TINY,TEXT_MUTED);
            DrawT(stat2,px+12,statsY+24,FONT_TINY,TEXT_MUTED);
            DrawT(stat3,px+12,statsY+40,FONT_TINY,TEXT_MUTED);

            if(sharedRes->deadlock_detected){
                DrawRectangle(px+8,py+ph-44,tableW-16,32,{255,45,120,30});
                DrawT(sharedRes->deadlock_msg,px+14,py+ph-36,FONT_TINY,NEON_PINK);
            }
            sem_post(shm_sem);
        }

        // --- RIGHT PANEL: live graphs ---
        DrawLine(px+tableW,py+42,px+tableW,py+ph-8,BORDER_DIM);
        DrawT("LIVE MONITOR",graphX,py+46,FONT_SMALL,TEXT_MUTED);

        // RAM graph
        DrawT("RAM Usage",graphX,py+68,FONT_TINY,NEON_GOLD);
        DrawMiniGraph(graphX,py+84,graphW-8,60,ramHistory,30,histIdx,NEON_GOLD,"RAM");

        // CPU graph
        DrawT("CPU Usage",graphX,py+158,FONT_TINY,NEON_GREEN);
        DrawMiniGraph(graphX,py+174,graphW-8,60,cpuHistory,30,histIdx,NEON_GREEN,"CPU");

        // Aging info
        DrawT("PRIORITY AGING",graphX,py+248,FONT_TINY,NEON_PURPLE);
        DrawRectangle(graphX,py+264,graphW-8,100,BG_DEEP);
        DrawRectangleLinesEx({(float)graphX,(float)(py+264),(float)(graphW-8),100},1,BORDER_DIM);
        if(sharedRes){
            sem_wait(shm_sem);
            int ay=py+272; int shown=0;
            for(int i=0;i<sharedRes->process_count&&shown<3;i++){
                PCB& p=sharedRes->processes[i];
                if(p.state==STATE_READY){
                    char ageStr[32];
                    int waited=(int)(time(nullptr)-p.ready_since);
                    sprintf(ageStr,"%-8s P%d W:%ds",p.name,p.priority,waited);
                    DrawT(ageStr,graphX+4,ay,FONT_TINY,waited>10?NEON_ORANGE:TEXT_MUTED);
                    ay+=22; shown++;
                }
            }
            if(shown==0) DrawT("No waiting processes",graphX+4,py+300,FONT_TINY,TEXT_DIM);
            sem_post(shm_sem);
        }

        // Launch Kernel Visualizer button
        DrawT("KERNEL VISUALIZER",graphX,py+378,FONT_TINY,TEXT_MUTED);
        if(DrawButton({(float)graphX,(float)(py+394),(float)(graphW-8),34},"Launch Full View",BG_HOVER,NEON_PURPLE,FONT_TINY))
            LaunchKernelVis();

        // Lock button
        if(DrawButton({(float)(px+pw-148),(float)(py+ph-42),138,30},"LOCK KERNEL",BG_DEEP,NEON_PURPLE,FONT_TINY))
            {kernelUnlocked=false;kernelModeActive=false;memset(kernelPass,0,32);Log("Kernel locked");}
    }
}

// ============================================================
//  BOOT ANIMATION
// ============================================================
static void RunBootAnimation(int& sw,int& sh)
{
    const char* lines[]={
        "Initializing hardware resources...",
        "Loading kernel modules...",
        "Setting up IPC message queues...",
        "Mounting virtual file system...",
        "Starting background daemons...",
        "NexOS ready."
    };
    int lineCount=6,shownLines=0,frame=0;
    float progress=0,alpha=0;
    while(!WindowShouldClose()&&progress<1.0f){
        progress+=0.004f; alpha=fminf(alpha+0.025f,1.0f); frame++;
        if(frame%38==0&&shownLines<lineCount)shownLines++;
        if(IsWindowResized()){sw=GetScreenWidth();sh=GetScreenHeight();}
        BeginDrawing(); ClearBackground(BG_DEEP); DrawCyberpunkGrid(sw,sh);
        const char* osn="NexOS"; int nw=MeasureT(osn,80);
        // Glow behind text
        DrawRectangle((sw-nw)/2-20,sh/2-166,nw+40,90,{0,255,200,8});
        DrawT(osn,(sw-nw)/2,sh/2-158,80,{0,255,200,(unsigned char)(int)(alpha*255)});
        const char* tag="A Multi-Process Operating System Simulator";
        int tw=MeasureT(tag,FONT_SMALL);
        DrawT(tag,(sw-tw)/2,sh/2-58,FONT_SMALL,{140,60,220,(unsigned char)(int)(alpha*200)});
        int bx=sw/2-270,bw=540,by=sh/2-18,bh=6;
        DrawRectangle(bx,by,bw,bh,BG_PANEL);
        DrawRectangle(bx,by,(int)(bw*progress),bh,NEON_CYAN);
        int tip=bx+(int)(bw*progress); DrawRectangle(tip-4,by-3,8,bh+6,{0,255,200,80});
        char pct[8]; sprintf(pct,"%d%%",(int)(progress*100)); DrawT(pct,bx+bw+10,by-5,FONT_TINY,TEXT_MUTED);
        for(int i=0;i<shownLines;i++) DrawT(lines[i],bx,by+22+i*20,FONT_TINY,i==shownLines-1?NEON_CYAN:TEXT_MUTED);
        EndDrawing();
    }
}

// ============================================================
//  SHUTDOWN
// ============================================================
static void RunShutdown(int& sw,int& sh)
{
    float a=0;
    while(!WindowShouldClose()&&a<1.0f){
        a+=0.012f; if(IsWindowResized()){sw=GetScreenWidth();sh=GetScreenHeight();}
        BeginDrawing(); ClearBackground(BG_DEEP); DrawCyberpunkGrid(sw,sh);
        const char* msg="Shutting down NexOS..."; int mw=MeasureT(msg,FONT_LARGE);
        DrawT(msg,(sw-mw)/2,sh/2-30,FONT_LARGE,{0,255,200,(unsigned char)(int)((1-a*0.4f)*255)});
        const char* bye="Thank you for using NexOS. Goodbye."; int bw2=MeasureT(bye,FONT_NORMAL);
        DrawT(bye,(sw-bw2)/2,sh/2+24,FONT_NORMAL,{140,60,220,(unsigned char)(int)(a*220)});
        EndDrawing();
    }
}

static void Shutdown()
{
    Log("NexOS shutting down");
    for(auto& ra:runningApps)kill(ra.pid,SIGTERM);
    for(auto& ra:runningApps)waitpid(ra.pid,nullptr,0);
    if(sharedRes)sharedRes->shutdown_requested=true; sleep(1);
    if(shm_sem){sem_close(shm_sem);sem_unlink(SEM_NAME);}
    if(shmid>=0)shmctl(shmid,IPC_RMID,nullptr);
    if(mqid>=0) msgctl(mqid,IPC_RMID,nullptr);
    if(logFile) fclose(logFile);
}

// ============================================================
//  MAIN
// ============================================================
int main()
{
    // Open window first so we can show the input screen
    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(1280,720,"NexOS — Hardware Setup");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);

    // Load custom font (put a .ttf in assets/fonts/ folder)
    // Falls back to raylib default if not found.
    gFontLoaded = false;
    if (FileExists("assets/fonts/JetBrainsMono-Regular.ttf")) {
        gFont = LoadFontEx("assets/fonts/JetBrainsMono-Regular.ttf", 32, nullptr, 0);
        gFontLoaded = (gFont.texture.id > 0);
    } else if (FileExists("assets/fonts/Roboto-Regular.ttf")) {
        gFont = LoadFontEx("assets/fonts/Roboto-Regular.ttf", 32, nullptr, 0);
        gFontLoaded = (gFont.texture.id > 0);
    } else if (FileExists("assets/fonts/Ubuntu-R.ttf")) {
        gFont = LoadFontEx("assets/fonts/Ubuntu-R.ttf", 32, nullptr, 0);
        gFontLoaded = (gFont.texture.id > 0);
    }
    if (gFontLoaded) SetTextureFilter(gFont.texture, TEXTURE_FILTER_BILINEAR);

    int sw=GetScreenWidth(), sh=GetScreenHeight();

    // ── STEP 1: Hardware input screen ──────────────────────
    int ramMB=2048, hddMB=262144, cores=8;
    if (!RunHardwareInputScreen(ramMB,hddMB,cores)) {
        CloseWindow(); return 0; // user closed window
    }

    SetWindowTitle("NexOS");

    // ── STEP 2: Init log, shared memory, IPC ───────────────
    logFile=fopen("logs/nexos.log","a");

    shmid=shmget(SHM_KEY,sizeof(OSResources),0666);
    if(shmid>=0)shmctl(shmid,IPC_RMID,nullptr);
    shmid=shmget(SHM_KEY,sizeof(OSResources),IPC_CREAT|0666);
    sharedRes=(OSResources*)shmat(shmid,nullptr,0);
    memset(sharedRes,0,sizeof(OSResources));
    sharedRes->total_ram_mb=ramMB; sharedRes->total_hdd_mb=hddMB;
    sharedRes->total_cores=cores;  sharedRes->logging_enabled=true;

    sem_unlink(SEM_NAME);
    shm_sem=sem_open(SEM_NAME,O_CREAT|O_EXCL,0666,1);
    msgctl(msgget(MSG_KEY,0666),IPC_RMID,nullptr);
    mqid=msgget(MSG_KEY,IPC_CREAT|0666);

    Log("NexOS booting. RAM:%dMB HDD:%dMB Cores:%d",ramMB,hddMB,cores);

    // ── STEP 3: Start background threads ───────────────────
    pthread_t tR,tS,tA,tD;
    pthread_create(&tR,nullptr,ResourceManagerThread,nullptr);
    pthread_create(&tS,nullptr,SchedulerThread,nullptr);
    pthread_create(&tA,nullptr,AgingThread,nullptr);
    pthread_create(&tD,nullptr,DeadlockThread,nullptr);

    // ── STEP 4: Load icons ─────────────────────────────────
    for(int i=0;i<APP_COUNT;i++){
        iconLoaded[i]=false;
        if(FileExists(APPS[i].iconFile)){
            iconTextures[i]=LoadTexture(APPS[i].iconFile);
            iconLoaded[i]=(iconTextures[i].id>0);
        }
    }

    UpdateSearch();

    // ── STEP 5: Boot animation ─────────────────────────────
    RunBootAnimation(sw,sh);

    // ── MAIN LOOP ──────────────────────────────────────────
    bool running=true;
    static double lastClickTime=0; static int lastClickIcon=-1;

    while(!WindowShouldClose()&&running){
        if(IsWindowResized()){sw=GetScreenWidth();sh=GetScreenHeight();}
        ReapChildren();

        // Update kernel graphs every second
        if(showKernelMode&&kernelUnlocked) UpdateKernelGraphs();

        // Global keys
        if(IsKeyPressed(KEY_ESCAPE)){
            if(searchOpen){searchOpen=false;searchLen=0;searchText[0]='\0';UpdateSearch();}
            else if(showKernelMode){showKernelMode=false;kernelUnlocked=false;kernelModeActive=false;}
            else running=false;
        }
        if(IsKeyPressed(KEY_K)&&!searchOpen&&!showKernelMode) showKernelMode=true;

        // Any printable key opens search
        if(!searchOpen&&!showKernelMode){
            int k=GetCharPressed();
            if(k>31){searchOpen=true;searchText[0]=(char)k;searchText[1]='\0';searchLen=1;UpdateSearch();}
        }

        // Dock double-click to launch
        if(!searchOpen&&!showKernelMode&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
            double now=GetTime();
            if(hoveredDockIcon>=0){
                if(hoveredDockIcon==lastClickIcon&&now-lastClickTime<0.4){LaunchApp(hoveredDockIcon);lastClickTime=0;lastClickIcon=-1;}
                else{lastClickTime=now;lastClickIcon=hoveredDockIcon;}
            }
        }

        HandleSearchInput();

        float ramFrac=0,hddFrac=0;
        if(sharedRes&&sharedRes->total_ram_mb>0){
            ramFrac=(float)sharedRes->used_ram_mb/sharedRes->total_ram_mb;
            hddFrac=(float)sharedRes->used_hdd_mb/sharedRes->total_hdd_mb;
        }

        // ── DRAW ──────────────────────────────────────────
        BeginDrawing();
        ClearBackground(BG_DEEP);
        DrawCyberpunkGrid(sw,sh);

        DrawDock(sh);
        DrawTaskbar(sw,sh,ramFrac,hddFrac,ramMB,hddMB);

        // Desktop hint
        DrawT("Type to search apps  •  [K] Kernel Mode  •  [ESC] Shutdown",
              72,sh-TASKBAR_H-22,FONT_TINY,TEXT_DIM);

        // Overlays (always on top)
        DrawSearchOverlay(sw,sh);
        DrawKernelMode(sw,sh);

        EndDrawing();
    }

    for(int i=0;i<APP_COUNT;i++) if(iconLoaded[i]) UnloadTexture(iconTextures[i]);
    if(gFontLoaded) UnloadFont(gFont);

    RunShutdown(sw,sh);
    Shutdown();
    CloseWindow();
    return 0;
}