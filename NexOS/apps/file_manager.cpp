// file_manager.cpp — implement using apps/app_template.cpp as base
#include "../include/theme.h"
#include "../include/resources.h"
#include "../include/ipc.h"
#include <raylib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <fstream>
#include <sstream>

#define APP_NAME  "File Manager"
#define RAM_MB    60
#define HDD_MB    30

// ── Layout ────────────────────────────────────────────────
#define TOOLBAR_H   40
#define STATUSBAR_H 24
#define PANEL_LEFT_W 240
#define PREVIEW_H   160
#define ITEM_H      28

// ── Colours ───────────────────────────────────────────────
static const Color FM_BG       = {  8,  8, 18, 255};
static const Color FM_PANEL    = { 14, 14, 30, 255};
static const Color FM_TOOLBAR  = { 10, 10, 22, 255};
static const Color FM_SEL      = {  0,120,200,  80};
static const Color FM_HOVER    = { 30, 30, 60, 200};
static const Color FM_GRID     = { 30, 30, 60, 255};

// ── Font helpers ──────────────────────────────────────────
static Font fmFont; static bool fmFontOK=false;
static void DT(const char* t,int x,int y,int sz,Color c){
    if(fmFontOK)DrawTextEx(fmFont,t,{(float)x,(float)y},(float)sz,1.0f,c);
    else DrawText(t,x,y,sz,c);}
static int MT(const char* t,int sz){
    if(fmFontOK)return(int)MeasureTextEx(fmFont,t,(float)sz,1.0f).x;
    return MeasureText(t,sz);}

// ============================================================
//  File entry
// ============================================================
struct FileEntry {
    std::string name;
    std::string fullPath;
    bool        isDir;
    long        size;       // bytes
    time_t      modified;
};

// ============================================================
//  State
// ============================================================
static std::string        currentDir   = "hdd";
static std::vector<FileEntry> entries;
static int                selectedIdx  = -1;
static int                scrollOffset = 0;

// ── Clipboard ─────────────────────────────────────────────
static std::string        clipPath     = "";
static bool               clipIsCopy   = true; // true=copy false=cut

// ── Dialogs ───────────────────────────────────────────────
enum DialogMode { DLG_NONE, DLG_RENAME, DLG_NEW_FILE, DLG_NEW_FOLDER,
                  DLG_DELETE_CONFIRM, DLG_FILE_INFO, DLG_COPY_DEST,
                  DLG_MOVE_DEST };
static DialogMode dlgMode  = DLG_NONE;
static char       dlgBuf[256] = "";
static int        dlgLen   = 0;

// ── Progress ──────────────────────────────────────────────
static bool       showProgress  = false;
static float      progressVal   = 0.0f;
static std::string progressMsg  = "";

// ── Preview ───────────────────────────────────────────────
static std::string previewContent = "";
static std::string previewName    = "";
static bool        previewOpen    = true;

// ── Status ────────────────────────────────────────────────
static char        statusMsg[256] = "Welcome to NexOS File Manager";
static double      statusAt       = 0.0;
static void SetStatus(const char* m){strncpy(statusMsg,m,255);statusAt=GetTime();}

// ── App running ───────────────────────────────────────────
static bool appRunning = true;

// ============================================================
//  Utilities
// ============================================================
static std::string FormatSize(long bytes){
    char buf[32];
    if(bytes<1024)snprintf(buf,sizeof(buf),"%ldB",bytes);
    else if(bytes<1024*1024)snprintf(buf,sizeof(buf),"%.1fKB",(float)bytes/1024);
    else snprintf(buf,sizeof(buf),"%.1fMB",(float)bytes/(1024*1024));
    return std::string(buf);
}

static std::string FormatTime(time_t t){
    char buf[32];
    strftime(buf,sizeof(buf),"%Y-%m-%d %H:%M",localtime(&t));
    return std::string(buf);
}

static std::string GetExt(const std::string& name){
    auto dot=name.rfind('.');
    if(dot==std::string::npos)return "";
    std::string ext=name.substr(dot+1);
    for(auto&c:ext)c=tolower(c);
    return ext;
}

static bool IsTextFile(const std::string& name){
    std::string e=GetExt(name);
    return e=="txt"||e=="cpp"||e=="h"||e=="c"||e=="py"||e=="md"||e=="log"||e=="csv"||e=="json"||e=="xml"||e=="";}

static Color GetFileColor(const FileEntry& f){
    if(f.isDir) return NEON_CYAN;
    std::string e=GetExt(f.name);
    if(e=="txt"||e=="md") return TEXT_PRIMARY;
    if(e=="cpp"||e=="h"||e=="c") return NEON_GREEN;
    if(e=="py") return NEON_GOLD;
    if(e=="log") return TEXT_MUTED;
    if(e=="png"||e=="jpg"||e=="bmp") return NEON_PURPLE;
    return TEXT_PRIMARY;
}

