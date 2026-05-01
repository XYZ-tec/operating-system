
#include "raylib.h"
#include "../include/theme.h"
#include "../include/resources.h"
#include "../include/ipc.h"

#include <unistd.h>
#include <pthread.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <dirent.h>

#define APP_NAME    "Notepad"
#define RAM_MB      50
#define HDD_MB      20
#define MENUBAR_H   32
#define STATUSBAR_H 26
#define FINDBAR_H   36
#define GUTTER_W    56
#define PADDING      8
#define ED_FONT     16
#define ED_LINE_H   22

static const Color ED_BG      = {  7,  7, 16, 255 };
static const Color ED_GUTTER  = { 12, 12, 26, 255 };
static const Color ED_LINEHIGH= { 18, 18, 42, 255 };
static const Color ED_LINENUM = { 60, 60,100, 255 };
static const Color ED_CURSOR_C= {  0,255,200, 255 };
static const Color ED_FIND_HL = {255,210,  0,  55 };
static const Color ED_SEL_HL  = {  0,120,220,  70 };

// ── Document ──────────────────────────────────────────────
static std::vector<std::string> lines;
static int  cursorRow=0, cursorCol=0, scrollRow=0;

// ── File tracking ─────────────────────────────────────────

static char currentFile[256] = "";
static bool isUntitled  = true;   // TRUE until user gives it a name
static bool isDirty     = false;

// ── Thread state ──────────────────────────────────────────
static pthread_mutex_t docMutex   = PTHREAD_MUTEX_INITIALIZER;
static volatile bool   appRunning = true;
static volatile bool   autoSaveSignal = false;

// ── Status ────────────────────────────────────────────────
static char   statusMsg[192]="";
static double statusAt=-999.0;
static Color  statusColor={0,255,200,255};
static void SetStatus(const char* m, Color c={0,255,200,255})
{
     strncpy(statusMsg,m,191); statusAt=GetTime(); statusColor=c;
}

// ── Clipboard ─────────────────────────────────────────────
static std::string clipboard="";

// ── Selection ─────────────────────────────────────────────
static bool hasSelect=false;
static int  selAnchorRow=0, selAnchorCol=0;

// ── Font ──────────────────────────────────────────────────
static Font edFont; 
static bool edFontOK=false;
static void DT(const char* t,int x,int y,int sz,Color c)
{
    if(edFontOK) 
        DrawTextEx(edFont,t,{(float)x,(float)y},(float)sz,1.0f,c);
    else 
        DrawText(t,x,y,sz,c);
    }
static int MT(const char* t,int sz)
{
    if(edFontOK) 
    return(int)MeasureTextEx(edFont,t,(float)sz,1.0f).x;
    return MeasureText(t,sz);}

// ── Menus ─────────────────────────────────────────────────
static bool showFileMenu=false, showEditMenu=false, showViewMenu=false;
static bool showLineNums=true, showStatusBar=true;
static void CloseMenus()
{
    showFileMenu=showEditMenu=showViewMenu=false;
}

// ── Find ──────────────────────────────────────────────────
static bool findOpen=false, replaceOpen=false, findFocused=true;
static char findBuf[128]="", repBuf[128]="";
static int  findLen=0, repLen=0;
static std::vector<std::pair<int,int>> findMatches;
static int  findIdx=-1;
static bool findNoMatch=false;

// ── Save As dialog ────────────────────────────────────────
static bool saveAsOpen=false;
static char saveAsBuf[96]="";
static int  saveAsLen=0;

// ── File browser ──────────────────────────────────────────
static bool browserOpen=false;
static std::vector<std::string> browserFiles;
static int  browserScroll=0;

// ── Unsaved prompt ────────────────────────────────────────
static bool unsavedPrompt=false;
static int  unsavedAction=0; // 1=new 2=open 3=quit

static const char* DisplayName()
{
    if(isUntitled) 
    return "untitled";
    const char* sl=strrchr(currentFile,'/');
    return sl?sl+1:currentFile;
}

static bool WriteToDisk(const char* path){
    mkdir("hdd",0755);
    pthread_mutex_lock(&docMutex);
    std::ofstream f(path);
    bool ok=false;
    if(f.is_open())
    {
        for(int i=0;i<(int)lines.size();i++)
        {
            f<<lines[i];
            if(i+1<(int)lines.size())f<<'\n';
        }
        f.close(); ok=true;
    }
    pthread_mutex_unlock(&docMutex);
    return ok;
}

// Save to currentFile 
static void SaveCurrent()
{
    if(isUntitled)
    {
        SetStatus("Use Ctrl+Shift+S or Save As to name this file first.",NEON_GOLD);
        return;
    }
    if(WriteToDisk(currentFile))
    {
        isDirty=false;SetStatus("Saved.",NEON_GREEN);
    }
    else 
    SetStatus("Error: could not write file.",NEON_PINK);
}

static void CommitSaveAs(const char* name)
{
    char path[300]; 
    sprintf(path,"hdd/%s.txt",name);
    if(WriteToDisk(path))
    {
        isDirty=false;
        SetStatus("Saved as new file.",NEON_GREEN);
    } 
    else {
        SetStatus("Error: could not create file.",NEON_PINK);
    }
}

static void LoadFromDisk(const char* path)
{
    pthread_mutex_lock(&docMutex);
    lines.clear();
    std::ifstream f(path);
    if(f.is_open())
    {
        std::string ln;
        while(std::getline(f,ln))lines.push_back(ln);
        f.close();
    }
    if(lines.empty())lines.push_back("");
    strncpy(currentFile,path,255);
    isUntitled=false; isDirty=false;
    cursorRow=cursorCol=scrollRow=0;
    hasSelect=false;
    pthread_mutex_unlock(&docMutex);
}

static void NewDocument()
{
    lines.clear(); lines.push_back("");
    currentFile[0]='\0';
    isUntitled=true; isDirty=false;
    cursorRow=cursorCol=scrollRow=0;
    hasSelect=false;
    SetStatus("New file press Ctrl+S to name and save it.");
}

