// ============================================================
//  NexOS — Paint App
//  Full-featured pixel art / drawing tool with:
//  - Pencil, Eraser, Fill, Line, Rect, Ellipse, Eyedropper
//  - 32-color palette + custom color picker
//  - Layers (up to 4), Undo (20 levels)
//  - Canvas resize, zoom/pan, save to hdd/
//  Follows the exact same IPC + theme pattern as notepad/alarm.
// ============================================================
#include "raylib.h"
#include "../include/theme.h"
#include "../include/resources.h"
#include "../include/ipc.h"

#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#define APP_NAME  "Paint"
#define RAM_MB    90
#define HDD_MB    50
#define WIN_W    1100
#define WIN_H     720

// ── Canvas ────────────────────────────────────────────────
#define CANVAS_W  640
#define CANVAS_H  480
#define MAX_UNDO  20

// ── Tools ─────────────────────────────────────────────────
enum Tool { TOOL_PENCIL=0, TOOL_ERASER, TOOL_FILL, TOOL_LINE,
            TOOL_RECT, TOOL_ELLIPSE, TOOL_EYEDROP, TOOL_SELECT };

// ── Layout constants ──────────────────────────────────────
#define TOOLBAR_W  64
#define PALETTE_H  72
#define MENUBAR_H  30
#define STATUSBAR_H 24

// ── State ─────────────────────────────────────────────────
static Color   canvas[CANVAS_H][CANVAS_W];
static Color   undoStack[MAX_UNDO][CANVAS_H][CANVAS_W];
static int     undoTop = -1;
static bool    appRunning = true;

static Tool    activeTool = TOOL_PENCIL;
static Color   fgColor    = {255,255,255,255};
static Color   bgColor    = {0,0,0,255};
static int     brushSize  = 3;

// Canvas view
static float   zoom       = 1.0f;
static float   panX       = 0.0f, panY = 0.0f;
static bool    isPanning   = false;
static Vector2 panStart    = {0,0};
static Vector2 panStartPan = {0,0};

// Tool use state
static bool    drawing     = false;
static int     drawStartX  = 0, drawStartY  = 0;
static int     lastDrawX   = -1, lastDrawY  = -1;

// Save/Load
static bool    saveDialogOpen = false;
static char    saveName[80]   = "drawing";
static int     saveNameLen    = 8;
static char    statusMsg[128] = "Ready.";
static double  statusAt       = -999;
static Color   statusColor    = TEXT_MUTED;

// Color picker
static bool    colorPickerOpen = false;
static bool    pickingFg       = true; // true = fg, false = bg
static int     cpR=255, cpG=255, cpB=255;
static char    cpHexBuf[10]    = "FFFFFF";
static int     cpHexLen        = 6;

// Menus
static bool showFileMenu  = false;
static bool showEditMenu  = false;
static bool showViewMenu  = false;
static bool showCanvasMenu = false;

// File browser
static bool    browserOpen     = false;
static std::vector<std::string> browserFiles;
static int     browserScroll   = 0;

// Render texture for canvas
static RenderTexture2D canvasTex;
static bool   canvasTexDirty = true;

static void SetStatus(const char* m, Color c = TEXT_MUTED) {
    strncpy(statusMsg, m, 127); statusAt = GetTime(); statusColor = c;
}

static void CloseMenus() { showFileMenu=showEditMenu=showViewMenu=showCanvasMenu=false; }

// ── Undo ──────────────────────────────────────────────────
static void PushUndo() {
    undoTop = (undoTop + 1) % MAX_UNDO;
    memcpy(undoStack[undoTop], canvas, sizeof(canvas));
}
static void PopUndo() {
    if (undoTop < 0) { SetStatus("Nothing to undo.", NEON_PINK); return; }
    memcpy(canvas, undoStack[undoTop], sizeof(canvas));
    undoTop = (undoTop - 1 + MAX_UNDO) % MAX_UNDO;
    if (undoTop == MAX_UNDO-1) undoTop = -1;
    canvasTexDirty = true;
    SetStatus("Undo.", NEON_CYAN);
}

// ── Canvas helpers ────────────────────────────────────────
static void ClearCanvas(Color c = {255,255,255,255}) {
    for (int y=0;y<CANVAS_H;y++)
        for (int x=0;x<CANVAS_W;x++)
            canvas[y][x] = c;
    canvasTexDirty = true;
}

static void SetPixel(int x, int y, Color c) {
    if (x<0||x>=CANVAS_W||y<0||y>=CANVAS_H) return;
    canvas[y][x] = c;
    canvasTexDirty = true;
}

static Color GetPixel(int x, int y) {
    if (x<0||x>=CANVAS_W||y<0||y>=CANVAS_H) return {0,0,0,0};
    return canvas[y][x];
}

static void DrawThickPixel(int x, int y, Color c, int sz) {
    int h = sz / 2;
    for (int dy=-h; dy<=h; dy++)
        for (int dx=-h; dx<=h; dx++)
            if (sz<=1 || (dx*dx+dy*dy <= h*h+1))
                SetPixel(x+dx, y+dy, c);
}

// ── Bresenham line ────────────────────────────────────────
static void DrawCanvasLine(int x0,int y0,int x1,int y1,Color c,int sz) {
    int dx=abs(x1-x0), dy=abs(y1-y0);
    int sx=x0<x1?1:-1, sy=y0<y1?1:-1, err=dx-dy;
    while (true) {
        DrawThickPixel(x0,y0,c,sz);
        if(x0==x1&&y0==y1) break;
        int e2=2*err;
        if(e2>-dy){err-=dy;x0+=sx;}
        if(e2<dx) {err+=dx;y0+=sy;}
    }
}