static const char* GetFileIcon(const FileEntry& f){
    if(f.isDir) return "[D]";
    std::string e=GetExt(f.name);
    if(e=="txt"||e=="md") return "[T]";
    if(e=="cpp"||e=="h"||e=="c") return "[C]";
    if(e=="py") return "[P]";
    if(e=="log") return "[L]";
    if(e=="png"||e=="jpg") return "[I]";
    return "[F]";
}

// ============================================================
//  Directory reading
// ============================================================
static void RefreshDirectory(){
    entries.clear();
    selectedIdx=-1;
    scrollOffset=0;

    // Always add parent dir entry unless at hdd root
    if(currentDir!="hdd"&&currentDir!="hdd/"){
        FileEntry up;
        up.name="..";
        up.isDir=true;
        up.size=0;
        up.modified=0;
        // parent path
        auto slash=currentDir.rfind('/');
        up.fullPath=(slash!=std::string::npos)?currentDir.substr(0,slash):"hdd";
        entries.push_back(up);
    }

    DIR* d=opendir(currentDir.c_str());
    if(!d){SetStatus("Cannot open directory.");return;}

    struct dirent* e;
    while((e=readdir(d))){
        std::string n(e->d_name);
        if(n=="."||n=="..")continue;
        FileEntry fe;
        fe.name=n;
        fe.fullPath=currentDir+"/"+n;
        struct stat st;
        if(stat(fe.fullPath.c_str(),&st)==0){
            fe.isDir=S_ISDIR(st.st_mode);
            fe.size=st.st_size;
            fe.modified=st.st_mtime;
        } else {
            fe.isDir=false; fe.size=0; fe.modified=0;
        }
        entries.push_back(fe);
    }
    closedir(d);

    // Sort: dirs first, then files, both alphabetical
    std::sort(entries.begin(),entries.end(),[](const FileEntry&a,const FileEntry&b){
        if(a.name=="..")return true;
        if(b.name=="..")return false;
        if(a.isDir!=b.isDir)return a.isDir>b.isDir;
        return a.name<b.name;
    });

    char msg[128];
    int files=0,dirs=0;
    for(auto&fe:entries)if(fe.name!="..")(fe.isDir?dirs:files)++;
    snprintf(msg,sizeof(msg),"%d folder(s), %d file(s) in %s",dirs,files,currentDir.c_str());
    SetStatus(msg);
}

// ============================================================
//  Text preview
// ============================================================
static void LoadPreview(const FileEntry& f){
    previewName=f.name;
    previewContent="";
    if(f.isDir){previewContent="[Directory]";return;}
    if(!IsTextFile(f.name)){
        previewContent="[Binary file — no preview]";return;}
    std::ifstream fs(f.fullPath);
    if(!fs.is_open()){previewContent="[Cannot read file]";return;}
    std::string line;
    int lineCount=0;
    while(std::getline(fs,line)&&lineCount<30){
        // Truncate long lines
        if((int)line.size()>100)line=line.substr(0,97)+"...";
        previewContent+=line+"\n";
        lineCount++;
    }
    if(lineCount==30)previewContent+="... (truncated)";
    fs.close();
}

// ============================================================
//  File operations
// ============================================================
static bool CopyFile_(const std::string& src,const std::string& dst){
    std::ifstream in(src,std::ios::binary);
    if(!in.is_open())return false;
    std::ofstream out(dst,std::ios::binary);
    if(!out.is_open())return false;
    out<<in.rdbuf();
    return true;
}

static void DeleteEntry(const FileEntry& f){
    if(f.isDir){
        // Recursively delete (simple: only one level deep for safety)
        DIR* d=opendir(f.fullPath.c_str());
        if(d){
            struct dirent* e;
            while((e=readdir(d))){
                std::string n(e->d_name);
                if(n=="."||n=="..")continue;
                std::string child=f.fullPath+"/"+n;
                remove(child.c_str());
            }
            closedir(d);
        }
        rmdir(f.fullPath.c_str());
    } else {
        remove(f.fullPath.c_str());
    }
}

static void DoRename(const std::string& oldPath,const std::string& newName){
    auto slash=oldPath.rfind('/');
    std::string dir=(slash!=std::string::npos)?oldPath.substr(0,slash):"hdd";
    std::string newPath=dir+"/"+newName;
    rename(oldPath.c_str(),newPath.c_str());
}