// ── Auto-save thread ──────────────────────────────────────
static void* AutoSaveThread(void*)
{
    while(appRunning)
    {
        for(int i=0;i<20&&appRunning;i++)
        usleep(500000);
        if(!appRunning)
        break;
        pthread_mutex_lock(&docMutex);
        bool dirty=isDirty;
        bool named=!isUntitled;
        char path[256]; 
        strncpy(path,currentFile,255);
        pthread_mutex_unlock(&docMutex);
        if(dirty&&named)
        {
            WriteToDisk(path);
            pthread_mutex_lock(&docMutex);
            isDirty=false;
            pthread_mutex_unlock(&docMutex);
            autoSaveSignal=true;
        }
    }
    return nullptr;
}

// ── File browser ──────────────────────────────────────────
static void OpenBrowser()
{
    browserFiles.clear();
    browserScroll=0;
    mkdir("hdd",0755);
    DIR* d=opendir("hdd");
    if(d)
    {
        struct dirent* e;
        while((e=readdir(d)))
        {
            std::string n(e->d_name);
            if(n!="."&&n!="..")
            browserFiles.push_back(n);
        }
        closedir(d);
    }
    std::sort(browserFiles.begin(),browserFiles.end());
    browserOpen=true;
}

// ============================================================
//  DOCUMENT EDITING
// ============================================================
static void EnsureLines(){if(lines.empty())lines.push_back("");}
static void ClampCursor(){
    EnsureLines();
    cursorRow=std::max(0,std::min(cursorRow,(int)lines.size()-1));
    cursorCol=std::max(0,std::min(cursorCol,(int)lines[cursorRow].size()));
}

static void StartSel(){if(!hasSelect){hasSelect=true;selAnchorRow=cursorRow;selAnchorCol=cursorCol;}}
static void ClearSel(){hasSelect=false;}
static void GetSelRange(int&r1,int&c1,int&r2,int&c2){
    r1=selAnchorRow;c1=selAnchorCol;r2=cursorRow;c2=cursorCol;
    if(r1>r2||(r1==r2&&c1>c2)){std::swap(r1,r2);std::swap(c1,c2);}
}
static std::string GetSelText(){
    if(!hasSelect)return"";
    int r1,c1,r2,c2;GetSelRange(r1,c1,r2,c2);
    std::string res;
    for(int r=r1;r<=r2;r++){
        int s=(r==r1)?c1:0,e=(r==r2)?c2:(int)lines[r].size();
        res+=lines[r].substr(s,e-s);if(r<r2)res+='\n';
    }
    return res;
}
static void DelSel(){
    if(!hasSelect)return;
    int r1,c1,r2,c2;GetSelRange(r1,c1,r2,c2);
    std::string bef=lines[r1].substr(0,c1),aft=(r2<(int)lines.size())?lines[r2].substr(c2):"";
    lines[r1]=bef+aft;
    lines.erase(lines.begin()+r1+1,lines.begin()+r2+1);
    cursorRow=r1;cursorCol=c1;hasSelect=false;isDirty=true;
}
static void SelectAll(){EnsureLines();hasSelect=true;selAnchorRow=0;selAnchorCol=0;cursorRow=(int)lines.size()-1;cursorCol=(int)lines.back().size();}
static void InsertChar(char c){if(hasSelect)DelSel();EnsureLines();lines[cursorRow].insert(cursorCol,1,c);cursorCol++;isDirty=true;}
static void InsertNewline(){if(hasSelect)DelSel();EnsureLines();std::string r=lines[cursorRow].substr(cursorCol);lines[cursorRow]=lines[cursorRow].substr(0,cursorCol);lines.insert(lines.begin()+cursorRow+1,r);cursorRow++;cursorCol=0;isDirty=true;}
static void DelBack(){if(hasSelect){DelSel();return;}EnsureLines();if(cursorCol>0){lines[cursorRow].erase(cursorCol-1,1);cursorCol--;isDirty=true;}else if(cursorRow>0){cursorCol=(int)lines[cursorRow-1].size();lines[cursorRow-1]+=lines[cursorRow];lines.erase(lines.begin()+cursorRow);cursorRow--;isDirty=true;}}
static void DelFwd(){if(hasSelect){DelSel();return;}EnsureLines();if(cursorCol<(int)lines[cursorRow].size()){lines[cursorRow].erase(cursorCol,1);isDirty=true;}else if(cursorRow<(int)lines.size()-1){lines[cursorRow]+=lines[cursorRow+1];lines.erase(lines.begin()+cursorRow+1);isDirty=true;}}
static void DoCopy(){clipboard=hasSelect?GetSelText():lines[cursorRow];SetStatus("Copied.");}
static void DoCut(){if(hasSelect){clipboard=GetSelText();DelSel();}else{clipboard=lines[cursorRow];if(lines.size()==1)lines[0]="";else{lines.erase(lines.begin()+cursorRow);cursorRow=std::min(cursorRow,(int)lines.size()-1);cursorCol=0;}isDirty=true;}SetStatus("Cut.");}
static void DoPaste(){if(clipboard.empty()){SetStatus("Clipboard empty.");return;}if(hasSelect)DelSel();for(char c:clipboard){if(c=='\n')InsertNewline();else InsertChar(c);}SetStatus("Pasted.");}

// ── Find ──────────────────────────────────────────────────
static void RunFind(){
    findMatches.clear();findIdx=-1;findNoMatch=false;
    if(findLen==0)return;
    std::string q(findBuf);
    for(int r=0;r<(int)lines.size();r++){size_t p=0;while((p=lines[r].find(q,p))!=std::string::npos){findMatches.push_back({r,(int)p});p+=q.size();}}
    if(findMatches.empty()){findNoMatch=true;SetStatus("No match found.",NEON_PINK);}
    else{findIdx=0;cursorRow=findMatches[0].first;cursorCol=findMatches[0].second;char m[64];sprintf(m,"Found %d match(es).",(int)findMatches.size());SetStatus(m,NEON_GREEN);}
}
static void FindNext(){if(findMatches.empty()){SetStatus("No match.",NEON_PINK);return;}findIdx=(findIdx+1)%(int)findMatches.size();cursorRow=findMatches[findIdx].first;cursorCol=findMatches[findIdx].second;}
static void FindPrev(){if(findMatches.empty()){SetStatus("No match.",NEON_PINK);return;}findIdx=(findIdx-1+(int)findMatches.size())%(int)findMatches.size();cursorRow=findMatches[findIdx].first;cursorCol=findMatches[findIdx].second;}
static void ReplaceOne(){if(findMatches.empty()||findIdx<0){SetStatus("Nothing to replace.",NEON_PINK);return;}auto[r,c]=findMatches[findIdx];lines[r].erase(c,findLen);lines[r].insert(c,repBuf);isDirty=true;RunFind();SetStatus("Replaced.",NEON_GOLD);}
static void ReplaceAll(){if(!findLen){SetStatus("Find field empty.",NEON_PINK);return;}int cnt=0;for(auto&ln:lines){size_t p=0;std::string q(findBuf),rp(repBuf);while((p=ln.find(q,p))!=std::string::npos){ln.replace(p,q.size(),rp);p+=rp.size();cnt++;}}isDirty=true;RunFind();char m[64];sprintf(m,"Replaced %d.",cnt);SetStatus(m,NEON_GOLD);}

