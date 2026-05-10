// ============================================================
//  NexOS — Browser Launcher
//  Detects installed browsers, launches them via fork/exec,
//  URL bar, quick bookmarks, history saved to hdd/
//  Fully follows NexOS IPC + theme pattern.
// ============================================================
#include "raylib.h"
#include "../include/theme.h"
#include "../include/resources.h"
#include "../include/ipc.h"

#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#define APP_NAME  "Browser"
#define RAM_MB    30
#define HDD_MB    5
#define WIN_W     860
#define WIN_H     600

#define HISTORY_PATH  "hdd/browser_history.txt"
#define MAX_HISTORY   40

// ── Browser descriptor ────────────────────────────────────
struct BrowserDef {
    const char* name;
    const char* binary;    // what execvp gets
    const char* which;     // what `which` checks for
    const char* icon;      // single letter fallback
    Color        color;
};

static const BrowserDef BROWSERS[] = {
    { "Chromium",  "chromium-browser",  "chromium-browser",  "C", {100, 180, 255, 255} },
    { "Chromium",  "chromium",          "chromium",          "C", {100, 180, 255, 255} },
    { "Chrome",    "google-chrome",     "google-chrome",     "G", {110, 200, 130, 255} },
    { "Firefox",   "firefox",           "firefox",           "F", {255, 160,  80, 255} },
    { "Firefox",   "firefox-esr",       "firefox-esr",       "F", {255, 160,  80, 255} },
    { "Brave",     "brave-browser",     "brave-browser",     "B", {255, 120, 100, 255} },
    { "Edge",      "microsoft-edge",    "microsoft-edge",    "E", { 80, 160, 220, 255} },
    { "Opera",     "opera",             "opera",             "O", {200,  80, 100, 255} },
    { "Vivaldi",   "vivaldi",           "vivaldi",           "V", {160,  80, 200, 255} },
    { "Epiphany",  "epiphany-browser",  "epiphany-browser",  "E", {120, 200, 180, 255} },
};
static const int BROWSER_DEF_COUNT = 10;

// ── Quick bookmarks ───────────────────────────────────────
struct Bookmark {
    const char* label;
    const char* url;
    Color        color;
};

static const Bookmark BOOKMARKS[] = {
    { "Google",      "https://www.google.com",          {100, 180, 255, 255} },
    { "YouTube",     "https://www.youtube.com",         {220,  80,  80, 255} },
    { "GitHub",      "https://www.github.com",          {170, 170, 190, 255} },
    { "Wikipedia",   "https://www.wikipedia.org",       {160, 200, 220, 255} },
    { "Reddit",      "https://www.reddit.com",          {255, 140,  80, 255} },
    { "Stack Overflow","https://stackoverflow.com",     {255, 180,  60, 255} },
    { "HackerNews",  "https://news.ycombinator.com",    {255, 150,  80, 255} },
    { "DuckDuckGo",  "https://www.duckduckgo.com",      {220, 120, 200, 255} },
};
static const int BOOKMARK_COUNT = 8;

// ── State ─────────────────────────────────────────────────
struct InstalledBrowser {
    std::string name;
    std::string binary;
    Color        color;
    char         icon;
};

static std::vector<InstalledBrowser> installed;
static int   selectedBrowser  = 0;   // index into installed
static bool  appRunning       = true;
static float animTime         = 0.0f;

// URL bar
static char  urlBuf[512]      = "https://";
static int   urlLen           = 8;
static bool  urlFocused       = true;

// History
struct HistEntry {
    std::string url;
    std::string browser;
    std::string timeStr;
};
static std::vector<HistEntry> history;
static int   histScroll       = 0;

// Status
static char  statusMsg[128]   = "";
static double statusAt        = -999;
static Color statusColor      = {100, 180, 200, 255};

// Launch flash
static double lastLaunchAt    = -999;
static std::string lastLaunchedUrl = "";

// ── Helpers ───────────────────────────────────────────────
static bool BinaryExists(const char* bin) {
    char cmd[128];
    snprintf(cmd, 128, "which %s > /dev/null 2>&1", bin);
    return system(cmd) == 0;
}