static void DoPaste(const std::string& destDir){
    if(clipPath.empty())return;
    auto slash=clipPath.rfind('/');
    std::string fname=(slash!=std::string::npos)?clipPath.substr(slash+1):clipPath;
    std::string destPath=destDir+"/"+fname;

    // Avoid overwriting
    if(destPath==clipPath){SetStatus("Source and destination are the same.");return;}

    struct stat st;
    bool isDir=(stat(clipPath.c_str(),&st)==0&&S_ISDIR(st.st_mode));

    if(isDir){
        // Simple directory copy (one level)
        mkdir(destPath.c_str(),0755);
        DIR* d=opendir(clipPath.c_str());
        if(d){
            struct dirent* e;
            while((e=readdir(d))){
                std::string n(e->d_name);
                if(n=="."||n=="..")continue;
                CopyFile_(clipPath+"/"+n,destPath+"/"+n);
            }
            closedir(d);
        }
        if(!clipIsCopy){
            // Remove source after move
            DIR* d2=opendir(clipPath.c_str());
            if(d2){struct dirent* e2;while((e2=readdir(d2))){std::string n(e2->d_name);if(n!="."&&n!="..")remove((clipPath+"/"+n).c_str());}closedir(d2);}
            rmdir(clipPath.c_str());
        }
    } else {
        if(!CopyFile_(clipPath,destPath)){SetStatus("Copy failed.");return;}
        if(!clipIsCopy)remove(clipPath.c_str());
    }

    clipPath="";
    SetStatus(clipIsCopy?"Copied successfully.":"Moved successfully.");
    RefreshDirectory();
}

// ============================================================
//  DRAW: TOOLBAR
// ============================================================
static void DrawToolbar(int sw){
    DrawRectangle(0,0,sw,TOOLBAR_H,FM_TOOLBAR);
    DrawLine(0,TOOLBAR_H,sw,TOOLBAR_H,BORDER_DIM);

    // Path breadcrumb
    DT(currentDir.c_str(),10,12,FONT_SMALL,NEON_CYAN);

    // Toolbar buttons
    struct{const char* lbl;Color c;}btns[]={
        {"New File",NEON_CYAN},{"New Folder",NEON_CYAN},
        {"Copy",NEON_GOLD},{"Cut",NEON_ORANGE},
        {"Paste",NEON_GREEN},{"Rename",NEON_PURPLE},
        {"Delete",NEON_PINK},{"Info",TEXT_MUTED},
        {"Preview",TEXT_MUTED},
    };
    int nbtn=9;
    int bx=sw-nbtn*80-10;
    for(int i=0;i<nbtn;i++){
        Rectangle r={(float)(bx+i*80),(float)6,(float)74,28};
        bool hov=CheckCollisionPointRec(GetMousePosition(),r);
        DrawRectangleRec(r,hov?BG_HOVER:FM_PANEL);
        DrawRectangleLinesEx(r,1.0f,hov?btns[i].c:BORDER_DIM);
        int tw=MT(btns[i].lbl,FONT_TINY);
        DT(btns[i].lbl,(int)(r.x+(r.width-tw)/2),(int)r.y+8,FONT_TINY,hov?btns[i].c:TEXT_MUTED);
        if(hov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
            if(i==0){ // New File
                dlgMode=DLG_NEW_FILE;memset(dlgBuf,0,256);dlgLen=0;
            } else if(i==1){ // New Folder
                dlgMode=DLG_NEW_FOLDER;memset(dlgBuf,0,256);dlgLen=0;
            } else if(i==2){ // Copy
                if(selectedIdx>=0&&selectedIdx<(int)entries.size()&&entries[selectedIdx].name!=".."){
                    clipPath=entries[selectedIdx].fullPath;clipIsCopy=true;
                    SetStatus(("Copied to clipboard: "+entries[selectedIdx].name).c_str());
                }
            } else if(i==3){ // Cut
                if(selectedIdx>=0&&selectedIdx<(int)entries.size()&&entries[selectedIdx].name!=".."){
                    clipPath=entries[selectedIdx].fullPath;clipIsCopy=false;
                    SetStatus(("Cut: "+entries[selectedIdx].name).c_str());
                }
            } else if(i==4){ // Paste
                DoPaste(currentDir);
            } else if(i==5){ // Rename
                if(selectedIdx>=0&&selectedIdx<(int)entries.size()&&entries[selectedIdx].name!=".."){
                    dlgMode=DLG_RENAME;
                    strncpy(dlgBuf,entries[selectedIdx].name.c_str(),255);
                    dlgLen=(int)strlen(dlgBuf);
                }
            } else if(i==6){ // Delete
                if(selectedIdx>=0&&selectedIdx<(int)entries.size()&&entries[selectedIdx].name!=".."){
                    dlgMode=DLG_DELETE_CONFIRM;
                }
            } else if(i==7){ // Info
                if(selectedIdx>=0&&selectedIdx<(int)entries.size()&&entries[selectedIdx].name!=".."){
                    dlgMode=DLG_FILE_INFO;
                }
            } else if(i==8){ // Toggle preview
                previewOpen=!previewOpen;
            }
        }
    }
}