// ── Stats ─────────────────────────────────────────────────
static void GetStats(int&tl,int&tw,int&tc){tl=(int)lines.size();tw=0;tc=0;bool w=false;for(auto&ln:lines){tc+=(int)ln.size();for(char c:ln){if(isspace(c))w=false;else if(!w){w=true;tw++;}}w=false;}}

// ============================================================
//  DRAW: SAVE AS DIALOG
// ============================================================
static void DrawSaveAsDialog(int sw,int sh){
    if(!saveAsOpen)return;
    DrawRectangle(0,0,sw,sh,{0,0,0,215});
    int pw=540,ph=260,px=(sw-pw)/2,py=(sh-ph)/2;
    DrawRectangle(px,py,pw,ph,BG_PANEL);
    DrawGlowRect({(float)px,(float)py,(float)pw,(float)ph},NEON_CYAN,7);
    DrawRectangle(px,py,pw,38,BG_TITLEBAR);
    DT("Save As - Name Your File",px+14,py+11,FONT_LARGE,NEON_CYAN);
    DrawLine(px,py+38,px+pw,py+38,BORDER_DIM);

    DT("Each file is saved separately in  hdd/  folder.",px+16,py+52,FONT_SMALL,TEXT_MUTED);
    DT("Allowed characters: letters, numbers, _ and -",px+16,py+72,FONT_TINY,TEXT_DIM);

    // Input box
    Rectangle box={(float)(px+16),(float)(py+98),(float)(pw-32),48};
    DrawRectangleRec(box,BG_DEEP);
    DrawRectangleLinesEx(box,2.2f,NEON_CYAN);
    DrawGlowRect(box,NEON_CYAN,4);

    DT("hdd/",(int)box.x+10,(int)box.y+13,FONT_NORMAL,TEXT_DIM);
    int pfx=MT("hdd/",FONT_NORMAL);

    DT(saveAsBuf,(int)box.x+pfx+16,(int)box.y+13,FONT_NORMAL,TEXT_PRIMARY);
    int nw=MT(saveAsBuf,FONT_NORMAL);
    DT(".txt",(int)box.x+pfx+18+nw,(int)box.y+13,FONT_NORMAL,TEXT_DIM);

    // Blinking cursor in input
    if((int)(GetTime()*2)%2==0)
        DT("|",(int)box.x+pfx+16+nw,(int)box.y+13,FONT_NORMAL,NEON_CYAN);

    // Preview / validation line
    if(saveAsLen>0){
        char prev[300]; sprintf(prev,"OK - Will create: hdd/%s.txt",saveAsBuf);
        DT(prev,px+16,py+158,FONT_SMALL,NEON_GREEN);
    } else {
        DT("Type a filename above, then press Enter or click Save",px+16,py+158,FONT_SMALL,NEON_GOLD);
    }

    // Buttons
    Rectangle saveBtn={(float)(px+pw-238),(float)(py+200),112,38};
    Rectangle cancelBtn={(float)(px+pw-118),(float)(py+200),106,38};

    bool canSave=(saveAsLen>0);
    bool shov=canSave&&CheckCollisionPointRec(GetMousePosition(),saveBtn);
    DrawRectangleRec(saveBtn,shov?NEON_CYAN:BG_HOVER);
    DrawRectangleLinesEx(saveBtn,1.8f,canSave?NEON_CYAN:BORDER_DIM);
    int slw=MT("Save",FONT_NORMAL);
    DT("Save",(int)(saveBtn.x+(saveBtn.width-slw)/2),(int)(saveBtn.y+10),
       FONT_NORMAL,shov?BG_DEEP:(canSave?NEON_CYAN:TEXT_DIM));

    auto doSave=[&](){
        CommitSaveAs(saveAsBuf);
        saveAsOpen=false;
    };
    if(shov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON))doSave();
    if(DrawButton(cancelBtn,"Cancel",BG_HOVER,NEON_PINK,FONT_NORMAL))saveAsOpen=false;

    // Keyboard input for the dialog
    int k=GetCharPressed();
    while(k>0){
        bool ok=(k>='a'&&k<='z')||(k>='A'&&k<='Z')||(k>='0'&&k<='9')||k=='_'||k=='-';
        if(ok&&saveAsLen<80){saveAsBuf[saveAsLen++]=(char)k;saveAsBuf[saveAsLen]='\0';}
        k=GetCharPressed();
    }
    if(IsKeyPressed(KEY_BACKSPACE)&&saveAsLen>0) saveAsBuf[--saveAsLen]='\0';
    if(IsKeyPressed(KEY_ENTER)&&saveAsLen>0) doSave();
    if(IsKeyPressed(KEY_ESCAPE)) saveAsOpen=false;
}