static void SetStatus(const char* m, Color c = {100,180,200,255}) {
    strncpy(statusMsg, m, 127);
    statusAt = GetTime();
    statusColor = c;
}

static std::string NowString() {
    time_t t = time(nullptr);
    char buf[32];
    strftime(buf, 32, "%H:%M  %d/%m", localtime(&t));
    return std::string(buf);
}

static void TrimUrl(std::string& s) {
    while (!s.empty() && (s.back()=='\n'||s.back()=='\r'||s.back()==' '))
        s.pop_back();
}

// ── Detect installed browsers ─────────────────────────────
static void DetectBrowsers() {
    installed.clear();
    std::vector<std::string> seen; // avoid duplicates by name
    for (int i = 0; i < BROWSER_DEF_COUNT; i++) {
        if (!BinaryExists(BROWSERS[i].which)) continue;
        // Deduplicate by name
        bool dup = false;
        for (auto& s : seen) if (s == BROWSERS[i].name) { dup=true; break; }
        if (dup) continue;
        seen.push_back(BROWSERS[i].name);
        InstalledBrowser b;
        b.name   = BROWSERS[i].name;
        b.binary = BROWSERS[i].binary;
        b.color  = BROWSERS[i].color;
        b.icon   = BROWSERS[i].icon[0];
        installed.push_back(b);
    }
}

// ── History I/O ───────────────────────────────────────────
static void LoadHistory() {
    history.clear();
    mkdir("hdd", 0755);
    FILE* f = fopen(HISTORY_PATH, "r");
    if (!f) return;
    char line[600];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back()=='\n'||s.back()=='\r')) s.pop_back();
        // Format: TIME|BROWSER|URL
        size_t p1 = s.find('|'), p2 = s.find('|', p1+1);
        if (p1==std::string::npos || p2==std::string::npos) continue;
        HistEntry e;
        e.timeStr = s.substr(0, p1);
        e.browser = s.substr(p1+1, p2-p1-1);
        e.url     = s.substr(p2+1);
        history.push_back(e);
    }
    fclose(f);
    // Keep newest on top for display — file is oldest-first
    std::reverse(history.begin(), history.end());
}

static void SaveHistoryEntry(const std::string& url, const std::string& browser) {
    mkdir("hdd", 0755);
    // Load existing, append, trim to MAX_HISTORY
    FILE* f = fopen(HISTORY_PATH, "a");
    if (!f) return;
    fprintf(f, "%s|%s|%s\n", NowString().c_str(), browser.c_str(), url.c_str());
    fclose(f);
    // Reload into memory
    LoadHistory();
    if ((int)history.size() > MAX_HISTORY) {
        history.resize(MAX_HISTORY);
        // Re-write trimmed file
        FILE* fw = fopen(HISTORY_PATH, "w");
        if (fw) {
            // Write oldest-first
            for (int i=(int)history.size()-1;i>=0;i--)
                fprintf(fw,"%s|%s|%s\n",history[i].timeStr.c_str(),history[i].browser.c_str(),history[i].url.c_str());
            fclose(fw);
        }
    }
}

// ── Launch browser ────────────────────────────────────────
static void LaunchUrl(const std::string& url) {
    if (installed.empty()) {
        SetStatus("No browser detected on system.", {220,100,100,255});
        return;
    }
    if (url.empty() || url == "https://" || url == "http://") {
        SetStatus("Enter a URL first.", {220,180,80,255});
        return;
    }

    auto& br = installed[selectedBrowser];

    pid_t pid = fork();
    if (pid < 0) {
        SetStatus("fork() failed.", {220,100,100,255});
        return;
    }
    if (pid == 0) {
        // Child: exec the browser
        // Detach from parent process group so it survives NexOS exit
        setsid();
        const char* args[] = { br.binary.c_str(), url.c_str(), nullptr };
        execvp(br.binary.c_str(), (char* const*)args);
        _exit(1); // exec failed
    }
    // Parent: don't wait — browser runs independently
    // Reap zombie immediately with WNOHANG
    waitpid(pid, nullptr, WNOHANG);

    SaveHistoryEntry(url, br.name);
    lastLaunchAt = GetTime();
    lastLaunchedUrl = url;

    char msg[128];
    snprintf(msg, 128, "Launched %s", br.name.c_str());
    SetStatus(msg, {100,220,180,255});
}