// ============================================================
//  DRAW: LEFT PANEL (folder tree — simplified, shows hdd subdirs)
// ============================================================
static void DrawLeftPanel(Rectangle panel){
    DrawRectangle((int)panel.x,(int)panel.y,(int)panel.width,(int)panel.height,FM_PANEL);
    DrawLine((int)(panel.x+panel.width),(int)panel.y,(int)(panel.x+panel.width),(int)(panel.y+panel.height),BORDER_DIM);

    DT("Folders",(int)panel.x+10,(int)panel.y+8,FONT_TINY,TEXT_MUTED);
    DrawLine((int)panel.x,(int)(panel.y+24),(int)(panel.x+panel.width),(int)(panel.y+24),BORDER_DIM);

    // Always show hdd root
    int ry=(int)panel.y+28;

    // Root entry
    bool rootSel=(currentDir=="hdd"||currentDir=="hdd/");
    if(rootSel)DrawRectangle((int)panel.x,ry,(int)panel.width,ITEM_H,FM_SEL);
    DT("[D] hdd/",(int)panel.x+8,ry+6,FONT_SMALL,rootSel?NEON_CYAN:TEXT_PRIMARY);
    Rectangle rootR={(float)panel.x,(float)ry,(float)panel.width,ITEM_H};
    if(CheckCollisionPointRec(GetMousePosition(),rootR)&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
        currentDir="hdd";RefreshDirectory();
    }
    ry+=ITEM_H;

    // Subdirectories of hdd
    DIR* d=opendir("hdd");
    if(d){
        std::vector<std::string> subdirs;
        struct dirent* e;
        while((e=readdir(d))){
            std::string n(e->d_name);
            if(n=="."||n=="..")continue;
            struct stat st;
            std::string fp="hdd/"+n;
            if(stat(fp.c_str(),&st)==0&&S_ISDIR(st.st_mode))subdirs.push_back(n);
        }
        closedir(d);
        std::sort(subdirs.begin(),subdirs.end());
        for(auto&sd:subdirs){
            std::string fp="hdd/"+sd;
            bool sel=(currentDir==fp);
            if(sel)DrawRectangle((int)panel.x,ry,(int)panel.width,ITEM_H,FM_SEL);
            bool hov=CheckCollisionPointRec(GetMousePosition(),{panel.x,(float)ry,panel.width,ITEM_H});
            if(hov&&!sel)DrawRectangle((int)panel.x,ry,(int)panel.width,ITEM_H,FM_HOVER);
            DT(("  [D] "+sd).c_str(),(int)panel.x+8,ry+6,FONT_SMALL,sel?NEON_CYAN:(hov?TEXT_PRIMARY:TEXT_MUTED));
            if(hov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){currentDir=fp;RefreshDirectory();}
            ry+=ITEM_H;
        }
    }
}

// ============================================================
//  DRAW: RIGHT PANEL (file list)
// ============================================================
static void DrawRightPanel(Rectangle panel){
    DrawRectangle((int)panel.x,(int)panel.y,(int)panel.width,(int)panel.height,FM_BG);

    // Column headers
    int hY=(int)panel.y;
    DrawRectangle((int)panel.x,hY,(int)panel.width,24,FM_TOOLBAR);
    DrawLine((int)panel.x,hY+24,(int)(panel.x+panel.width),hY+24,BORDER_DIM);
    DT("Name",(int)panel.x+32,hY+6,FONT_TINY,TEXT_MUTED);
    DT("Size",(int)(panel.x+panel.width-230),hY+6,FONT_TINY,TEXT_MUTED);
    DT("Modified",(int)(panel.x+panel.width-160),hY+6,FONT_TINY,TEXT_MUTED);
    DT("Type",(int)(panel.x+panel.width-60),hY+6,FONT_TINY,TEXT_MUTED);

    int contentY=hY+24;
    int contentH=(int)panel.height-24;
    int visItems=contentH/ITEM_H;

    // Clamp scroll
    int maxScroll=std::max(0,(int)entries.size()-visItems);
    scrollOffset=std::max(0,std::min(scrollOffset,maxScroll));

    // Scissor / clip drawing
    BeginScissorMode((int)panel.x,contentY,(int)panel.width,contentH);

    Vector2 mouse=GetMousePosition();
    for(int i=scrollOffset;i<(int)entries.size()&&i<scrollOffset+visItems+1;i++){
        auto& f=entries[i];
        int ry=contentY+(i-scrollOffset)*ITEM_H;
        Rectangle row={(float)panel.x,(float)ry,(float)panel.width,(float)ITEM_H};

        bool hov=CheckCollisionPointRec(mouse,row);
        bool sel=(i==selectedIdx);

        if(sel)      DrawRectangleRec(row,FM_SEL);
        else if(hov) DrawRectangleRec(row,FM_HOVER);

        // Alternating row tint
        if(!sel&&!hov&&i%2==0)
            DrawRectangle((int)panel.x,ry,(int)panel.width,ITEM_H,{0,0,0,20});

        // Separator
        DrawLine((int)panel.x,ry+ITEM_H-1,(int)(panel.x+panel.width),ry+ITEM_H-1,{30,30,50,180});

        // Icon
        Color fc=GetFileColor(f);
        DT(GetFileIcon(f),(int)panel.x+6,ry+7,FONT_TINY,fc);

        // Name (truncate if too long)
        std::string displayName=f.name;
        while((int)displayName.size()>36&&MT(displayName.c_str(),FONT_SMALL)>300)
            displayName=displayName.substr(0,displayName.size()-1);
        if(displayName!=f.name)displayName+="...";
        DT(displayName.c_str(),(int)panel.x+30,ry+7,FONT_SMALL,sel?NEON_CYAN:(hov?TEXT_PRIMARY:fc));

        // Size
        if(!f.isDir){
            std::string sz=FormatSize(f.size);
            DT(sz.c_str(),(int)(panel.x+panel.width-230),ry+7,FONT_TINY,TEXT_MUTED);
        }

        // Modified
        if(f.modified>0){
            std::string mt=FormatTime(f.modified);
            DT(mt.c_str(),(int)(panel.x+panel.width-160),ry+7,FONT_TINY,TEXT_MUTED);
        }

        // Type
        std::string typeStr=f.isDir?"Folder":GetExt(f.name).empty()?"File":GetExt(f.name);
        DT(typeStr.c_str(),(int)(panel.x+panel.width-60),ry+7,FONT_TINY,TEXT_DIM);

        // Click handling
        if(hov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
            selectedIdx=i;
            if(!f.isDir&&IsTextFile(f.name)) LoadPreview(f);
            else if(f.isDir&&f.name!="..") previewContent="[Directory]";
        }

        // Double click: enter dir or open file
        static double lastClick=0; static int lastClickIdx=-1;
        if(hov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
            double now=GetTime();
            if(i==lastClickIdx&&now-lastClick<0.4){
                if(f.isDir){
                    currentDir=f.fullPath;
                    RefreshDirectory();
                }
            }
            lastClick=now; lastClickIdx=i;
        }
    }

    EndScissorMode();

    // Scrollbar
    if((int)entries.size()>visItems){
        float frac=(float)scrollOffset/std::max(1,maxScroll);
        int sbH=std::max(20,(int)((float)visItems/(int)entries.size()*contentH));
        int sbY=contentY+(int)(frac*(contentH-sbH));
        DrawRectangle((int)(panel.x+panel.width-4),contentY,4,contentH,{20,20,40,200});
        DrawRectangle((int)(panel.x+panel.width-4),sbY,4,sbH,{0,255,200,120});
    }

    // Scroll wheel
    if(CheckCollisionPointRec(mouse,panel)){
        float wh=GetMouseWheelMove();
        if(wh!=0)scrollOffset=std::max(0,std::min(maxScroll,scrollOffset-(int)wh*3));
    }
}