// ============================================================
//  DRAW: FILE BROWSER
// ============================================================
static void DrawFileBrowser(int sw,int sh){
    if(!browserOpen)return;
    DrawRectangle(0,0,sw,sh,{0,0,0,195});
    int pw=500,ph=420,px=(sw-pw)/2,py=(sh-ph)/2;
    DrawRectangle(px,py,pw,ph,BG_PANEL);
    DrawGlowRect({(float)px,(float)py,(float)pw,(float)ph},NEON_CYAN,5);
    DrawRectangle(px,py,pw,38,BG_TITLEBAR);
    DT("Open File",px+14,py+11,FONT_LARGE,NEON_CYAN);
    DrawLine(px,py+38,px+pw,py+38,BORDER_DIM);
    DT("Showing files in:  hdd/",px+14,py+46,FONT_TINY,TEXT_DIM);

    Vector2 mouse=GetMousePosition();
    int vis=11,si=browserScroll;
    for(int i=si;i<(int)browserFiles.size()&&i<si+vis;i++){
        int ry=py+64+(i-si)*28;
        Rectangle row={(float)(px+8),(float)ry,(float)(pw-20),26};
        bool hov=CheckCollisionPointRec(mouse,row);
        if(hov)DrawRectangleRec(row,BG_HOVER);
        // File icon
        DrawRectangleLinesEx({(float)(px+14),(float)(ry+5),14,16},1,hov?NEON_CYAN:BORDER_DIM);
        DT(browserFiles[i].c_str(),px+36,ry+5,FONT_SMALL,hov?NEON_CYAN:TEXT_PRIMARY);
        if(hov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
            char path[300];sprintf(path,"hdd/%s",browserFiles[i].c_str());
            LoadFromDisk(path);browserOpen=false;
            SetStatus("File opened.",NEON_GREEN);
        }
    }
    if(browserFiles.empty()){
        DT("No files saved yet.",px+20,py+85,FONT_NORMAL,TEXT_MUTED);
        DT("Use Ctrl+Shift+S to save a named file first.",px+20,py+115,FONT_SMALL,TEXT_DIM);
    }
    if(browserScroll>0)
        if(DrawButton({(float)(px+pw-44),(float)(py+62),36,22},"^",BG_HOVER,TEXT_MUTED,FONT_SMALL))browserScroll--;
    if(browserScroll+vis<(int)browserFiles.size())
        if(DrawButton({(float)(px+pw-44),(float)(py+ph-34),36,22},"v",BG_HOVER,TEXT_MUTED,FONT_SMALL))browserScroll++;
    if(DrawButton({(float)(px+pw-92),(float)(py+ph-40),84,30},"Cancel",BG_HOVER,NEON_PINK,FONT_SMALL))browserOpen=false;
    if(IsKeyPressed(KEY_ESCAPE))browserOpen=false;
}

// ============================================================
//  DRAW: UNSAVED PROMPT
// ============================================================
static void DrawUnsavedPrompt(int sw,int sh){
    if(!unsavedPrompt)return;
    DrawRectangle(0,0,sw,sh,{0,0,0,200});
    int pw=440,ph=165,px=(sw-pw)/2,py=(sh-ph)/2;
    DrawRectangle(px,py,pw,ph,BG_PANEL);
    DrawGlowRect({(float)px,(float)py,(float)pw,(float)ph},NEON_GOLD,5);
    DT("Unsaved Changes",px+14,py+14,FONT_LARGE,NEON_GOLD);
    DrawLine(px,py+42,px+pw,py+42,BORDER_DIM);
    DT("You have unsaved changes. Save before continuing?",px+14,py+58,FONT_SMALL,TEXT_PRIMARY);
    char fn[80];sprintf(fn,"File: %s",isUntitled?"(untitled)":DisplayName());
    DT(fn,px+14,py+82,FONT_TINY,TEXT_MUTED);

    auto act=[&](){
        unsavedPrompt=false;
        if(unsavedAction==1) NewDocument();
        else if(unsavedAction==2) OpenBrowser();
        else if(unsavedAction==3) appRunning=false;
    };

    if(DrawButton({(float)(px+14),(float)(py+118),100,34},"Save",BG_HOVER,NEON_CYAN,FONT_NORMAL)){
        if(isUntitled){saveAsOpen=true;memset(saveAsBuf,0,96);saveAsLen=0;unsavedPrompt=false;}
        else{SaveCurrent();act();}
    }
    if(DrawButton({(float)(px+124),(float)(py+118),136,34},"Don't Save",BG_HOVER,NEON_ORANGE,FONT_NORMAL)){isDirty=false;act();}
    if(DrawButton({(float)(px+pw-114),(float)(py+118),100,34},"Cancel",BG_HOVER,NEON_PINK,FONT_NORMAL))unsavedPrompt=false;
    if(IsKeyPressed(KEY_ESCAPE))unsavedPrompt=false;
}