// ── Validate / normalise URL ──────────────────────────────
static std::string NormaliseUrl(const std::string& raw) {
    std::string u = raw;
    TrimUrl(u);
    if (u.empty()) return u;
    // If no scheme, prepend https://
    if (u.substr(0,7) != "http://" && u.substr(0,8) != "https://")
        u = "https://" + u;
    return u;
}

// ── Draw helpers ──────────────────────────────────────────
static void DrawTxtC(const char* t,int cx,int y,int sz,Color c){
    DrawText(t,cx-MeasureText(t,sz)/2,y,sz,c);
}

static bool NiceButton(Rectangle r, const char* label, Color accent) {
    bool hov = CheckCollisionPointRec(GetMousePosition(), r);
    Color bg  = hov ? Color{(unsigned char)std::min(255,(int)accent.r/3+20),
                            (unsigned char)std::min(255,(int)accent.g/3+20),
                            (unsigned char)std::min(255,(int)accent.b/3+20),255}
                    : Color{14,18,30,255};
    DrawRectangleRounded(r, 0.25f, 8, bg);
    DrawRectangleLinesEx(r, hov?1.6f:1.0f,
        {accent.r,accent.g,accent.b,(unsigned char)(hov?220:120)});
    int tw = MeasureText(label, FONT_SMALL);
    DrawText(label,(int)(r.x+(r.width-tw)/2),(int)(r.y+(r.height-FONT_SMALL)/2),
             FONT_SMALL, hov?Color{240,245,250,255}:Color{160,180,195,230});
    return hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
}