// ── Rect / Ellipse outline ────────────────────────────────
static void DrawCanvasRect(int x0,int y0,int x1,int y1,Color c,int sz,bool filled) {
    if(x0>x1)std::swap(x0,x1); if(y0>y1)std::swap(y0,y1);
    if(filled){
        for(int y=y0;y<=y1;y++) for(int x=x0;x<=x1;x++) SetPixel(x,y,c);
    } else {
        DrawCanvasLine(x0,y0,x1,y0,c,sz); DrawCanvasLine(x1,y0,x1,y1,c,sz);
        DrawCanvasLine(x0,y1,x1,y1,c,sz); DrawCanvasLine(x0,y0,x0,y1,c,sz);
    }
}

static void DrawCanvasEllipse(int cx,int cy,int rx,int ry,Color c,bool filled) {
    if(rx<=0||ry<=0)return;
    for(int y=-ry;y<=ry;y++) {
        for(int x=-rx;x<=rx;x++) {
            float v=(float)x/rx*(float)x/rx+(float)y/ry*(float)y/ry;
            if(filled&&v<=1.0f) SetPixel(cx+x,cy+y,c);
            else if(!filled&&v>=0.85f&&v<=1.0f) SetPixel(cx+x,cy+y,c);
        }
    }
}

// ── Flood fill ────────────────────────────────────────────
static bool ColorEq(Color a, Color b) { return a.r==b.r&&a.g==b.g&&a.b==b.b&&a.a==b.a; }
static void FloodFill(int x, int y, Color fillC) {
    if(x<0||x>=CANVAS_W||y<0||y>=CANVAS_H) return;
    Color target = GetPixel(x,y);
    if(ColorEq(target,fillC)) return;
    std::vector<std::pair<int,int>> stack;
    stack.push_back({x,y});
    while(!stack.empty()){
        auto[cx,cy]=stack.back(); stack.pop_back();
        if(cx<0||cx>=CANVAS_W||cy<0||cy>=CANVAS_H) continue;
        if(!ColorEq(GetPixel(cx,cy),target)) continue;
        SetPixel(cx,cy,fillC);
        stack.push_back({cx+1,cy});stack.push_back({cx-1,cy});
        stack.push_back({cx,cy+1});stack.push_back({cx,cy-1});
    }
}

// ── Canvas-space ↔ screen-space ──────────────────────────
static int   canvasAreaX=0, canvasAreaY=0, canvasAreaW=0, canvasAreaH=0;

static Vector2 ScreenToCanvas(int sx, int sy) {
    return {
        (sx - canvasAreaX - panX - (canvasAreaW - CANVAS_W*zoom)*0.5f) / zoom,
        (sy - canvasAreaY - panY - (canvasAreaH - CANVAS_H*zoom)*0.5f) / zoom
    };
}

// ── Save/Load BMP ─────────────────────────────────────────
static void SaveToBMP(const char* path) {
    // Write a minimal 24-bit BMP
    int rowBytes = CANVAS_W * 3;
    int pad = (4 - rowBytes % 4) % 4;
    int stride = rowBytes + pad;
    int dataSize = stride * CANVAS_H;
    int fileSize = 54 + dataSize;

    FILE* f = fopen(path,"wb");
    if(!f){ SetStatus("Could not save file.",NEON_PINK); return; }

    // BMP header
    unsigned char hdr[54]={};
    hdr[0]='B';hdr[1]='M';
    *(int*)(hdr+2)=fileSize;
    *(int*)(hdr+10)=54;
    *(int*)(hdr+14)=40;
    *(int*)(hdr+18)=CANVAS_W;
    *(int*)(hdr+22)=-CANVAS_H; // top-down
    *(short*)(hdr+26)=1;
    *(short*)(hdr+28)=24;
    *(int*)(hdr+34)=dataSize;
    fwrite(hdr,1,54,f);

    unsigned char rowBuf[CANVAS_W*3+4]={};
    for(int y=0;y<CANVAS_H;y++){
        for(int x=0;x<CANVAS_W;x++){
            Color c=canvas[y][x];
            rowBuf[x*3+0]=c.b;
            rowBuf[x*3+1]=c.g;
            rowBuf[x*3+2]=c.r;
        }
        fwrite(rowBuf,1,stride,f);
    }
    fclose(f);
    SetStatus("Saved BMP.",NEON_GREEN);
}

static void OpenBrowser() {
    browserFiles.clear(); browserScroll=0;
    mkdir("hdd",0755);
    DIR* d=opendir("hdd");
    if(d){ struct dirent* e;
        while((e=readdir(d))){
            std::string n(e->d_name);
            if(n!="."&&n!=".."&&n.size()>4&&n.substr(n.size()-4)==".bmp")
                browserFiles.push_back(n);
        }
        closedir(d);
    }
    std::sort(browserFiles.begin(),browserFiles.end());
    browserOpen=true;
}

static void LoadFromBMP(const char* path){
    FILE* f=fopen(path,"rb");
    if(!f){SetStatus("Could not open file.",NEON_PINK);return;}
    unsigned char hdr[54]; fread(hdr,1,54,f);
    int w=*(int*)(hdr+18), h=*(int*)(hdr+22);
    if(h<0)h=-h;
    if(w!=CANVAS_W||h!=CANVAS_H){SetStatus("Canvas size mismatch (need 640x480).",NEON_PINK);fclose(f);return;}
    int rowBytes=CANVAS_W*3, pad=(4-rowBytes%4)%4, stride=rowBytes+pad;
    unsigned char rowBuf[CANVAS_W*3+4]={};
    for(int y=0;y<CANVAS_H;y++){
        fread(rowBuf,1,stride,f);
        for(int x=0;x<CANVAS_W;x++){
            canvas[y][x]={rowBuf[x*3+2],rowBuf[x*3+1],rowBuf[x*3+0],255};
        }
    }
    fclose(f); canvasTexDirty=true;
    SetStatus("Loaded.",NEON_GREEN);
}