// ============================================================
//  DRAW: PREVIEW PANEL
// ============================================================
static void DrawPreviewPanel(Rectangle panel){
    DrawRectangle((int)panel.x,(int)panel.y,(int)panel.width,(int)panel.height,FM_PANEL);
    DrawLine((int)panel.x,(int)panel.y,(int)(panel.x+panel.width),(int)panel.y,BORDER_DIM);

    // Header
    char hdr[128];
    if(previewName.empty())snprintf(hdr,sizeof(hdr),"Preview");
    else snprintf(hdr,sizeof(hdr),"Preview: %s",previewName.c_str());
    DT(hdr,(int)panel.x+10,(int)panel.y+6,FONT_TINY,NEON_CYAN);
    DrawLine((int)panel.x,(int)(panel.y+22),(int)(panel.x+panel.width),(int)(panel.y+22),BORDER_DIM);

    if(previewContent.empty()){
        DT("Select a text file to preview it here.",
           (int)panel.x+14,(int)panel.y+30,FONT_SMALL,TEXT_DIM);
        return;
    }

    // Draw preview text line by line
    std::istringstream ss(previewContent);
    std::string line;
    int py=(int)panel.y+28;
    int maxY=(int)(panel.y+panel.height-6);
    while(std::getline(ss,line)&&py<maxY){
        if((int)line.size()>120)line=line.substr(0,117)+"...";
        DT(line.c_str(),(int)panel.x+10,py,FONT_TINY,TEXT_PRIMARY);
        py+=14;
    }
}