// ============================================================
//  DRAW: MENU BAR
// ============================================================
static void DrawMenuBar(int sw){
    DrawRectangle(0,0,sw,MENUBAR_H,BG_TITLEBAR);
    DrawLine(0,MENUBAR_H,sw,MENUBAR_H,BORDER_DIM);

    struct ME{const char* l;bool* f;int x;}menus[3];
    int mx=8;
    menus[0]={"File",&showFileMenu,mx};mx+=MT("File",FONT_SMALL)+22;
    menus[1]={"Edit",&showEditMenu,mx};mx+=MT("Edit",FONT_SMALL)+22;
    menus[2]={"View",&showViewMenu,mx};
    for(auto&m:menus){
        int w=MT(m.l,FONT_SMALL)+16;
        Rectangle r={(float)(m.x-4),4,(float)w,(float)(MENUBAR_H-8)};
        bool h=CheckCollisionPointRec(GetMousePosition(),r);
        if(h||*m.f)DrawRectangleRec(r,BG_HOVER);
        DT(m.l,m.x+2,9,FONT_SMALL,(h||*m.f)?NEON_CYAN:TEXT_PRIMARY);
        if(h&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){bool was=*m.f;CloseMenus();if(!was)*m.f=true;}
    }

    // Title - shows filename + dirty marker
    char title[200];
    sprintf(title,"%s%s - NexOS Notepad",isDirty?"* ":"",isUntitled?"untitled":DisplayName());
    int tw=MT(title,FONT_SMALL);
    DT(title,(sw-tw)/2,9,FONT_SMALL,isDirty?NEON_GOLD:TEXT_MUTED);

    DT("Ctrl+S  Ctrl+Shift+S  Ctrl+F  Ctrl+O  Ctrl+N",sw-340,10,FONT_TINY,TEXT_DIM);

    if(autoSaveSignal&&GetTime()-statusAt<2.0){
        DrawRectangle(sw-128,5,120,22,{0,255,200,16});
        DT("[OK] Auto-saved",sw-124,9,FONT_TINY,NEON_CYAN);
    }

    // ── FILE MENU ──
    if(showFileMenu){
        struct{const char* l;const char* sc;Color c;}items[]={
            {"New",               "Ctrl+N",      TEXT_PRIMARY},
            {"Open",              "Ctrl+O",      TEXT_PRIMARY},
            {"Save",              "Ctrl+S",      TEXT_PRIMARY},
            {"Save As (new name)","Ctrl+Shift+S",TEXT_PRIMARY},
            {"-------------------", "",            TEXT_DIM},
            {"Exit",              "",            NEON_PINK},
        };
        int ic=6,bx=4,by=MENUBAR_H,bw=226,bh=28;
        DrawRectangle(bx,by,bw,ic*bh+4,BG_PANEL);
        DrawRectangleLinesEx({(float)bx,(float)by,(float)bw,(float)(ic*bh+4)},1,NEON_CYAN);
        for(int i=0;i<ic;i++){
            Rectangle ir={(float)(bx+2),(float)(by+2+i*bh),(float)(bw-4),(float)bh};
            bool sep=(strncmp(items[i].l,"-",3)==0);
            bool ih=!sep&&CheckCollisionPointRec(GetMousePosition(),ir);
            if(ih)DrawRectangleRec(ir,BG_HOVER);
            DT(items[i].l,(int)ir.x+10,(int)ir.y+7,FONT_SMALL,ih?NEON_CYAN:items[i].c);
            if(strlen(items[i].sc))DT(items[i].sc,(int)(ir.x+bw-MT(items[i].sc,FONT_TINY)-8),(int)ir.y+9,FONT_TINY,TEXT_DIM);
            if(ih&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
                CloseMenus();
                if(i==0){if(isDirty){unsavedPrompt=true;unsavedAction=1;}else NewDocument();}
                else if(i==1){if(isDirty){unsavedPrompt=true;unsavedAction=2;}else OpenBrowser();}
                else if(i==2) SaveCurrent();
                else if(i==3){saveAsOpen=true;memset(saveAsBuf,0,96);saveAsLen=0;}
                else if(i==5){if(isDirty){unsavedPrompt=true;unsavedAction=3;}else appRunning=false;}
            }
        }
    }

    // ── EDIT MENU ──
    if(showEditMenu){
        struct{const char* l;const char* sc;}items[]={
            {"Select All",    "Ctrl+A"},
            {"Copy",          "Ctrl+C"},
            {"Cut",           "Ctrl+X"},
            {"Paste",         "Ctrl+V"},
            {"---------------", ""},
            {"Find",          "Ctrl+F"},
            {"Find & Replace","Ctrl+H"},
        };
        int ic=7,bx=MT("File",FONT_SMALL)+26,by=MENUBAR_H,bw=220,bh=28;
        DrawRectangle(bx,by,bw,ic*bh+4,BG_PANEL);
        DrawRectangleLinesEx({(float)bx,(float)by,(float)bw,(float)(ic*bh+4)},1,NEON_CYAN);
        for(int i=0;i<ic;i++){
            Rectangle ir={(float)(bx+2),(float)(by+2+i*bh),(float)(bw-4),(float)bh};
            bool sep=(strncmp(items[i].l,"-",3)==0);
            bool ih=!sep&&CheckCollisionPointRec(GetMousePosition(),ir);
            if(ih)DrawRectangleRec(ir,BG_HOVER);
            DT(items[i].l,(int)ir.x+10,(int)ir.y+7,FONT_SMALL,ih?NEON_CYAN:TEXT_PRIMARY);
            if(strlen(items[i].sc))DT(items[i].sc,(int)(ir.x+bw-MT(items[i].sc,FONT_TINY)-8),(int)ir.y+9,FONT_TINY,TEXT_DIM);
            if(ih&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
                CloseMenus();
                if(i==0)SelectAll();
                else if(i==1)DoCopy();
                else if(i==2)DoCut();
                else if(i==3)DoPaste();
                else if(i==5){findOpen=true;replaceOpen=false;findFocused=true;memset(findBuf,0,128);findLen=0;findMatches.clear();findNoMatch=false;}
                else if(i==6){findOpen=true;replaceOpen=true;findFocused=true;memset(findBuf,0,128);findLen=0;findMatches.clear();findNoMatch=false;}
            }
        }
    }

    // ── VIEW MENU ──
    if(showViewMenu){
        struct{const char* l;bool* t;}items[]={{"Line Numbers",&showLineNums},{"Status Bar",&showStatusBar}};
        int ic=2,bx=MT("File",FONT_SMALL)+26+MT("Edit",FONT_SMALL)+26,by=MENUBAR_H,bw=190,bh=28;
        DrawRectangle(bx,by,bw,ic*bh+4,BG_PANEL);
        DrawRectangleLinesEx({(float)bx,(float)by,(float)bw,(float)(ic*bh+4)},1,NEON_CYAN);
        for(int i=0;i<ic;i++){
            Rectangle ir={(float)(bx+2),(float)(by+2+i*bh),(float)(bw-4),(float)bh};
            bool ih=CheckCollisionPointRec(GetMousePosition(),ir);
            if(ih)DrawRectangleRec(ir,BG_HOVER);
            DT(*items[i].t?"x":" ",(int)ir.x+8,(int)ir.y+7,FONT_SMALL,NEON_CYAN);
            DT(items[i].l,(int)ir.x+26,(int)ir.y+7,FONT_SMALL,ih?NEON_CYAN:TEXT_PRIMARY);
            if(ih&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){*items[i].t=!*items[i].t;CloseMenus();}
        }
    }
}