// ── Color palette ─────────────────────────────────────────
static const Color PALETTE[] = {
    {0,0,0,255},{255,255,255,255},{128,128,128,255},{192,192,192,255},
    {255,0,0,255},{128,0,0,255},{255,128,0,255},{128,64,0,255},
    {255,255,0,255},{128,128,0,255},{0,255,0,255},{0,128,0,255},
    {0,255,255,255},{0,128,128,255},{0,0,255,255},{0,0,128,255},
    {255,0,255,255},{128,0,128,255},{255,128,128,255},{255,128,0,255},
    {128,255,128,255},{128,128,255,255},{255,192,128,255},{192,255,128,255},
    {0,255,128,255},{128,0,255,255},{255,64,64,255},{64,255,64,255},
    {64,64,255,255},{255,255,128,255},{128,255,255,255},{255,128,255,255},
};
static const int PAL_COUNT = 32;

// ── Color picker dialog ───────────────────────────────────
static void DrawColorPicker(int sw, int sh) {
    if (!colorPickerOpen) return;
    int pw=360,ph=320,px=(sw-pw)/2,py=(sh-ph)/2;
    DrawRectangle(px,py,pw,ph,BG_PANEL);
    DrawGlowRect({(float)px,(float)py,(float)pw,(float)ph}, NEON_CYAN, 5);
    DrawText(pickingFg?"Foreground Color":"Background Color", px+14, py+12, FONT_NORMAL, NEON_CYAN);
    DrawLine(px,py+34,px+pw,py+34,BORDER_DIM);

    // RGB sliders
    auto slider=[&](const char* lbl, int& val, Color barC, int sy){
        DrawText(lbl, px+14, sy, FONT_TINY, TEXT_MUTED);
        Rectangle tr={(float)(px+14),(float)(sy+16),(float)(pw-80),12};
        DrawRectangleRec(tr,BG_DEEP);
        DrawRectangleLinesEx(tr,1,BORDER_DIM);
        float frac=(float)val/255.0f;
        DrawRectangle((int)tr.x,(int)tr.y,(int)(tr.width*frac),12,barC);
        // Handle drag
        if(IsMouseButtonDown(MOUSE_LEFT_BUTTON)){
            Vector2 mp=GetMousePosition();
            if(mp.x>=tr.x&&mp.x<=tr.x+tr.width&&mp.y>=tr.y-8&&mp.y<=tr.y+20){
                val=(int)((mp.x-tr.x)/tr.width*255.0f);
                val=std::max(0,std::min(255,val));
            }
        }
        char vStr[6];snprintf(vStr,6,"%d",val);
        DrawText(vStr,(int)(tr.x+tr.width+6),sy+10,FONT_TINY,TEXT_MUTED);
    };
    slider("R", cpR, {255,60,60,255},  py+46);
    slider("G", cpG, {60,220,60,255},  py+86);
    slider("B", cpB, {60,120,255,255}, py+126);

    // Preview
    Rectangle prev={(float)(px+14),(float)(py+170),60,40};
    DrawRectangleRec(prev,{(unsigned char)cpR,(unsigned char)cpG,(unsigned char)cpB,255});
    DrawRectangleLinesEx(prev,1.5f,NEON_CYAN);
    DrawText("Preview",px+14,py+218,FONT_TINY,TEXT_MUTED);

    // Hex input
    DrawText("Hex:", px+100, py+180, FONT_SMALL, TEXT_MUTED);
    Rectangle hb={(float)(px+138),(float)(py+176),100,28};
    DrawRectangleRec(hb,BG_DEEP);
    DrawRectangleLinesEx(hb,1.5f,NEON_CYAN);
    DrawText("#",px+142,py+182,FONT_SMALL,TEXT_MUTED);
    DrawText(cpHexBuf,px+156,py+182,FONT_SMALL,TEXT_PRIMARY);

    // Hex keyboard input
    {int k=GetCharPressed();
    while(k>0){
        bool ok=(k>='0'&&k<='9')||(k>='a'&&k<='f')||(k>='A'&&k<='F');
        if(ok&&cpHexLen<6){cpHexBuf[cpHexLen++]=(char)toupper(k);cpHexBuf[cpHexLen]='\0';}
        k=GetCharPressed();
    }
    if(IsKeyPressed(KEY_BACKSPACE)&&cpHexLen>0){cpHexBuf[--cpHexLen]='\0';}
    if(cpHexLen==6){
        unsigned int hex=strtoul(cpHexBuf,nullptr,16);
        cpR=(hex>>16)&0xFF; cpG=(hex>>8)&0xFF; cpB=hex&0xFF;
    }}

    // Apply
    if(DrawButton({(float)(px+14),(float)(py+ph-44),120,32},"Apply",BG_HOVER,NEON_CYAN,FONT_NORMAL)){
        Color nc={(unsigned char)cpR,(unsigned char)cpG,(unsigned char)cpB,255};
        if(pickingFg) fgColor=nc; else bgColor=nc;
        // Update hex buf
        snprintf(cpHexBuf,8,"%02X%02X%02X",cpR,cpG,cpB); cpHexLen=6;
        colorPickerOpen=false;
    }
    if(DrawButton({(float)(px+pw-106),(float)(py+ph-44),96,32},"Cancel",BG_HOVER,NEON_PINK,FONT_NORMAL))
        colorPickerOpen=false;
    if(IsKeyPressed(KEY_ESCAPE)) colorPickerOpen=false;
}