// ============================================================
//  DRAW: DIALOGS
// ============================================================
static void DrawDialogs(int sw,int sh){
    if(dlgMode==DLG_NONE)return;

    // Overlay
    DrawRectangle(0,0,sw,sh,{0,0,0,200});

    auto drawDlgBase=[&](int pw,int ph,const char* title)->std::pair<int,int>{
        int px=(sw-pw)/2,py=(sh-ph)/2;
        DrawRectangle(px,py,pw,ph,BG_PANEL);
        DrawGlowRect({(float)px,(float)py,(float)pw,(float)ph},NEON_CYAN,5);
        DrawRectangle(px,py,pw,36,BG_TITLEBAR);
        DT(title,px+14,py+10,FONT_LARGE,NEON_CYAN);
        DrawLine(px,py+36,px+pw,py+36,BORDER_DIM);
        return {px,py};
    };

    auto drawInputBox=[&](int px,int py,int pw,int boxY){
        Rectangle box={(float)(px+16),(float)(py+boxY),(float)(pw-32),38};
        DrawRectangleRec(box,BG_DEEP);
        DrawRectangleLinesEx(box,2.0f,NEON_CYAN);
        DrawGlowRect(box,NEON_CYAN,3);
        DT(dlgBuf,(int)box.x+10,(int)box.y+10,FONT_NORMAL,TEXT_PRIMARY);
        if((int)(GetTime()*2)%2==0){
            int cw=MT(dlgBuf,FONT_NORMAL);
            DT("|",(int)box.x+12+cw,(int)box.y+10,FONT_NORMAL,NEON_CYAN);
        }
    };

    auto handleInput=[&](){
        int k=GetCharPressed();
        while(k>0){
            bool ok=(k>='a'&&k<='z')||(k>='A'&&k<='Z')||(k>='0'&&k<='9')||
                     k=='_'||k=='-'||k=='.'||k==' ';
            if(ok&&dlgLen<254){dlgBuf[dlgLen++]=(char)k;dlgBuf[dlgLen]='\0';}
            k=GetCharPressed();
        }
        if(IsKeyPressed(KEY_BACKSPACE)&&dlgLen>0)dlgBuf[--dlgLen]='\0';
        if(IsKeyPressed(KEY_ESCAPE))dlgMode=DLG_NONE;
    };

    // ── RENAME ────────────────────────────────────────────
    if(dlgMode==DLG_RENAME){
        auto[px,py]=drawDlgBase(480,180,"Rename");
        DT("New name:",(int)(px+16),(int)(py+48),FONT_SMALL,TEXT_MUTED);
        drawInputBox(px,py,480,72);
        DT("Press Enter to confirm",(int)(px+16),(int)(py+122),FONT_TINY,TEXT_DIM);
        handleInput();
        if(IsKeyPressed(KEY_ENTER)&&dlgLen>0&&selectedIdx>=0){
            DoRename(entries[selectedIdx].fullPath,std::string(dlgBuf));
            dlgMode=DLG_NONE;
            RefreshDirectory();
            SetStatus("Renamed successfully.");
        }
        if(DrawButton({(float)(px+480-116),(float)(py+138),106,30},"Cancel",BG_HOVER,NEON_PINK,FONT_SMALL))dlgMode=DLG_NONE;
    }

    // ── NEW FILE ──────────────────────────────────────────
    else if(dlgMode==DLG_NEW_FILE){
        auto[px,py]=drawDlgBase(480,190,"New File");
        DT("File name (e.g. notes.txt):",(int)(px+16),(int)(py+48),FONT_SMALL,TEXT_MUTED);
        drawInputBox(px,py,480,72);
        DT("File will be created in current directory",(int)(px+16),(int)(py+124),FONT_TINY,TEXT_DIM);
        handleInput();
        if(IsKeyPressed(KEY_ENTER)&&dlgLen>0){
            std::string fp=currentDir+"/"+std::string(dlgBuf);
            std::ofstream f(fp);
            if(f.is_open()){f.close();SetStatus("File created.");}
            else SetStatus("Failed to create file.");
            dlgMode=DLG_NONE;RefreshDirectory();
        }
        if(DrawButton({(float)(px+480-116),(float)(py+148),106,30},"Cancel",BG_HOVER,NEON_PINK,FONT_SMALL))dlgMode=DLG_NONE;
    }

    // ── NEW FOLDER ────────────────────────────────────────
    else if(dlgMode==DLG_NEW_FOLDER){
        auto[px,py]=drawDlgBase(480,190,"New Folder");
        DT("Folder name:",(int)(px+16),(int)(py+48),FONT_SMALL,TEXT_MUTED);
        drawInputBox(px,py,480,72);
        handleInput();
        if(IsKeyPressed(KEY_ENTER)&&dlgLen>0){
            std::string fp=currentDir+"/"+std::string(dlgBuf);
            if(mkdir(fp.c_str(),0755)==0)SetStatus("Folder created.");
            else SetStatus("Failed to create folder.");
            dlgMode=DLG_NONE;RefreshDirectory();
        }
        if(DrawButton({(float)(px+480-116),(float)(py+148),106,30},"Cancel",BG_HOVER,NEON_PINK,FONT_SMALL))dlgMode=DLG_NONE;
    }

    // ── DELETE CONFIRM ────────────────────────────────────
    else if(dlgMode==DLG_DELETE_CONFIRM){
        auto[px,py]=drawDlgBase(480,200,"Delete");
        if(selectedIdx>=0&&selectedIdx<(int)entries.size()){
            char msg2[128];
            snprintf(msg2,sizeof(msg2),"Delete  \"%s\" ?",entries[selectedIdx].name.c_str());
            DT(msg2,px+16,py+52,FONT_NORMAL,TEXT_PRIMARY);
            DT("This action cannot be undone.",px+16,py+80,FONT_SMALL,NEON_PINK);
        }
        if(DrawButton({(float)(px+16),(float)(py+130),140,36},"Delete",BG_HOVER,NEON_PINK,FONT_NORMAL)){
            if(selectedIdx>=0&&selectedIdx<(int)entries.size()){
                DeleteEntry(entries[selectedIdx]);
                SetStatus("Deleted.");
                previewContent=""; previewName="";
            }
            dlgMode=DLG_NONE;RefreshDirectory();
        }
        if(DrawButton({(float)(px+480-116),(float)(py+130),106,36},"Cancel",BG_HOVER,NEON_CYAN,FONT_NORMAL))dlgMode=DLG_NONE;
        if(IsKeyPressed(KEY_ESCAPE))dlgMode=DLG_NONE;
    }

    // ── FILE INFO ─────────────────────────────────────────
    else if(dlgMode==DLG_FILE_INFO){
        auto[px,py]=drawDlgBase(500,280,"File Info");
        if(selectedIdx>=0&&selectedIdx<(int)entries.size()){
            auto& f=entries[selectedIdx];
            struct stat st;
            stat(f.fullPath.c_str(),&st);

            auto row=[&](int ry,const char* lbl,const char* val){
                DT(lbl,px+16,py+ry,FONT_SMALL,TEXT_MUTED);
                DT(val,px+140,py+ry,FONT_SMALL,TEXT_PRIMARY);
            };
            row(50,"Name:",f.name.c_str());
            row(76,"Path:",f.fullPath.c_str());
            row(102,"Type:",f.isDir?"Directory":(GetExt(f.name).empty()?"File":GetExt(f.name).c_str()));
            char szBuf[32]; snprintf(szBuf,sizeof(szBuf),"%s (%ld bytes)",FormatSize(f.size).c_str(),f.size);
            row(128,"Size:",f.isDir?"—":szBuf);
            row(154,"Modified:",FormatTime(f.modified).c_str());

            // Permissions
            char permBuf[12];
            snprintf(permBuf,sizeof(permBuf),"%s%s%s",
                (st.st_mode&S_IRUSR)?"r":"-",
                (st.st_mode&S_IWUSR)?"w":"-",
                (st.st_mode&S_IXUSR)?"x":"-");
            row(180,"Permissions:",permBuf);
        }
        if(DrawButton({(float)(px+500-116),(float)(py+230),106,34},"Close",BG_HOVER,NEON_CYAN,FONT_NORMAL))dlgMode=DLG_NONE;
        if(IsKeyPressed(KEY_ESCAPE))dlgMode=DLG_NONE;
    }
}