// ── Sections ──────────────────────────────────────────────
static void DrawBrowserPicker(int sw, int topY) {
    // Label
    DrawText("Browser", 20, topY, FONT_TINY, {80,105,120,200});

    if (installed.empty()) {
        DrawText("No supported browser found on this system.",
                 20, topY+18, FONT_SMALL, {160,80,80,220});
        return;
    }

    int bx = 20;
    for (int i=0;i<(int)installed.size();i++) {
        auto& b = installed[i];
        bool sel = (i==selectedBrowser);
        int bw = MeasureText(b.name.c_str(),FONT_SMALL)+24;
        Rectangle r={(float)bx,(float)(topY+14),(float)bw,28};
        Color ac = b.color;
        Color bg = sel ? Color{(unsigned char)(ac.r/4+10),(unsigned char)(ac.g/4+10),(unsigned char)(ac.b/4+10),255}
                       : Color{14,18,30,255};
        DrawRectangleRounded(r,0.3f,8,bg);
        DrawRectangleLinesEx(r,sel?1.8f:1.0f,{ac.r,ac.g,ac.b,(unsigned char)(sel?230:100)});
        if(sel) DrawRectangle((int)r.x,(int)(r.y+r.height-2),(int)r.width,2,{ac.r,ac.g,ac.b,160});
        DrawText(b.name.c_str(),(int)(r.x+10),(int)(r.y+7),FONT_SMALL,
            sel?Color{230,240,248,255}:Color{130,155,170,220});
        if(CheckCollisionPointRec(GetMousePosition(),r)&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
            selectedBrowser=i;
        bx+=bw+8;
    }
}

static void DrawUrlBar(int sw, int topY) {
    // Label
    DrawText("URL", 20, topY, FONT_TINY, {80,105,120,200});

    int barW = sw - 130;
    Rectangle box={20,(float)(topY+14),(float)barW,38};

    // Background
    DrawRectangleRec(box, {10,14,24,255});
    DrawRectangleLinesEx(box, urlFocused?1.8f:1.0f,
        urlFocused?Color{80,160,200,220}:Color{35,50,68,200});
    if(urlFocused)
        DrawRectangle((int)box.x,(int)(box.y+box.height-2),(int)box.width,2,{60,140,180,120});

    // URL text (truncate display if too long)
    std::string display(urlBuf);
    while(display.size()>1 && MeasureText(display.c_str(),FONT_SMALL)>barW-20)
        display = display.substr(1);
    DrawText(display.c_str(),(int)box.x+10,(int)box.y+10,FONT_SMALL,{190,215,230,240});

    // Blinking cursor
    if(urlFocused&&(int)(GetTime()*2)%2==0){
        int cw=MeasureText(display.c_str(),FONT_SMALL);
        DrawText("|",(int)box.x+12+cw,(int)box.y+10,FONT_SMALL,{80,160,200,255});
    }

    // Click to focus
    if(IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
        urlFocused = CheckCollisionPointRec(GetMousePosition(), box);

    // Go button
    Rectangle goBtn={(float)(sw-102),(float)(topY+14),80,38};
    Color goBg={20,50,60,255};
    bool goHov=CheckCollisionPointRec(GetMousePosition(),goBtn);
    DrawRectangleRounded(goBtn,0.25f,8,goHov?Color{30,80,100,255}:goBg);
    DrawRectangleLinesEx(goBtn,goHov?1.8f:1.2f,{70,170,200,(unsigned char)(goHov?230:160)});
    DrawTxtC("GO",(int)(goBtn.x+goBtn.width/2),(int)(goBtn.y+10),FONT_NORMAL,{120,210,230,255});
    if(goHov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
        LaunchUrl(NormaliseUrl(std::string(urlBuf)));

    // Keyboard input for URL
    if(urlFocused){
        int k=GetCharPressed();
        while(k>0){
            if(k>=32&&urlLen<510){urlBuf[urlLen++]=(char)k;urlBuf[urlLen]='\0';}
            k=GetCharPressed();
        }
        if(IsKeyPressed(KEY_BACKSPACE)&&urlLen>0) urlBuf[--urlLen]='\0';
        if(IsKeyPressed(KEY_ENTER))
            LaunchUrl(NormaliseUrl(std::string(urlBuf)));
        // Ctrl+A select all (clear + retype)
        if((IsKeyDown(KEY_LEFT_CONTROL)||IsKeyDown(KEY_RIGHT_CONTROL))&&IsKeyPressed(KEY_A)){
            memset(urlBuf,0,512); urlLen=0;
        }
    }
}

static void DrawBookmarks(int sw, int topY) {
    DrawText("Quick Access", 20, topY, FONT_TINY, {80,105,120,200});

    int bx=20, by=topY+16;
    int maxW=sw-24;

    for(int i=0;i<BOOKMARK_COUNT;i++){
        int bw=MeasureText(BOOKMARKS[i].label,FONT_SMALL)+20;
        if(bx+bw>maxW){ bx=20; by+=34; }
        Rectangle r={(float)bx,(float)by,(float)bw,26};
        Color ac=BOOKMARKS[i].color;
        bool hov=CheckCollisionPointRec(GetMousePosition(),r);
        DrawRectangleRounded(r,0.35f,8,
            hov?Color{(unsigned char)(ac.r/4+12),(unsigned char)(ac.g/4+12),(unsigned char)(ac.b/4+12),255}
               :Color{12,16,26,255});
        DrawRectangleLinesEx(r,hov?1.5f:1.0f,{ac.r,ac.g,ac.b,(unsigned char)(hov?200:90)});
        DrawText(BOOKMARKS[i].label,(int)(r.x+10),(int)(r.y+6),FONT_SMALL,
            hov?Color{230,240,248,255}:Color{ac.r,ac.g,ac.b,200});
        if(hov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
            strncpy(urlBuf,BOOKMARKS[i].url,511);
            urlLen=(int)strlen(urlBuf);
            LaunchUrl(std::string(BOOKMARKS[i].url));
        }
        bx+=bw+8;
    }
}

static void DrawHistory(int sw, int topY, int botY) {
    int h=botY-topY;
    DrawRectangle(0,topY,sw,1,{30,42,58,200});
    DrawText("Recent", 20, topY+10, FONT_TINY, {80,105,120,200});

    if(history.empty()){
        DrawText("No history yet — launch a site to get started.",
                 20,topY+30,FONT_SMALL,{55,75,90,180});
        return;
    }

    int itemH=44;
    int visCount=(h-36)/itemH;
    int maxScroll=std::max(0,(int)history.size()-visCount);
    histScroll=std::max(0,std::min(histScroll,maxScroll));

    // Mouse wheel scroll when over history
    Vector2 mouse=GetMousePosition();
    if(mouse.y>topY){
        float wh=GetMouseWheelMove();
        histScroll-=(int)wh;
    }

    for(int i=histScroll;i<(int)history.size()&&i<histScroll+visCount;i++){
        int iy=topY+30+(i-histScroll)*itemH;
        auto& e=history[i];

        Rectangle row={8,(float)iy,(float)(sw-16),(float)(itemH-4)};
        bool hov=CheckCollisionPointRec(mouse,row);
        DrawRectangleRounded(row,0.1f,6,
            hov?Color{18,26,42,255}:Color{11,15,26,255});
        DrawRectangleLinesEx(row,1.0f,{30,42,60,(unsigned char)(hov?140:70)});

        // Favicon-style colored dot
        Color dot={80,140,180,255};
        for(auto& b:installed) if(b.name==e.browser){dot=b.color;break;}
        DrawCircle((int)(row.x+18),(int)(iy+itemH/2-2),5,{dot.r,dot.g,dot.b,180});

        // URL (truncate)
        std::string disp=e.url;
        int maxUrlW=sw-230;
        while(disp.size()>4&&MeasureText(disp.c_str(),FONT_SMALL)>maxUrlW)
            disp.pop_back();
        if(disp!=e.url) disp+="...";
        DrawText(disp.c_str(),(int)(row.x+32),(int)(iy+6),FONT_SMALL,
            hov?Color{190,215,230,240}:Color{130,160,180,200});

        // Time + browser
        char meta[48]; snprintf(meta,48,"%s  •  %s",e.timeStr.c_str(),e.browser.c_str());
        DrawText(meta,(int)(row.x+32),(int)(iy+24),FONT_TINY,{65,90,110,180});

        // Re-open button
        Rectangle openBtn={(float)(sw-80),(float)(iy+8),68,26};
        if(NiceButton(openBtn,"Open",{80,160,200,255})){
            strncpy(urlBuf,e.url.c_str(),511);
            urlLen=(int)strlen(urlBuf);
            LaunchUrl(e.url);
        }
    }

    // Scrollbar
    if((int)history.size()>visCount&&maxScroll>0){
        float frac=(float)histScroll/maxScroll;
        int sbH=std::max(24,(int)((float)visCount/history.size()*(h-36)));
        int sbY=topY+30+(int)(frac*(h-36-sbH));
        DrawRectangle(sw-4,topY+30,4,h-36,{18,24,38,200});
        DrawRectangle(sw-4,sbY,4,sbH,{70,110,140,200});
    }

    // Clear history button
    Rectangle clrBtn={(float)(sw-90),(float)(topY+6),82,22};
    if(NiceButton(clrBtn,"Clear",{180,80,90,255})){
        history.clear();
        remove(HISTORY_PATH);
        SetStatus("History cleared.",{180,140,100,255});
    }
}

static void DrawStatusBar(int sw, int sh) {
    DrawRectangle(0,sh-22,sw,22,{8,11,20,230});
    DrawLine(0,sh-22,sw,22,{30,42,58,160});

    // Status message
    double age=GetTime()-statusAt;
    if(age<5.0){
        unsigned char a=(age>4.0)?(unsigned char)((5.0-age)*255):255;
        Color mc=statusColor; mc.a=a;
        DrawText(statusMsg,14,sh-16,FONT_TINY,mc);
    }

    // Launch flash indicator
    double la=GetTime()-lastLaunchAt;
    if(la<3.0&&!lastLaunchedUrl.empty()){
        unsigned char a=(la>2.0)?(unsigned char)((3.0-la)*255):255;
        std::string lbl="↗ Opened in "+
            (selectedBrowser<(int)installed.size()?installed[selectedBrowser].name:std::string("browser"));
        int lw=MeasureText(lbl.c_str(),FONT_TINY);
        DrawText(lbl.c_str(),sw-lw-14,sh-16,FONT_TINY,{100,210,180,(unsigned char)a});
    }

    // Keyboard hints on right
    const char* hints="Enter to go   Ctrl+A clear bar";
    int hw=MeasureText(hints,FONT_TINY);
    if(age>=5.0||GetTime()-statusAt<0)
        DrawText(hints,sw-hw-14,sh-16,FONT_TINY,{50,70,85,180});
}

// ── Top header ────────────────────────────────────────────
static void DrawHeader(int sw) {
    DrawRectangle(0,0,sw,36,{10,12,24,255});
    DrawLine(0,36,sw,36,{30,44,60,180});
    // Logo
    DrawText("NexOS",14,10,FONT_NORMAL,{80,150,180,230});
    DrawText("Browser",14+MeasureText("NexOS",FONT_NORMAL)+8,10,FONT_NORMAL,{50,90,110,180});
    // Animated dot when browser is running
    double la=GetTime()-lastLaunchAt;
    if(la<4.0){
        float pulse=sinf(animTime*4.0f)*0.4f+0.6f;
        DrawCircle(sw-20,18,5,{100,220,180,(unsigned char)(int)(pulse*200)});
        DrawText("Running",sw-95,10,FONT_TINY,{100,200,170,200});
    }
}

// ============================================================
//  MAIN
// ============================================================
int main() {
    if(!RequestResources(APP_NAME,RAM_MB,HDD_MB,PRIORITY_NORMAL,1)){
        InitWindow(440,120,"Browser — Denied"); SetTargetFPS(30);
        double t=GetTime();
        while(!WindowShouldClose()&&GetTime()-t<3.5){
            BeginDrawing();ClearBackground(BG_DEEP);
            DrawText("Insufficient resources.",18,40,FONT_NORMAL,NEON_PINK);
            EndDrawing();
        }
        CloseWindow(); return 1;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(WIN_W, WIN_H, "NexOS Browser");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    SetWindowFocused();

    DetectBrowsers();
    LoadHistory();
    mkdir("hdd",0755);

    if(installed.empty())
        SetStatus("No browser found. Install chromium, firefox, or brave.",{220,140,80,255});
    else {
        char msg[64];
        snprintf(msg,64,"Found %d browser(s). Ready.",(int)installed.size());
        SetStatus(msg,{100,200,170,255});
    }

    while(!WindowShouldClose()&&appRunning){
        int sw=GetScreenWidth(), sh=GetScreenHeight();
        animTime+=GetFrameTime();

        // Reap any zombie browser children quietly
        waitpid(-1,nullptr,WNOHANG);

        BeginDrawing();
        ClearBackground({8,10,20,255});

        // Subtle background grid
        for(int x=0;x<sw;x+=44) DrawLine(x,0,x,sh,{14,18,30,55});
        for(int y=0;y<sh;y+=44) DrawLine(0,y,sw,y,{14,18,30,55});

        // Layout (all Y positions relative to sections)
        int y = 0;

        // Header bar
        DrawHeader(sw);
        y = 44;

        // Browser picker
        DrawRectangle(0,y,sw,60,{9,12,22,255});
        DrawBrowserPicker(sw,y+6);
        y+=60;
        DrawLine(0,y,sw,y,{28,40,56,200});

        // URL bar
        DrawRectangle(0,y,sw,66,{9,11,21,255});
        DrawUrlBar(sw,y+6);
        y+=66;
        DrawLine(0,y,sw,y,{28,40,56,200});

        // Bookmarks
        DrawRectangle(0,y,sw,66,{8,10,20,255});
        DrawBookmarks(sw,y+6);
        y+=66;
        DrawLine(0,y,sw,y,{28,40,56,200});

        // History (fills remaining space)
        DrawHistory(sw,y,sh-22);

        // Status bar
        DrawStatusBar(sw,sh);

        EndDrawing();
    }

    appRunning=false;
    ReleaseResources(APP_NAME,RAM_MB,HDD_MB);
    CloseWindow();
    return 0;
}