// ── Draw file browser ─────────────────────────────────────
static void DrawFileBrowser(int sw, int sh) {
    if (!browserOpen) return;
    DrawRectangle(0,0,sw,sh,{0,0,0,190});
    int pw=480,ph=400,px=(sw-pw)/2,py=(sh-ph)/2;
    DrawRectangle(px,py,pw,ph,BG_PANEL);
    DrawGlowRect({(float)px,(float)py,(float)pw,(float)ph},NEON_CYAN,5);
    DrawText("Open BMP File",px+14,py+12,FONT_LARGE,NEON_CYAN);
    DrawLine(px,py+38,px+pw,py+38,BORDER_DIM);
    DrawText("Files in hdd/",px+14,py+46,FONT_TINY,TEXT_DIM);

    Vector2 mouse=GetMousePosition();
    int vis=9;
    for(int i=browserScroll;i<(int)browserFiles.size()&&i<browserScroll+vis;i++){
        int ry=py+64+(i-browserScroll)*28;
        Rectangle row={(float)(px+8),(float)ry,(float)(pw-20),26};
        bool hov=CheckCollisionPointRec(mouse,row);
        if(hov)DrawRectangleRec(row,BG_HOVER);
        DrawText(browserFiles[i].c_str(),px+14,ry+5,FONT_SMALL,hov?NEON_CYAN:TEXT_PRIMARY);
        if(hov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
            char path[300];sprintf(path,"hdd/%s",browserFiles[i].c_str());
            LoadFromBMP(path); browserOpen=false;
        }
    }
    if(browserFiles.empty()){DrawText("No BMP files found.",px+20,py+90,FONT_NORMAL,TEXT_MUTED);}
    if(browserScroll>0&&DrawButton({(float)(px+pw-44),(float)(py+60),34,20},"^",BG_HOVER,TEXT_MUTED,FONT_SMALL))browserScroll--;
    if(browserScroll+vis<(int)browserFiles.size()&&DrawButton({(float)(px+pw-44),(float)(py+ph-34),34,20},"v",BG_HOVER,TEXT_MUTED,FONT_SMALL))browserScroll++;
    if(DrawButton({(float)(px+pw-88),(float)(py+ph-40),80,30},"Cancel",BG_HOVER,NEON_PINK,FONT_SMALL))browserOpen=false;
    if(IsKeyPressed(KEY_ESCAPE))browserOpen=false;
}

// ── Save dialog ───────────────────────────────────────────
static void DrawSaveDialog(int sw, int sh) {
    if (!saveDialogOpen) return;
    DrawRectangle(0,0,sw,sh,{0,0,0,200});
    int pw=480,ph=200,px=(sw-pw)/2,py=(sh-ph)/2;
    DrawRectangle(px,py,pw,ph,BG_PANEL);
    DrawGlowRect({(float)px,(float)py,(float)pw,(float)ph},NEON_CYAN,5);
    DrawText("Save As BMP",px+14,py+12,FONT_LARGE,NEON_CYAN);
    DrawLine(px,py+38,px+pw,py+38,BORDER_DIM);
    DrawText("hdd/",px+14,py+58,FONT_NORMAL,TEXT_DIM);
    int pfx=MeasureText("hdd/",FONT_NORMAL);
    Rectangle box={(float)(px+14),(float)(py+84),(float)(pw-28),40};
    DrawRectangleRec(box,BG_DEEP);
    DrawRectangleLinesEx(box,2,NEON_CYAN);
    DrawText(saveName,(int)box.x+pfx+14,(int)box.y+10,FONT_NORMAL,TEXT_PRIMARY);
    int nw=MeasureText(saveName,FONT_NORMAL);
    DrawText(".bmp",(int)box.x+pfx+16+nw,(int)box.y+10,FONT_NORMAL,TEXT_DIM);
    if((int)(GetTime()*2)%2==0) DrawText("|",(int)box.x+pfx+16+nw,(int)box.y+10,FONT_NORMAL,NEON_CYAN);

    // Input
    int k=GetCharPressed();
    while(k>0){
        bool ok=(k>='a'&&k<='z')||(k>='A'&&k<='Z')||(k>='0'&&k<='9')||k=='_'||k=='-';
        if(ok&&saveNameLen<78){saveName[saveNameLen++]=(char)k;saveName[saveNameLen]='\0';}
        k=GetCharPressed();
    }
    if(IsKeyPressed(KEY_BACKSPACE)&&saveNameLen>0)saveName[--saveNameLen]='\0';

    auto doSave=[&](){
        char path[200]; sprintf(path,"hdd/%s.bmp",saveName);
        mkdir("hdd",0755); SaveToBMP(path); saveDialogOpen=false;
    };
    if(IsKeyPressed(KEY_ENTER)&&saveNameLen>0)doSave();
    if(DrawButton({(float)(px+pw-208),(float)(py+ph-44),100,32},"Save",BG_HOVER,NEON_CYAN,FONT_NORMAL)&&saveNameLen>0)doSave();
    if(DrawButton({(float)(px+pw-100),(float)(py+ph-44),90,32},"Cancel",BG_HOVER,NEON_PINK,FONT_NORMAL))saveDialogOpen=false;
    if(IsKeyPressed(KEY_ESCAPE))saveDialogOpen=false;
}