// ============================================================
//  DRAW: STATUS BAR
// ============================================================
static void DrawStatusBar(int sw,int sh){
    int y=sh-STATUSBAR_H;
    DrawRectangle(0,y,sw,STATUSBAR_H,FM_TOOLBAR);
    DrawLine(0,y,sw,y,BORDER_DIM);

    // Status message
    double age=GetTime()-statusAt;
    unsigned char a=(age>3.0)?(unsigned char)std::max(0.0,(4.0-age)*255):255;
    DT(statusMsg,10,y+5,FONT_TINY,{0,255,200,a});

    // Selection info
    if(selectedIdx>=0&&selectedIdx<(int)entries.size()&&entries[selectedIdx].name!=".."){
        auto& f=entries[selectedIdx];
        char info[64];
        if(f.isDir)snprintf(info,sizeof(info),"[Folder] %s",f.name.c_str());
        else snprintf(info,sizeof(info),"[File] %s  %s",f.name.c_str(),FormatSize(f.size).c_str());
        int iw=MT(info,FONT_TINY);
        DT(info,sw-iw-12,y+5,FONT_TINY,TEXT_MUTED);
    }

    // Clipboard indicator
    if(!clipPath.empty()){
        auto slash=clipPath.rfind('/');
        std::string cn=(slash!=std::string::npos)?clipPath.substr(slash+1):clipPath;
        char cb[64]; snprintf(cb,sizeof(cb),"%s: %s",clipIsCopy?"Copied":"Cut",cn.c_str());
        int cw=MT(cb,FONT_TINY);
        DT(cb,sw/2-cw/2,y+5,FONT_TINY,clipIsCopy?NEON_GOLD:NEON_ORANGE);
    }
}