// ============================================================
//  DRAW: FIND BAR
// ============================================================
static void DrawFindBar(int sw,int y){
    int barH=replaceOpen?FINDBAR_H*2:FINDBAR_H;
    DrawRectangle(0,y,sw,barH,{14,14,32,255});
    DrawLine(0,y,sw,y,{0,255,200,55});
    DrawLine(0,y+barH,sw,y+barH,BORDER_DIM);

    // Find row
    DT("Find:",8,y+10,FONT_SMALL,TEXT_MUTED);
    int bx=54,bw=sw-370;
    Rectangle fb={(float)bx,(float)(y+5),(float)bw,26};
    Color bord=findNoMatch?NEON_PINK:(findFocused?NEON_CYAN:BORDER_DIM);
    Color bg2=findNoMatch?Color{40,8,8,255}:BG_DEEP;
    DrawRectangleRec(fb,bg2);
    DrawRectangleLinesEx(fb,findFocused?2.0f:1.5f,bord);
    DT(findBuf,bx+6,y+9,FONT_SMALL,findNoMatch?NEON_PINK:TEXT_PRIMARY);
    if(findFocused&&(int)(GetTime()*2)%2==0){int fw=MT(findBuf,FONT_SMALL);DT("|",bx+8+fw,y+9,FONT_SMALL,NEON_CYAN);}
    if(!findMatches.empty()){char rc[20];sprintf(rc,"%d/%d",findIdx+1,(int)findMatches.size());DT(rc,(int)(fb.x+fb.width-MT(rc,FONT_TINY)-6),y+11,FONT_TINY,NEON_GREEN);}
    if(IsMouseButtonPressed(MOUSE_LEFT_BUTTON)&&CheckCollisionPointRec(GetMousePosition(),fb))findFocused=true;

    int btnX=bx+bw+8;
    if(DrawButton({(float)btnX,(float)(y+5),46,26},"Prev",BG_HOVER,NEON_CYAN,FONT_TINY))FindPrev();
    if(DrawButton({(float)(btnX+52),(float)(y+5),46,26},"Next",BG_HOVER,NEON_CYAN,FONT_TINY))FindNext();
    if(DrawButton({(float)(btnX+104),(float)(y+5),78,26},"Replace",BG_HOVER,NEON_GOLD,FONT_TINY))replaceOpen=!replaceOpen;
    if(DrawButton({(float)(btnX+188),(float)(y+5),66,26},"Close X",BG_HOVER,NEON_PINK,FONT_TINY)){
        findOpen=false;replaceOpen=false;findMatches.clear();findNoMatch=false;findIdx=-1;
        memset(findBuf,0,128);findLen=0;memset(repBuf,0,128);repLen=0;
    }

    // Replace row
    if(replaceOpen){
        int ry=y+FINDBAR_H;
        DT("Replace:",8,ry+10,FONT_SMALL,TEXT_MUTED);
        Rectangle rb={(float)76,(float)(ry+5),(float)(bw-22),26};
        DrawRectangleRec(rb,BG_DEEP);
        DrawRectangleLinesEx(rb,!findFocused?2.0f:1.5f,!findFocused?NEON_PURPLE:BORDER_DIM);
        DT(repBuf,(int)rb.x+6,ry+9,FONT_SMALL,TEXT_PRIMARY);
        if(!findFocused&&(int)(GetTime()*2)%2==0){int rw=MT(repBuf,FONT_SMALL);DT("|",(int)rb.x+8+rw,ry+9,FONT_SMALL,NEON_PURPLE);}
        if(IsMouseButtonPressed(MOUSE_LEFT_BUTTON)&&CheckCollisionPointRec(GetMousePosition(),rb))findFocused=false;
        if(DrawButton({(float)btnX,(float)(ry+5),94,26},"Replace One",BG_HOVER,NEON_GOLD,FONT_TINY))ReplaceOne();
        if(DrawButton({(float)(btnX+100),(float)(ry+5),94,26},"Replace All",BG_HOVER,NEON_ORANGE,FONT_TINY))ReplaceAll();
    }
}

// ============================================================
//  DRAW: STATUS BAR
// ============================================================
static void DrawStatusBar(int sw,int sh){
    int y=sh-STATUSBAR_H;
    DrawRectangle(0,y,sw,STATUSBAR_H,BG_TITLEBAR);
    DrawLine(0,y,sw,y,BORDER_DIM);

    char pos[28];sprintf(pos,"Ln %d  Col %d",cursorRow+1,cursorCol+1);
    DT(pos,10,y+7,FONT_TINY,TEXT_MUTED);

    int tl,tw2,tc;GetStats(tl,tw2,tc);
    char stats[64];sprintf(stats,"Lines: %d   Words: %d   Chars: %d",tl,tw2,tc);
    int sw2=MT(stats,FONT_TINY);DT(stats,(sw-sw2)/2,y+7,FONT_TINY,TEXT_MUTED);

    // File path on left-middle
    char fp[200];
    if(isUntitled) strcpy(fp,"[Not saved] - press Ctrl+Shift+S to save");
    else sprintf(fp,"Saved: %s",currentFile);
    DT(fp,220,y+7,FONT_TINY,isUntitled?NEON_GOLD:TEXT_DIM);

    // Status message bottom right
    double age=GetTime()-statusAt;
    if(age<4.5){
        unsigned char a=(age>3.5)?(unsigned char)((4.5-age)*255):255;
        int mw=MT(statusMsg,FONT_TINY);
        Color mc=statusColor;mc.a=a;
        DT(statusMsg,sw-mw-12,y+7,FONT_TINY,mc);
    }
}