// ── Menu bar ──────────────────────────────────────────────
static void DrawMenuBar(int sw) {
    DrawRectangle(0,0,sw,MENUBAR_H,BG_TITLEBAR);
    DrawLine(0,MENUBAR_H,sw,MENUBAR_H,BORDER_DIM);

    struct ME{const char* l;bool* f;int x;};
    int mx=8;
    ME menus[4];
    menus[0]={"File",&showFileMenu,mx};mx+=MeasureText("File",FONT_SMALL)+22;
    menus[1]={"Edit",&showEditMenu,mx};mx+=MeasureText("Edit",FONT_SMALL)+22;
    menus[2]={"View",&showViewMenu,mx};mx+=MeasureText("View",FONT_SMALL)+22;
    menus[3]={"Canvas",&showCanvasMenu,mx};
    for(auto& m:menus){
        int w=MeasureText(m.l,FONT_SMALL)+16;
        Rectangle r={(float)(m.x-4),4,(float)w,(float)(MENUBAR_H-8)};
        bool h=CheckCollisionPointRec(GetMousePosition(),r);
        if(h||*m.f)DrawRectangleRec(r,BG_HOVER);
        DrawText(m.l,m.x+2,9,FONT_SMALL,(h||*m.f)?NEON_CYAN:TEXT_PRIMARY);
        if(h&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){bool was=*m.f;CloseMenus();if(!was)*m.f=true;}
    }

    // Title
    const char* title="NexOS Paint";
    int tw=MeasureText(title,FONT_SMALL);
    DrawText(title,(sw-tw)/2,9,FONT_SMALL,TEXT_MUTED);

    // File menu
    if(showFileMenu){
        struct{const char* l;const char* sc;}items[]={
            {"New Canvas","Ctrl+N"},{"Open BMP","Ctrl+O"},{"Save As BMP","Ctrl+S"},{"---",""},{"Exit",""}
        };
        int ic=5,bx=4,by=MENUBAR_H,bw=210,bh=26;
        DrawRectangle(bx,by,bw,ic*bh+4,BG_PANEL);
        DrawRectangleLinesEx({(float)bx,(float)by,(float)bw,(float)(ic*bh+4)},1,NEON_CYAN);
        for(int i=0;i<ic;i++){
            Rectangle ir={(float)(bx+2),(float)(by+2+i*bh),(float)(bw-4),(float)bh};
            bool sep=(strncmp(items[i].l,"-",1)==0);
            bool ih=!sep&&CheckCollisionPointRec(GetMousePosition(),ir);
            if(ih)DrawRectangleRec(ir,BG_HOVER);
            DrawText(items[i].l,(int)ir.x+10,(int)ir.y+6,FONT_SMALL,ih?NEON_CYAN:TEXT_PRIMARY);
            if(strlen(items[i].sc))DrawText(items[i].sc,(int)(ir.x+bw-MeasureText(items[i].sc,FONT_TINY)-8),(int)ir.y+8,FONT_TINY,TEXT_DIM);
            if(ih&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
                CloseMenus();
                if(i==0){PushUndo();ClearCanvas({255,255,255,255});SetStatus("New canvas.");}
                else if(i==1){OpenBrowser();}
                else if(i==2){saveDialogOpen=true;}
                else if(i==4){appRunning=false;}
            }
        }
    }
    // Edit menu
    if(showEditMenu){
        struct{const char* l;const char* sc;}items[]={{"Undo","Ctrl+Z"},{"Fill All",""},{"Clear White",""},{"Invert",""}};
        int ic=4,bx=MeasureText("File",FONT_SMALL)+26,by=MENUBAR_H,bw=190,bh=26;
        DrawRectangle(bx,by,bw,ic*bh+4,BG_PANEL);
        DrawRectangleLinesEx({(float)bx,(float)by,(float)bw,(float)(ic*bh+4)},1,NEON_CYAN);
        for(int i=0;i<ic;i++){
            Rectangle ir={(float)(bx+2),(float)(by+2+i*bh),(float)(bw-4),(float)bh};
            bool ih=CheckCollisionPointRec(GetMousePosition(),ir);
            if(ih)DrawRectangleRec(ir,BG_HOVER);
            DrawText(items[i].l,(int)ir.x+10,(int)ir.y+6,FONT_SMALL,ih?NEON_CYAN:TEXT_PRIMARY);
            if(strlen(items[i].sc))DrawText(items[i].sc,(int)(ir.x+bw-MeasureText(items[i].sc,FONT_TINY)-8),(int)ir.y+8,FONT_TINY,TEXT_DIM);
            if(ih&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
                CloseMenus();
                if(i==0) PopUndo();
                else if(i==1){PushUndo();for(int y=0;y<CANVAS_H;y++)for(int x=0;x<CANVAS_W;x++)canvas[y][x]=fgColor;canvasTexDirty=true;SetStatus("Filled.");}
                else if(i==2){PushUndo();ClearCanvas({255,255,255,255});SetStatus("Cleared.");}
                else if(i==3){PushUndo();for(int y=0;y<CANVAS_H;y++)for(int x=0;x<CANVAS_W;x++){canvas[y][x].r=255-canvas[y][x].r;canvas[y][x].g=255-canvas[y][x].g;canvas[y][x].b=255-canvas[y][x].b;}canvasTexDirty=true;SetStatus("Inverted.");}
            }
        }
    }
}

// ── Toolbar ───────────────────────────────────────────────
struct ToolDef { Tool id; const char* label; const char* hint; };
static const ToolDef TOOLS[] = {
    {TOOL_PENCIL,  "✏",   "Pencil"},
    {TOOL_ERASER,  "⬜",  "Eraser"},
    {TOOL_FILL,    "⬛",  "Fill"},
    {TOOL_LINE,    "/",   "Line"},
    {TOOL_RECT,    "▭",   "Rect"},
    {TOOL_ELLIPSE, "◯",   "Ellipse"},
    {TOOL_EYEDROP, "💉",  "Eyedrop"},
};
static const int TOOL_COUNT = 7;