// ============================================================
//  KEYBOARD SHORTCUTS
// ============================================================
static void HandleKeys(){
    if(dlgMode!=DLG_NONE)return;
    bool ctrl=IsKeyDown(KEY_LEFT_CONTROL)||IsKeyDown(KEY_RIGHT_CONTROL);
    if(ctrl){
        if(IsKeyPressed(KEY_C)&&selectedIdx>=0&&selectedIdx<(int)entries.size()&&entries[selectedIdx].name!=".."){
            clipPath=entries[selectedIdx].fullPath;clipIsCopy=true;
            SetStatus(("Copied: "+entries[selectedIdx].name).c_str());
        }
        if(IsKeyPressed(KEY_X)&&selectedIdx>=0&&selectedIdx<(int)entries.size()&&entries[selectedIdx].name!=".."){
            clipPath=entries[selectedIdx].fullPath;clipIsCopy=false;
            SetStatus(("Cut: "+entries[selectedIdx].name).c_str());
        }
        if(IsKeyPressed(KEY_V)) DoPaste(currentDir);
        if(IsKeyPressed(KEY_N)){dlgMode=DLG_NEW_FILE;memset(dlgBuf,0,256);dlgLen=0;}
        if(IsKeyPressed(KEY_R)&&selectedIdx>=0&&selectedIdx<(int)entries.size()&&entries[selectedIdx].name!=".."){
            dlgMode=DLG_RENAME;
            strncpy(dlgBuf,entries[selectedIdx].name.c_str(),255);dlgLen=(int)strlen(dlgBuf);
        }
    }
    if(IsKeyPressed(KEY_DELETE)&&selectedIdx>=0&&selectedIdx<(int)entries.size()&&entries[selectedIdx].name!="..")
        dlgMode=DLG_DELETE_CONFIRM;
    if(IsKeyPressed(KEY_F5))RefreshDirectory();
    if(IsKeyPressed(KEY_BACKSPACE)){
        // Go up one directory
        if(currentDir!="hdd"&&currentDir!="hdd/"){
            auto slash=currentDir.rfind('/');
            currentDir=(slash!=std::string::npos)?currentDir.substr(0,slash):"hdd";
            RefreshDirectory();
        }
    }

    // Arrow key navigation
    if(!entries.empty()){
        if(IsKeyPressed(KEY_UP)||IsKeyPressedRepeat(KEY_UP)){
            selectedIdx=std::max(0,selectedIdx-1);
            if(selectedIdx<scrollOffset)scrollOffset=selectedIdx;
            if(selectedIdx>=0&&selectedIdx<(int)entries.size()&&!entries[selectedIdx].isDir)
                LoadPreview(entries[selectedIdx]);
        }
        if(IsKeyPressed(KEY_DOWN)||IsKeyPressedRepeat(KEY_DOWN)){
            selectedIdx=std::min((int)entries.size()-1,selectedIdx+1);
            if(selectedIdx<0)selectedIdx=0;
            if(selectedIdx>=0&&selectedIdx<(int)entries.size()&&!entries[selectedIdx].isDir)
                LoadPreview(entries[selectedIdx]);
        }
        if(IsKeyPressed(KEY_ENTER)&&selectedIdx>=0&&selectedIdx<(int)entries.size()){
            auto& f=entries[selectedIdx];
            if(f.isDir){currentDir=f.fullPath;RefreshDirectory();}
        }
    }
}

// ============================================================
//  MAIN
// ============================================================
int main(){
    if(!RequestResources(APP_NAME,RAM_MB,HDD_MB,PRIORITY_NORMAL,1)){
        InitWindow(440,120,"File Manager — Denied");SetTargetFPS(30);
        double t=GetTime();
        while(!WindowShouldClose()&&GetTime()-t<3.5){
            BeginDrawing();ClearBackground(BG_DEEP);
            DrawText("Insufficient resources.",18,40,FONT_NORMAL,NEON_PINK);EndDrawing();}
        CloseWindow();return 1;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(1100,700,"NexOS File Manager");
    SetTargetFPS(60);SetExitKey(KEY_NULL);
    SetWindowFocused();

    // Load font
    fmFontOK=false;
    if(FileExists("assets/fonts/JetBrainsMono-Regular.ttf")){
        fmFont=LoadFontEx("assets/fonts/JetBrainsMono-Regular.ttf",28,nullptr,0);
        fmFontOK=(fmFont.texture.id>0);
        if(fmFontOK)SetTextureFilter(fmFont.texture,TEXTURE_FILTER_BILINEAR);
    }

    // Ensure hdd exists
    mkdir("hdd",0755);
    RefreshDirectory();

    while(!WindowShouldClose()&&appRunning){
        int sw=GetScreenWidth(),sh=GetScreenHeight();

        HandleKeys();

        // Layout
        int previewH = previewOpen ? PREVIEW_H : 0;
        Rectangle toolbar={0,0,(float)sw,(float)TOOLBAR_H};
        Rectangle leftPanel={0,(float)TOOLBAR_H,(float)PANEL_LEFT_W,(float)(sh-TOOLBAR_H-STATUSBAR_H-previewH)};
        Rectangle rightPanel={(float)PANEL_LEFT_W,(float)TOOLBAR_H,(float)(sw-PANEL_LEFT_W),(float)(sh-TOOLBAR_H-STATUSBAR_H-previewH)};
        Rectangle previewPanel={0,(float)(sh-STATUSBAR_H-previewH),(float)sw,(float)previewH};

        BeginDrawing();
        ClearBackground(FM_BG);

        DrawRightPanel(rightPanel);
        DrawLeftPanel(leftPanel);
        DrawToolbar(sw);
        if(previewOpen)DrawPreviewPanel(previewPanel);
        DrawStatusBar(sw,sh);
        DrawDialogs(sw,sh);

        EndDrawing();
    }

    if(fmFontOK)UnloadFont(fmFont);
    ReleaseResources(APP_NAME,RAM_MB,HDD_MB);
    CloseWindow();
    return 0;
}