// ============================================================
//  DRAW: EDITOR
// ============================================================
static void DrawEditor(int sw,int cY,int cH){
    DrawRectangle(0,cY,sw,cH,ED_BG);
    int gW=showLineNums?GUTTER_W:0;
    if(showLineNums){DrawRectangle(0,cY,gW,cH,ED_GUTTER);DrawLine(gW,cY,gW,cY+cH,{40,40,80,160});}

    EnsureLines();
    int vis=cH/ED_LINE_H;
    if(cursorRow<scrollRow)scrollRow=cursorRow;
    if(cursorRow>=scrollRow+vis)scrollRow=cursorRow-vis+1;
    scrollRow=std::max(0,scrollRow);
    int last=std::min(scrollRow+vis+1,(int)lines.size());

    int sr1=-1,sc1=0,sr2=-1,sc2=0;
    if(hasSelect)GetSelRange(sr1,sc1,sr2,sc2);

    for(int r=scrollRow;r<last;r++){
        int py2=cY+(r-scrollRow)*ED_LINE_H;
        if(r==cursorRow)DrawRectangle(gW,py2,sw-gW,ED_LINE_H,ED_LINEHIGH);

        if(showLineNums){char ln[8];sprintf(ln,"%d",r+1);int lw=MT(ln,FONT_TINY);DT(ln,gW-lw-5,py2+4,FONT_TINY,r==cursorRow?NEON_CYAN:ED_LINENUM);}

        // Selection highlight
        if(hasSelect&&r>=sr1&&r<=sr2){
            int s=(r==sr1)?sc1:0,e=(r==sr2)?sc2:(int)lines[r].size();
            int hx=gW+PADDING+MT(lines[r].substr(0,s).c_str(),ED_FONT);
            int hw=MT(lines[r].substr(s,e-s).c_str(),ED_FONT);
            if(hw<=0)hw=6;
            DrawRectangle(hx,py2,hw,ED_LINE_H,ED_SEL_HL);
        }

        // Find highlights
        for(auto&[fr,fc]:findMatches){
            if(fr!=r)continue;
            int hx=gW+PADDING+MT(lines[r].substr(0,fc).c_str(),ED_FONT);
            int hw=std::max(4,MT(findBuf,ED_FONT));
            DrawRectangle(hx,py2,hw,ED_LINE_H,ED_FIND_HL);
            DrawRectangleLinesEx({(float)hx,(float)py2,(float)hw,(float)ED_LINE_H},1,{255,210,0,90});
        }

        if(!lines[r].empty())DT(lines[r].c_str(),gW+PADDING,py2+3,ED_FONT,TEXT_PRIMARY);

        // Cursor
        if(r==cursorRow){
            int cx=gW+PADDING;
            if(cursorCol>0)cx+=MT(lines[r].substr(0,cursorCol).c_str(),ED_FONT);
            if((int)(GetTime()*2)%2==0)DrawRectangle(cx,py2+2,2,ED_LINE_H-4,ED_CURSOR_C);
            DrawRectangle(cx-1,py2,4,ED_LINE_H,{0,255,200,16});
        }
    }

    // Scrollbar
    if((int)lines.size()>vis){
        float frac=(float)scrollRow/std::max(1,(int)lines.size()-vis);
        int sbH=std::max(22,(int)((float)vis/(int)lines.size()*cH));
        int sbY=cY+(int)(frac*(cH-sbH));
        DrawRectangle(sw-5,cY,5,cH,{18,18,38,200});
        DrawRectangle(sw-5,sbY,5,sbH,{0,255,200,110});
    }
}

// ============================================================
//  KEYBOARD
// ============================================================
static void HandleKeys(){
    if(saveAsOpen||browserOpen||unsavedPrompt)return;
    if(showFileMenu||showEditMenu||showViewMenu){if(IsKeyPressed(KEY_ESCAPE))CloseMenus();return;}

    bool ctrl=IsKeyDown(KEY_LEFT_CONTROL)||IsKeyDown(KEY_RIGHT_CONTROL);
    bool shift=IsKeyDown(KEY_LEFT_SHIFT)||IsKeyDown(KEY_RIGHT_SHIFT);

    if(ctrl){
        if(IsKeyPressed(KEY_S)){
            if(shift){
                // Ctrl+Shift+S = always Save As (new name)
                saveAsOpen=true;memset(saveAsBuf,0,96);saveAsLen=0;
                // Pre-fill with current name if not untitled
                if(!isUntitled){
                    const char* n=DisplayName();
                    strncpy(saveAsBuf,n,80);saveAsLen=(int)strlen(saveAsBuf);
                    // strip .txt
                    char* dot=strrchr(saveAsBuf,'.');if(dot){*dot='\0';saveAsLen=(int)strlen(saveAsBuf);}
                }
            } else {
                saveAsOpen=true;memset(saveAsBuf,0,96);saveAsLen=0;
                // Pre-fill with current name if not untitled
                if(!isUntitled){
                    const char* n=DisplayName();
                    strncpy(saveAsBuf,n,80);saveAsLen=(int)strlen(saveAsBuf);
                    // strip .txt
                    char* dot=strrchr(saveAsBuf,'.');if(dot){*dot='\0';saveAsLen=(int)strlen(saveAsBuf);}
                }
            }
        }
        if(IsKeyPressed(KEY_N)){if(isDirty){unsavedPrompt=true;unsavedAction=1;}else NewDocument();}
        if(IsKeyPressed(KEY_O)){if(isDirty){unsavedPrompt=true;unsavedAction=2;}else OpenBrowser();}
        if(IsKeyPressed(KEY_A))SelectAll();
        if(IsKeyPressed(KEY_C))DoCopy();
        if(IsKeyPressed(KEY_X))DoCut();
        if(IsKeyPressed(KEY_V))DoPaste();
        if(IsKeyPressed(KEY_F)){
            if(findOpen&&!replaceOpen)findOpen=false;
            else{findOpen=true;replaceOpen=false;findFocused=true;memset(findBuf,0,128);findLen=0;findMatches.clear();findNoMatch=false;}
        }
        if(IsKeyPressed(KEY_H)){findOpen=true;replaceOpen=true;findFocused=true;memset(findBuf,0,128);findLen=0;findMatches.clear();findNoMatch=false;}
        if(IsKeyPressed(KEY_W)){if(isDirty){unsavedPrompt=true;unsavedAction=3;}else appRunning=false;}
        return;
    }

    // Find bar input
    if(findOpen){
        int k=GetCharPressed();
        while(k>0){
            if(k>=32){
                if(findFocused&&findLen<126){findBuf[findLen++]=(char)k;findBuf[findLen]='\0';RunFind();}
                else if(!findFocused&&repLen<126){repBuf[repLen++]=(char)k;repBuf[repLen]='\0';}
            }
            k=GetCharPressed();
        }
        if(IsKeyPressed(KEY_BACKSPACE)){if(findFocused&&findLen>0){findBuf[--findLen]='\0';RunFind();}else if(!findFocused&&repLen>0)repBuf[--repLen]='\0';}
        if(IsKeyPressed(KEY_TAB))findFocused=!findFocused;
        if(IsKeyPressed(KEY_ENTER)){if(findFocused)FindNext();else ReplaceOne();}
        if(IsKeyPressed(KEY_F3))FindNext();
        if(IsKeyPressed(KEY_ESCAPE)){findOpen=false;replaceOpen=false;findMatches.clear();findNoMatch=false;findIdx=-1;memset(findBuf,0,128);findLen=0;memset(repBuf,0,128);repLen=0;}
        return;
    }

    // Navigation with shift-select
    auto nav=[&](bool pressed,auto fn){
        if(!pressed)return;
        if(shift)StartSel();else ClearSel();
        fn();ClampCursor();
    };
    nav(IsKeyPressed(KEY_UP)||IsKeyPressedRepeat(KEY_UP),        [&]{cursorRow--;});
    nav(IsKeyPressed(KEY_DOWN)||IsKeyPressedRepeat(KEY_DOWN),    [&]{cursorRow++;});
    nav(IsKeyPressed(KEY_LEFT)||IsKeyPressedRepeat(KEY_LEFT),    [&]{if(cursorCol>0)cursorCol--;else if(cursorRow>0){cursorRow--;cursorCol=(int)lines[cursorRow].size();}});
    nav(IsKeyPressed(KEY_RIGHT)||IsKeyPressedRepeat(KEY_RIGHT),  [&]{if(cursorCol<(int)lines[cursorRow].size())cursorCol++;else if(cursorRow<(int)lines.size()-1){cursorRow++;cursorCol=0;}});
    nav(IsKeyPressed(KEY_HOME)||IsKeyPressedRepeat(KEY_HOME),    [&]{cursorCol=0;});
    nav(IsKeyPressed(KEY_END)||IsKeyPressedRepeat(KEY_END),      [&]{cursorCol=(int)lines[cursorRow].size();});
    nav(IsKeyPressed(KEY_PAGE_UP),  [&]{cursorRow-=12;});
    nav(IsKeyPressed(KEY_PAGE_DOWN),[&]{cursorRow+=12;});

    // Mouse
    if(IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
        Vector2 mp=GetMousePosition();
        int gW=showLineNums?GUTTER_W:0;
        int fbH=findOpen?(replaceOpen?FINDBAR_H*2:FINDBAR_H):0;
        int cY=MENUBAR_H+fbH;
        int sbH2=showStatusBar?STATUSBAR_H:0;
        if(mp.x>gW&&mp.y>cY&&mp.y<GetScreenHeight()-sbH2){
            ClearSel();
            int cr=scrollRow+(int)((mp.y-cY)/ED_LINE_H);
            cr=std::max(0,std::min(cr,(int)lines.size()-1));cursorRow=cr;
            std::string&ln=lines[cr];int best=0;
            for(int c=0;c<=(int)ln.size();c++){if(gW+PADDING+MT(ln.substr(0,c).c_str(),ED_FONT)<=(int)mp.x)best=c;else break;}
            cursorCol=best;
        }
    }

    float wh=GetMouseWheelMove();if(wh!=0.0f)scrollRow-=(int)wh*3;

    if(IsKeyPressed(KEY_ENTER)||IsKeyPressedRepeat(KEY_ENTER))   InsertNewline();
    if(IsKeyPressed(KEY_BACKSPACE)||IsKeyPressedRepeat(KEY_BACKSPACE))DelBack();
    if(IsKeyPressed(KEY_DELETE)||IsKeyPressedRepeat(KEY_DELETE)) DelFwd();
    if(IsKeyPressed(KEY_TAB)||IsKeyPressedRepeat(KEY_TAB)){ClearSel();for(int i=0;i<4;i++)InsertChar(' ');}
    int k=GetCharPressed();while(k>0){if(k>=32&&k<127)InsertChar((char)k);k=GetCharPressed();}
    ClampCursor();
}