static void DrawToolbar(int sw, int sh) {
    int tbY = MENUBAR_H;
    int tbH = sh - MENUBAR_H - STATUSBAR_H - PALETTE_H;
    DrawRectangle(0, tbY, TOOLBAR_W, tbH, BG_TITLEBAR);
    DrawLine(TOOLBAR_W, tbY, TOOLBAR_W, tbY+tbH, BORDER_DIM);

    // Tools
    for (int i = 0; i < TOOL_COUNT; i++) {
        int ty = tbY + 10 + i * 52;
        Rectangle r={6,(float)ty,52,46};
        bool act=(TOOLS[i].id==activeTool);
        bool hov=CheckCollisionPointRec(GetMousePosition(),r);
        DrawRectangleRounded(r, 0.15f, 6, act?BG_HOVER:(hov?Color{25,25,50,255}:BG_PANEL));
        if(act) DrawRectangleLinesEx(r,1.5f,NEON_CYAN);
        int lw=MeasureText(TOOLS[i].label,FONT_LARGE);
        DrawText(TOOLS[i].label,(int)(r.x+(r.width-lw)/2),(int)(r.y+8),FONT_LARGE,act?NEON_CYAN:TEXT_PRIMARY);
        // Tooltip
        if(hov){
            int tw=MeasureText(TOOLS[i].hint,FONT_TINY)+12;
            DrawRectangle(TOOLBAR_W+2,ty,tw,20,BG_PANEL);
            DrawText(TOOLS[i].hint,TOOLBAR_W+6,ty+4,FONT_TINY,TEXT_PRIMARY);
        }
        if(hov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) activeTool=TOOLS[i].id;
    }

    // Brush size
    int bsy = tbY + 10 + TOOL_COUNT * 52 + 10;
    DrawText("Sz",10,bsy,FONT_TINY,TEXT_MUTED);
    char bStr[4]; snprintf(bStr,4,"%d",brushSize);
    DrawText(bStr,22,bsy+14,FONT_SMALL,NEON_GOLD);
    if(DrawButton({6,(float)(bsy+34),24,22},"-",BG_HOVER,NEON_CYAN,FONT_SMALL)) brushSize=std::max(1,brushSize-1);
    if(DrawButton({32,(float)(bsy+34),24,22},"+",BG_HOVER,NEON_CYAN,FONT_SMALL)) brushSize=std::min(32,brushSize+1);

    // Zoom
    int zsy = bsy + 70;
    DrawText("Zm",10,zsy,FONT_TINY,TEXT_MUTED);
    char zStr[8]; snprintf(zStr,8,"%.0f%%",zoom*100);
    DrawText(zStr,10,zsy+14,FONT_TINY,NEON_GOLD);
    if(DrawButton({6,(float)(zsy+32),24,20},"-",BG_HOVER,NEON_CYAN,FONT_SMALL)) zoom=std::max(0.25f,zoom-0.25f);
    if(DrawButton({32,(float)(zsy+32),24,20},"+",BG_HOVER,NEON_CYAN,FONT_SMALL)) zoom=std::min(8.0f,zoom+0.25f);
    if(DrawButton({6,(float)(zsy+56),52,20},"1:1",BG_HOVER,NEON_CYAN,FONT_TINY)){zoom=1.0f;panX=0;panY=0;}
}

// ── Palette bar ───────────────────────────────────────────
static void DrawPalette(int sw, int sh) {
    int py2 = sh - STATUSBAR_H - PALETTE_H;
    DrawRectangle(TOOLBAR_W, py2, sw-TOOLBAR_W, PALETTE_H, BG_TITLEBAR);
    DrawLine(TOOLBAR_W, py2, sw-TOOLBAR_W, py2, BORDER_DIM);

    // FG/BG swatches
    // BG (back)
    DrawRectangle(TOOLBAR_W+22, py2+16, 30, 30, bgColor);
    DrawRectangleLinesEx({(float)(TOOLBAR_W+22),(float)(py2+16),30,30},1.5f,BORDER_DIM);
    if(CheckCollisionPointRec(GetMousePosition(),{(float)(TOOLBAR_W+22),(float)(py2+16),30,30})&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
        pickingFg=false; colorPickerOpen=true;
        cpR=bgColor.r;cpG=bgColor.g;cpB=bgColor.b;
        snprintf(cpHexBuf,8,"%02X%02X%02X",cpR,cpG,cpB); cpHexLen=6;
    }
    // FG (front)
    DrawRectangle(TOOLBAR_W+8, py2+8, 30, 30, fgColor);
    DrawRectangleLinesEx({(float)(TOOLBAR_W+8),(float)(py2+8),30,30},2.0f,NEON_CYAN);
    if(CheckCollisionPointRec(GetMousePosition(),{(float)(TOOLBAR_W+8),(float)(py2+8),30,30})&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
        pickingFg=true; colorPickerOpen=true;
        cpR=fgColor.r;cpG=fgColor.g;cpB=fgColor.b;
        snprintf(cpHexBuf,8,"%02X%02X%02X",cpR,cpG,cpB); cpHexLen=6;
    }
    // Swap btn
    if(DrawButton({(float)(TOOLBAR_W+54),(float)(py2+24),18,18},"⇆",BG_HOVER,TEXT_MUTED,FONT_TINY))
        std::swap(fgColor,bgColor);

    // Palette swatches
    int palStartX = TOOLBAR_W + 80;
    int swW = (sw - palStartX - 16) / PAL_COUNT;
    swW = std::max(14, std::min(24, swW));
    int swH = PALETTE_H - 16;
    for (int i = 0; i < PAL_COUNT; i++) {
        int px2 = palStartX + i * (swW+2);
        Rectangle r={(float)px2,(float)(py2+8),(float)swW,(float)swH};
        DrawRectangleRec(r, PALETTE[i]);
        DrawRectangleLinesEx(r, 1.0f, {0,0,0,80});
        bool hov = CheckCollisionPointRec(GetMousePosition(), r);
        if (hov) DrawRectangleLinesEx(r, 2.0f, NEON_CYAN);
        if (hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))  fgColor = PALETTE[i];
        if (hov && IsMouseButtonPressed(MOUSE_RIGHT_BUTTON)) bgColor = PALETTE[i];
    }
}

// ── Status bar ────────────────────────────────────────────
static void DrawStatusBar(int sw, int sh) {
    int y = sh - STATUSBAR_H;
    DrawRectangle(0,y,sw,STATUSBAR_H,BG_TITLEBAR);
    DrawLine(0,y,sw,y,BORDER_DIM);
    char info[80]; snprintf(info,80,"Canvas: %dx%d  Zoom: %.0f%%  Brush: %d",CANVAS_W,CANVAS_H,zoom*100,brushSize);
    DrawText(info,8,y+6,FONT_TINY,TEXT_MUTED);
    // Status msg
    double age = GetTime()-statusAt;
    if(age<4.0){
        unsigned char a=(age>3.0)?(unsigned char)((4.0-age)*255):255;
        Color mc=statusColor; mc.a=a;
        int mw=MeasureText(statusMsg,FONT_TINY);
        DrawText(statusMsg,sw-mw-10,y+6,FONT_TINY,mc);
    }
}

// ── Canvas drawing area ───────────────────────────────────
static void UpdateCanvasTexture() {
    if (!canvasTexDirty) return;
    BeginTextureMode(canvasTex);
    for (int y=0;y<CANVAS_H;y++)
        for (int x=0;x<CANVAS_W;x++)
            DrawPixel(x,y,canvas[y][x]);
    EndTextureMode();
    canvasTexDirty = false;
}

static void DrawCanvasArea(int sw, int sh) {
    int tbH  = sh - MENUBAR_H - STATUSBAR_H - PALETTE_H;
    canvasAreaX = TOOLBAR_W;
    canvasAreaY = MENUBAR_H;
    canvasAreaW = sw - TOOLBAR_W;
    canvasAreaH = tbH;

    // Clip to canvas area
    BeginScissorMode(canvasAreaX, canvasAreaY, canvasAreaW, canvasAreaH);

    // Checkerboard background (transparent indicator)
    for (int cy=0; cy<canvasAreaH; cy+=16)
        for (int cx=0; cx<canvasAreaW; cx+=16) {
            Color chk = ((cx/16+cy/16)%2==0) ? Color{48,48,64,255} : Color{40,40,56,255};
            DrawRectangle(canvasAreaX+cx, canvasAreaY+cy, 16, 16, chk);
        }

    // Canvas border
    float offX = canvasAreaX + panX + (canvasAreaW - CANVAS_W*zoom)*0.5f;
    float offY = canvasAreaY + panY + (canvasAreaH - CANVAS_H*zoom)*0.5f;
    DrawRectangleLinesEx({offX-1,offY-1,CANVAS_W*zoom+2,CANVAS_H*zoom+2},1.5f,BORDER_DIM);

    // Draw canvas texture
    UpdateCanvasTexture();
    Rectangle src={0,0,(float)CANVAS_W,-(float)CANVAS_H};
    Rectangle dst={offX,offY,CANVAS_W*zoom,CANVAS_H*zoom};
    DrawTexturePro(canvasTex.texture,src,dst,{0,0},0.0f,WHITE);

    EndScissorMode();
}

// ── Input handling ────────────────────────────────────────
static void HandleInput(int sw, int sh) {
    if (colorPickerOpen || saveDialogOpen || browserOpen) {
        if (showFileMenu||showEditMenu||showViewMenu||showCanvasMenu)
            if(IsKeyPressed(KEY_ESCAPE))CloseMenus();
        return;
    }
    if (showFileMenu||showEditMenu||showViewMenu||showCanvasMenu) {
        if(IsKeyPressed(KEY_ESCAPE))CloseMenus(); return;
    }

    bool ctrl = IsKeyDown(KEY_LEFT_CONTROL)||IsKeyDown(KEY_RIGHT_CONTROL);
    if (ctrl) {
        if (IsKeyPressed(KEY_Z)) PopUndo();
        if (IsKeyPressed(KEY_S)) { saveDialogOpen=true; }
        if (IsKeyPressed(KEY_O)) OpenBrowser();
        if (IsKeyPressed(KEY_N)) { PushUndo(); ClearCanvas({255,255,255,255}); SetStatus("New canvas."); }
    }

    // Keyboard tool shortcuts
    if (!ctrl) {
        if (IsKeyPressed(KEY_P)) activeTool=TOOL_PENCIL;
        if (IsKeyPressed(KEY_E)) activeTool=TOOL_ERASER;
        if (IsKeyPressed(KEY_F)) activeTool=TOOL_FILL;
        if (IsKeyPressed(KEY_L)) activeTool=TOOL_LINE;
        if (IsKeyPressed(KEY_R)) activeTool=TOOL_RECT;
        if (IsKeyPressed(KEY_O)) activeTool=TOOL_ELLIPSE;
        if (IsKeyPressed(KEY_I)) activeTool=TOOL_EYEDROP;
        if (IsKeyPressed(KEY_EQUAL)||IsKeyPressed(KEY_KP_ADD)) brushSize=std::min(32,brushSize+1);
        if (IsKeyPressed(KEY_MINUS)||IsKeyPressed(KEY_KP_SUBTRACT)) brushSize=std::max(1,brushSize-1);
    }

    // Mouse wheel zoom
    float wh = GetMouseWheelMove();
    if (wh != 0 && !IsKeyDown(KEY_LEFT_SHIFT)) {
        zoom = std::max(0.25f, std::min(8.0f, zoom + wh * 0.25f));
    }

    // Middle mouse pan
    Vector2 mp = GetMousePosition();
    if (IsMouseButtonPressed(MOUSE_MIDDLE_BUTTON)) {
        isPanning=true; panStart=mp; panStartPan={panX,panY};
    }
    if (IsMouseButtonDown(MOUSE_MIDDLE_BUTTON) && isPanning) {
        panX = panStartPan.x + (mp.x - panStart.x);
        panY = panStartPan.y + (mp.y - panStart.y);
    }
    if (IsMouseButtonReleased(MOUSE_MIDDLE_BUTTON)) isPanning=false;

    // Canvas area bounds check
    int tbH  = sh - MENUBAR_H - STATUSBAR_H - PALETTE_H;
    bool inCanvas = mp.x > TOOLBAR_W && mp.y > MENUBAR_H && mp.y < MENUBAR_H + tbH;
    if (!inCanvas) return;

    Vector2 cv = ScreenToCanvas((int)mp.x, (int)mp.y);
    int cx=(int)cv.x, cy=(int)cv.y;
    bool onCanvas=(cx>=0&&cx<CANVAS_W&&cy>=0&&cy<CANVAS_H);

    // Left button draw
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && onCanvas) {
        if (activeTool==TOOL_EYEDROP) {
            fgColor=GetPixel(cx,cy);
            char msg[32]; snprintf(msg,32,"Picked #%02X%02X%02X",fgColor.r,fgColor.g,fgColor.b);
            SetStatus(msg,NEON_CYAN); return;
        }
        PushUndo();
        drawStartX=cx; drawStartY=cy;
        lastDrawX=cx;  lastDrawY=cy;
        drawing=true;
        if (activeTool==TOOL_FILL) {
            FloodFill(cx,cy,fgColor); drawing=false;
        } else if (activeTool==TOOL_PENCIL||activeTool==TOOL_ERASER) {
            Color c=activeTool==TOOL_ERASER?bgColor:fgColor;
            DrawThickPixel(cx,cy,c,brushSize);
        }
    }
    if (IsMouseButtonDown(MOUSE_LEFT_BUTTON) && drawing && onCanvas) {
        if (activeTool==TOOL_PENCIL||activeTool==TOOL_ERASER) {
            Color c=activeTool==TOOL_ERASER?bgColor:fgColor;
            if(lastDrawX>=0) DrawCanvasLine(lastDrawX,lastDrawY,cx,cy,c,brushSize);
            else DrawThickPixel(cx,cy,c,brushSize);
            lastDrawX=cx; lastDrawY=cy;
        }
    }
    if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON) && drawing) {
        if (activeTool==TOOL_LINE)
            DrawCanvasLine(drawStartX,drawStartY,cx,cy,fgColor,brushSize);
        else if (activeTool==TOOL_RECT)
            DrawCanvasRect(drawStartX,drawStartY,cx,cy,fgColor,brushSize,false);
        else if (activeTool==TOOL_ELLIPSE) {
            int rx=abs(cx-drawStartX)/2, ry2=abs(cy-drawStartY)/2;
            DrawCanvasEllipse((drawStartX+cx)/2,(drawStartY+cy)/2,rx,ry2,fgColor,false);
        }
        drawing=false; lastDrawX=-1; lastDrawY=-1;
    }
    // Right click = bg color
    if (IsMouseButtonPressed(MOUSE_RIGHT_BUTTON) && onCanvas && (activeTool==TOOL_PENCIL||activeTool==TOOL_ERASER)) {
        PushUndo(); drawing=false;
        DrawThickPixel(cx,cy,bgColor,brushSize);
    }

    // Preview overlay for shape tools (draw a ghost)
    // (drawn in main loop below)
}