// ============================================================
//  MAIN
// ============================================================
int main(){
    if(!RequestResources(APP_NAME,RAM_MB,HDD_MB,PRIORITY_NORMAL,1)){
        InitWindow(440,120,"Notepad - Denied");SetTargetFPS(30);
        double t=GetTime();
        while(!WindowShouldClose()&&GetTime()-t<3.5){BeginDrawing();ClearBackground(BG_DEEP);DrawText("Insufficient resources.",18,40,FONT_NORMAL,NEON_PINK);EndDrawing();}
        CloseWindow();return 1;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(960,680,"NexOS Notepad");
    SetTargetFPS(60);SetExitKey(KEY_NULL);SetWindowFocused();

    edFontOK=false;
    // Removed font loading to avoid asset dependencies
    // if(FileExists("assets/fonts/JetBrainsMono-Regular.ttf")){
    //     edFont=LoadFontEx("assets/fonts/JetBrainsMono-Regular.ttf",32,nullptr,0);
    //     edFontOK=(edFont.texture.id>0);
    //     if(edFontOK)SetTextureFilter(edFont.texture,TEXTURE_FILTER_BILINEAR);
    // }

    mkdir("hdd",0755);
    NewDocument();

    pthread_t asSave;pthread_create(&asSave,nullptr,AutoSaveThread,nullptr);

    while(!WindowShouldClose()&&appRunning){
        int sw=GetScreenWidth(),sh=GetScreenHeight();
        if(autoSaveSignal){autoSaveSignal=false;SetStatus("Auto-saved.",NEON_CYAN);}
        if(IsMouseButtonPressed(MOUSE_LEFT_BUTTON)&&(showFileMenu||showEditMenu||showViewMenu)&&GetMousePosition().y>MENUBAR_H)CloseMenus();
        HandleKeys();
        int fbH=findOpen?(replaceOpen?FINDBAR_H*2:FINDBAR_H):0;
        int sbH2=showStatusBar?STATUSBAR_H:0;
        int cY=MENUBAR_H+fbH,cH=sh-MENUBAR_H-sbH2-fbH;

        BeginDrawing();ClearBackground(ED_BG);
        DrawEditor(sw,cY,cH);
        DrawMenuBar(sw);
        if(findOpen)DrawFindBar(sw,MENUBAR_H);
        if(showStatusBar)DrawStatusBar(sw,sh);
        DrawFileBrowser(sw,sh);
        DrawSaveAsDialog(sw,sh);
        DrawUnsavedPrompt(sw,sh);
        EndDrawing();
    }

    appRunning=false;pthread_join(asSave,nullptr);
    if(isDirty&&!isUntitled)WriteToDisk(currentFile);
    if(edFontOK)UnloadFont(edFont);
    ReleaseResources(APP_NAME,RAM_MB,HDD_MB);
    CloseWindow();return 0;
}