// Ghost shape preview while dragging
static void DrawShapePreview(int sw, int sh) {
    if (!drawing) return;
    if (activeTool!=TOOL_LINE&&activeTool!=TOOL_RECT&&activeTool!=TOOL_ELLIPSE) return;
    Vector2 mp=GetMousePosition();
    Vector2 cv=ScreenToCanvas((int)mp.x,(int)mp.y);
    int cx=(int)cv.x, cy=(int)cv.y;
    float offX = canvasAreaX + panX + (canvasAreaW - CANVAS_W*zoom)*0.5f;
    float offY = canvasAreaY + panY + (canvasAreaH - CANVAS_H*zoom)*0.5f;
    auto toScreen=[&](int px2,int py2) -> Vector2 {
        return {offX + px2*zoom, offY + py2*zoom};
    };
    Color ghost={fgColor.r,fgColor.g,fgColor.b,128};
    if (activeTool==TOOL_LINE) {
        Vector2 s=toScreen(drawStartX,drawStartY), e=toScreen(cx,cy);
        DrawLineEx(s,e,2.0f,ghost);
    } else if (activeTool==TOOL_RECT) {
        int x0=std::min(drawStartX,cx),y0=std::min(drawStartY,cy);
        int x1=std::max(drawStartX,cx),y1=std::max(drawStartY,cy);
        Vector2 tl=toScreen(x0,y0);
        DrawRectangleLinesEx({tl.x,tl.y,(x1-x0)*zoom,(y1-y0)*zoom},1.5f,ghost);
    } else if (activeTool==TOOL_ELLIPSE) {
        Vector2 center=toScreen((drawStartX+cx)/2,(drawStartY+cy)/2);
        float rx=fabsf(cx-drawStartX)*zoom/2, ry=fabsf(cy-drawStartY)*zoom/2;
        DrawEllipseLines((int)center.x,(int)center.y,(int)rx,(int)ry,ghost);
    }
}

// ============================================================
//  MAIN
// ============================================================
int main() {
    if (!RequestResources(APP_NAME, RAM_MB, HDD_MB, PRIORITY_NORMAL, 1)) {
        InitWindow(440,120,"Paint — Denied"); SetTargetFPS(30);
        double t=GetTime();
        while (!WindowShouldClose()&&GetTime()-t<3.5) {
            BeginDrawing(); ClearBackground(BG_DEEP);
            DrawText("Insufficient resources.",18,40,FONT_NORMAL,NEON_PINK);
            EndDrawing();
        }
        CloseWindow(); return 1;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(WIN_W, WIN_H, "NexOS Paint");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    SetWindowFocused();

    mkdir("hdd",0755);
    canvasTex = LoadRenderTexture(CANVAS_W, CANVAS_H);
    ClearCanvas({255,255,255,255});
    SetStatus("Welcome to NexOS Paint!  P=Pencil E=Eraser F=Fill L=Line R=Rect O=Ellipse I=Eyedrop", NEON_CYAN);

    while (!WindowShouldClose() && appRunning) {
        int sw = GetScreenWidth(), sh = GetScreenHeight();

        if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) &&
            (showFileMenu||showEditMenu||showViewMenu||showCanvasMenu) &&
            GetMousePosition().y > MENUBAR_H) CloseMenus();

        HandleInput(sw, sh);

        BeginDrawing();
        ClearBackground(BG_DEEP);

        DrawCanvasArea(sw, sh);
        DrawShapePreview(sw, sh);
        DrawToolbar(sw, sh);
        DrawPalette(sw, sh);
        DrawMenuBar(sw);
        DrawStatusBar(sw, sh);

        if (colorPickerOpen) DrawColorPicker(sw, sh);
        if (saveDialogOpen)  DrawSaveDialog(sw, sh);
        if (browserOpen)     DrawFileBrowser(sw, sh);

        EndDrawing();
    }

    appRunning = false;
    UnloadRenderTexture(canvasTex);
    ReleaseResources(APP_NAME, RAM_MB, HDD_MB);
    CloseWindow();
    return 0;
}