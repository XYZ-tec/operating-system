// nexos_shell.cpp — implement using apps/app_template.cpp as base
#include "../include/theme.h"
#include "../include/resources.h"
#include "../include/ipc.h"

#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <dirent.h>
#include <fcntl.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <sstream>
#include <fstream>

#define APP_NAME  "Shell"
#define RAM_MB    60
#define HDD_MB    10

// ── Terminal layout ───────────────────────────────────────
#define TERM_FONT_SIZE  15
#define TERM_LINE_H     18
#define TERM_PAD_X      10
#define TERM_PAD_Y       8
#define INPUT_H          28
#define TOPBAR_H         32

// ── Terminal colours ──────────────────────────────────────
static const Color TERM_BG       = {  5,  5, 12, 255};
static const Color TERM_PROMPT   = {  0,255,200, 255}; // cyan
static const Color TERM_CMD      = {220,220,255, 255}; // white
static const Color TERM_OUTPUT   = {160,160,200, 255}; // light purple
static const Color TERM_ERROR    = {255, 80, 80, 255}; // red
static const Color TERM_SUCCESS  = { 57,255, 20, 255}; // green
static const Color TERM_DIR      = {  0,200,255, 255}; // light blue
static const Color TERM_FILE     = {200,200,255, 255}; // white-ish
static const Color TERM_CURSOR   = {  0,255,200, 255};
static const Color INPUT_BG      = { 10, 10, 24, 255};

// ── Font ──────────────────────────────────────────────────
static Font shFont; static bool shFontOK=false;
static void DT(const char* t,int x,int y,Color c){
    if(shFontOK)DrawTextEx(shFont,t,{(float)x,(float)y},(float)TERM_FONT_SIZE,1.0f,c);
    else DrawText(t,x,y,TERM_FONT_SIZE,c);}
static int MT(const char* t){
    if(shFontOK)return(int)MeasureTextEx(shFont,t,(float)TERM_FONT_SIZE,1.0f).x;
    return MeasureText(t,TERM_FONT_SIZE);}

// ============================================================
//  Terminal line — each line has text + colour
// ============================================================
struct TermLine {
    std::string text;
    Color       color;
};

// ============================================================
//  Shell state
// ============================================================
static std::vector<TermLine>  termLines;
static std::string            currentDir   = "hdd";
static char                   inputBuf[512]= "";
static int                    inputLen     = 0;
static int                    scrollOffset = 0;  // lines scrolled from bottom
static bool                   appRunning   = true;

// History
static std::vector<std::string> history;
static int                      histIdx    = -1;

static std::string BuildPrompt(){
    char user[64] = "nexos_user";
    if (getlogin_r(user, sizeof(user)) != 0 || user[0] == '\0') {
        strncpy(user, "nexos_user", sizeof(user) - 1);
        user[sizeof(user) - 1] = '\0';
    }
    return std::string(user) + "@NexOS:" + currentDir + "$ ";
}

// ============================================================
//  Output helpers
// ============================================================
static void PushLine(const std::string& text, Color color){
    // Split on newlines
    std::string line;
    for(char c:text){
        if(c=='\n'){termLines.push_back({line,color});line="";}
        else line+=c;
    }
    if(!line.empty()||text.empty())termLines.push_back({line,color});
}

static void PushPrompt(){
    termLines.push_back({BuildPrompt(),TERM_PROMPT});
}

static std::string GetHostPrompt(){
    return BuildPrompt();
}

// ============================================================
//  Tokenise command line
// ============================================================
static std::vector<std::string> Tokenise(const std::string& cmd){
    std::vector<std::string> tokens;
    std::string tok;
    bool inQ=false;
    for(char c:cmd){
        if(c=='"'){inQ=!inQ;}
        else if(c==' '&&!inQ){if(!tok.empty()){tokens.push_back(tok);tok="";}}
        else tok+=c;
    }
    if(!tok.empty())tokens.push_back(tok);
    return tokens;
}

// ============================================================
//  Built-in: ls
// ============================================================
static void CmdLs(const std::string& path){
    std::string target=path.empty()?currentDir:path;
    DIR* d=opendir(target.c_str());
    if(!d){PushLine("ls: cannot access '"+target+"': No such directory",TERM_ERROR);return;}
    std::vector<std::string> dirs,files;
    struct dirent* e;
    while((e=readdir(d))){
        std::string n(e->d_name);
        if(n=="."||n=="..")continue;
        struct stat st;
        std::string fp=target+"/"+n;
        stat(fp.c_str(),&st);
        if(S_ISDIR(st.st_mode))dirs.push_back(n+"/");
        else files.push_back(n);
    }
    closedir(d);
    std::sort(dirs.begin(),dirs.end());
    std::sort(files.begin(),files.end());

    // Format in columns
    std::string line;
    int col=0;
    auto flush=[&](){if(!line.empty()){PushLine(line,TERM_OUTPUT);line="";col=0;}};
    for(auto&n:dirs){
        char buf[40];snprintf(buf,sizeof(buf),"%-20s",n.c_str());
        line+=buf;
        if(++col>=4)flush();
    }
    flush();
    for(auto&n:files){
        char buf[40];snprintf(buf,sizeof(buf),"%-20s",n.c_str());
        line+=buf;
        if(++col>=4)flush();
    }
    flush();
    if(dirs.empty()&&files.empty())PushLine("(empty directory)",TERM_OUTPUT);
}

// ============================================================
//  Built-in: cat
// ============================================================
static void CmdCat(const std::string& file){
    std::string fp;
    if(file[0]=='/'||file.substr(0,3)=="hdd")fp=file;
    else fp=currentDir+"/"+file;

    std::ifstream f(fp);
    if(!f.is_open()){PushLine("cat: "+file+": No such file",TERM_ERROR);return;}
    std::string line;
    int count=0;
    while(std::getline(f,line)&&count<200){
        PushLine(line,TERM_OUTPUT);
        count++;
    }
    if(count==200)PushLine("... (truncated at 200 lines)",TERM_ERROR);
    f.close();
}

// ============================================================
//  Built-in: cp
// ============================================================
static void CmdCp(const std::string& src,const std::string& dst){
    std::string srcFp=src;
    std::string dstFp=dst;
    if(src[0]!='/')srcFp=currentDir+"/"+src;
    if(dst[0]!='/')dstFp=currentDir+"/"+dst;

    // If dst is a directory, copy file into it
    struct stat st;
    if(stat(dstFp.c_str(),&st)==0&&S_ISDIR(st.st_mode)){
        auto slash=srcFp.rfind('/');
        std::string fname=(slash!=std::string::npos)?srcFp.substr(slash+1):srcFp;
        dstFp=dstFp+"/"+fname;
    }

    std::ifstream in(srcFp,std::ios::binary);
    if(!in.is_open()){PushLine("cp: cannot open '"+src+"'",TERM_ERROR);return;}
    std::ofstream out(dstFp,std::ios::binary);
    if(!out.is_open()){PushLine("cp: cannot create '"+dst+"'",TERM_ERROR);return;}
    out<<in.rdbuf();
    PushLine("Copied '"+src+"' -> '"+dst+"'",TERM_SUCCESS);
}

// ============================================================
//  Built-in: mv
// ============================================================
static void CmdMv(const std::string& src,const std::string& dst){
    std::string srcFp=(src[0]!='/')?currentDir+"/"+src:src;
    std::string dstFp=(dst[0]!='/')?currentDir+"/"+dst:dst;
    struct stat st;
    if(stat(dstFp.c_str(),&st)==0&&S_ISDIR(st.st_mode)){
        auto slash=srcFp.rfind('/');
        std::string fname=(slash!=std::string::npos)?srcFp.substr(slash+1):srcFp;
        dstFp=dstFp+"/"+fname;
    }
    if(rename(srcFp.c_str(),dstFp.c_str())==0)
        PushLine("Moved '"+src+"' -> '"+dst+"'",TERM_SUCCESS);
    else
        PushLine("mv: failed to move '"+src+"'",TERM_ERROR);
}

// ============================================================
//  Execute a command
// ============================================================
static void ExecuteCommand(const std::string& raw){
    if(raw.empty())return;

    // Add to history
    if(history.empty()||history.back()!=raw)
        history.push_back(raw);
    histIdx=-1;

    // Echo the command with prompt
    std::string promptLine=GetHostPrompt()+raw;
    termLines.back().text=promptLine;
    termLines.back().color=TERM_PROMPT;

    // Tokenise
    auto tokens=Tokenise(raw);
    if(tokens.empty())return;

    std::string cmd=tokens[0];

    // ── Built-ins ─────────────────────────────────────────

    if(cmd=="exit"||cmd=="quit"){
        appRunning=false;
        return;
    }

    if(cmd=="clear"){
        termLines.clear();
        PushPrompt();
        return;
    }

    if(cmd=="help"){
        PushLine("NexOS Shell - Available Commands:",TERM_PROMPT);
        PushLine("",TERM_OUTPUT);
        PushLine("  Navigation:",TERM_SUCCESS);
        PushLine("    cd <dir>         Change directory",TERM_OUTPUT);
        PushLine("    ls [dir]         List directory contents",TERM_OUTPUT);
        PushLine("    pwd              Print working directory",TERM_OUTPUT);
        PushLine("",TERM_OUTPUT);
        PushLine("  File Operations:",TERM_SUCCESS);
        PushLine("    mkdir <dir>      Create directory",TERM_OUTPUT);
        PushLine("    rm <file>        Remove file",TERM_OUTPUT);
        PushLine("    cp <src> <dst>   Copy file",TERM_OUTPUT);
        PushLine("    mv <src> <dst>   Move / rename file",TERM_OUTPUT);
        PushLine("    cat <file>       Display file contents",TERM_OUTPUT);
        PushLine("    touch <file>     Create empty file",TERM_OUTPUT);
        PushLine("",TERM_OUTPUT);
        PushLine("  Output:",TERM_SUCCESS);
        PushLine("    echo <text>      Print text",TERM_OUTPUT);
        PushLine("",TERM_OUTPUT);
        PushLine("  Shell:",TERM_SUCCESS);
        PushLine("    clear            Clear screen",TERM_OUTPUT);
        PushLine("    history          Show command history",TERM_OUTPUT);
        PushLine("    help             Show this help",TERM_OUTPUT);
        PushLine("    exit             Close shell",TERM_OUTPUT);
        PushLine("",TERM_OUTPUT);
        PushLine("  Tips:",TERM_DIR);
        PushLine("    UP/DOWN keys    Navigate command history",TERM_OUTPUT);
        PushLine("    Tab             Auto-complete (first match)",TERM_OUTPUT);
        PushLine("    Files live in: hdd/",TERM_OUTPUT);
        PushPrompt();
        return;
    }

    if(cmd=="pwd"){
        PushLine(currentDir,TERM_OUTPUT);
        PushPrompt();
        return;
    }

    if(cmd=="cd"){
        std::string target=(tokens.size()>1)?tokens[1]:"hdd";
        // Resolve relative
        std::string resolved;
        if(target==".."){
            auto slash=currentDir.rfind('/');
            resolved=(slash!=std::string::npos)?currentDir.substr(0,slash):"hdd";
        } else if(target[0]=='/'){
            resolved=target;
        } else if(target=="~"||target=="hdd"){
            resolved="hdd";
        } else {
            resolved=currentDir+"/"+target;
        }
        struct stat st;
        if(stat(resolved.c_str(),&st)==0&&S_ISDIR(st.st_mode)){
            currentDir=resolved;
            PushLine("",TERM_OUTPUT);
        } else {
            PushLine("cd: "+target+": No such directory",TERM_ERROR);
        }
        PushPrompt();
        return;
    }

    if(cmd=="ls"){
        std::string arg=(tokens.size()>1)?tokens[1]:"";
        if(!arg.empty()&&arg[0]!='/') arg=currentDir+"/"+arg;
        CmdLs(arg);
        PushPrompt();
        return;
    }

    if(cmd=="mkdir"){
        if(tokens.size()<2){PushLine("Usage: mkdir <directory>",TERM_ERROR);PushPrompt();return;}
        std::string fp=(tokens[1][0]!='/')?currentDir+"/"+tokens[1]:tokens[1];
        if(mkdir(fp.c_str(),0755)==0)PushLine("Directory created: "+tokens[1],TERM_SUCCESS);
        else PushLine("mkdir: cannot create '"+tokens[1]+"': already exists or permission denied",TERM_ERROR);
        PushPrompt();
        return;
    }

    if(cmd=="rm"){
        if(tokens.size()<2){PushLine("Usage: rm <file>",TERM_ERROR);PushPrompt();return;}
        std::string fp=(tokens[1][0]!='/')?currentDir+"/"+tokens[1]:tokens[1];
        // Safety: only allow deleting inside hdd/
        if(fp.find("hdd/")==std::string::npos){
            PushLine("rm: restricted to hdd/ directory for safety",TERM_ERROR);
            PushPrompt();return;
        }
        if(remove(fp.c_str())==0)PushLine("Removed: "+tokens[1],TERM_SUCCESS);
        else PushLine("rm: cannot remove '"+tokens[1]+"': No such file",TERM_ERROR);
        PushPrompt();
        return;
    }

    if(cmd=="cp"){
        if(tokens.size()<3){PushLine("Usage: cp <source> <destination>",TERM_ERROR);PushPrompt();return;}
        CmdCp(tokens[1],tokens[2]);
        PushPrompt();
        return;
    }

    if(cmd=="mv"){
        if(tokens.size()<3){PushLine("Usage: mv <source> <destination>",TERM_ERROR);PushPrompt();return;}
        CmdMv(tokens[1],tokens[2]);
        PushPrompt();
        return;
    }

    if(cmd=="cat"){
        if(tokens.size()<2){PushLine("Usage: cat <file>",TERM_ERROR);PushPrompt();return;}
        CmdCat(tokens[1]);
        PushPrompt();
        return;
    }

    if(cmd=="touch"){
        if(tokens.size()<2){PushLine("Usage: touch <file>",TERM_ERROR);PushPrompt();return;}
        std::string fp=(tokens[1][0]!='/')?currentDir+"/"+tokens[1]:tokens[1];
        std::ofstream f(fp);
        if(f.is_open()){f.close();PushLine("Created: "+tokens[1],TERM_SUCCESS);}
        else PushLine("touch: cannot create '"+tokens[1]+"'",TERM_ERROR);
        PushPrompt();
        return;
    }

    if(cmd=="echo"){
        std::string out;
        for(int i=1;i<(int)tokens.size();i++){
            if(i>1)out+=" ";
            out+=tokens[i];
        }
        PushLine(out,TERM_OUTPUT);
        PushPrompt();
        return;
    }

    if(cmd=="history"){
        if(history.empty()){PushLine("No history yet.",TERM_OUTPUT);}
        else{
            for(int i=0;i<(int)history.size();i++){
                char buf[8];snprintf(buf,sizeof(buf),"%3d ",i+1);
                PushLine(std::string(buf)+history[i],TERM_OUTPUT);
            }
        }
        PushPrompt();
        return;
    }

    if(cmd=="date"){
        time_t now=time(nullptr);
        char buf[64];strftime(buf,sizeof(buf),"%A %B %d %Y %H:%M:%S",localtime(&now));
        PushLine(std::string(buf),TERM_OUTPUT);
        PushPrompt();
        return;
    }

    if(cmd=="whoami"){
        char user[64]="nexos_user";
        getlogin_r(user,sizeof(user));
        PushLine(std::string(user),TERM_OUTPUT);
        PushPrompt();
        return;
    }

    if(cmd=="uname"){
        PushLine("NexOS 1.0 (Multi-Process OS Simulator)",TERM_OUTPUT);
        PushPrompt();
        return;
    }

    // ── External commands via fork/exec ───────────────────
    // Build argv
    std::vector<const char*> argv;
    for(auto& t:tokens)argv.push_back(t.c_str());
    argv.push_back(nullptr);

    // Capture output via pipe
    int pipeFd[2];
    if(pipe(pipeFd)<0){
        PushLine("shell: pipe failed",TERM_ERROR);
        PushPrompt();
        return;
    }

    pid_t pid=fork();
    if(pid<0){
        PushLine("shell: fork failed",TERM_ERROR);
        close(pipeFd[0]);close(pipeFd[1]);
        PushPrompt();
        return;
    }

    if(pid==0){
        // Child: redirect stdout+stderr to pipe
        close(pipeFd[0]);
        dup2(pipeFd[1],STDOUT_FILENO);
        dup2(pipeFd[1],STDERR_FILENO);
        close(pipeFd[1]);
        // Change to current dir
        chdir(currentDir.c_str());
        execvp(argv[0],(char*const*)argv.data());
        // exec failed
        const char* msg="command not found\n";
        write(STDERR_FILENO,msg,strlen(msg));
        exit(127);
    }

    // Parent: read output
    close(pipeFd[1]);
    char buf[4096];
    std::string output;
    ssize_t n;
    while((n=read(pipeFd[0],buf,sizeof(buf)-1))>0){
        buf[n]='\0';
        output+=std::string(buf);
    }
    close(pipeFd[0]);

    int status;
    waitpid(pid,&status,0);
    int exitCode=WIFEXITED(status)?WEXITSTATUS(status):-1;

    // Display output
    if(!output.empty()){
        Color outColor=(exitCode==0)?TERM_OUTPUT:TERM_ERROR;
        // Split output into lines
        std::istringstream ss(output);
        std::string line;
        while(std::getline(ss,line))PushLine(line,outColor);
    }

    if(exitCode==127){
        PushLine(cmd+": command not found. Type 'help' for available commands.",TERM_ERROR);
    }

    PushPrompt();
}

// ============================================================
//  Tab auto-complete
// ============================================================
static void DoTabComplete(){
    if(inputLen==0)return;
    std::string input(inputBuf);

    // Find last token (the one to complete)
    auto lastSpace=input.rfind(' ');
    std::string prefix=(lastSpace==std::string::npos)?input:input.substr(lastSpace+1);
    std::string before=(lastSpace==std::string::npos)?"":input.substr(0,lastSpace+1);

    // List files in current dir matching prefix
    std::vector<std::string> matches;
    DIR* d=opendir(currentDir.c_str());
    if(d){
        struct dirent* e;
        while((e=readdir(d))){
            std::string n(e->d_name);
            if(n=="."||n=="..")continue;
            if(n.substr(0,prefix.size())==prefix)matches.push_back(n);
        }
        closedir(d);
    }

    if(matches.size()==1){
        // Complete it
        std::string completed=before+matches[0];
        strncpy(inputBuf,completed.c_str(),511);
        inputLen=(int)strlen(inputBuf);
    } else if(matches.size()>1){
        // Show all matches
        std::string hint;
        for(auto&m:matches)hint+=m+"  ";
        // Don't push prompt — just show hint above input
        PushLine(hint,TERM_DIR);
    }
}

// ============================================================
//  Draw top bar
// ============================================================
static void DrawTopBar(int sw){
    DrawRectangle(0,0,sw,TOPBAR_H,{10,10,22,255});
    DrawLine(0,TOPBAR_H,sw,TOPBAR_H,{0,255,200,60});

    // Shell title
    DrawText("NexOS Shell",10,9,FONT_NORMAL,NEON_CYAN);

    // Current dir
    std::string dirInfo="  ["+currentDir+"]";
    DrawText(dirInfo.c_str(),MeasureText("NexOS Shell",FONT_NORMAL)+12,10,FONT_SMALL,TEXT_MUTED);

    // Shortcuts hint
    const char* hint="UP/DOWN history  Tab autocomplete  help  exit";
    DrawText(hint,sw-MeasureText(hint,FONT_TINY)-10,11,FONT_TINY,TEXT_DIM);
}

// ============================================================
//  Draw terminal output
// ============================================================
static void DrawTerminal(int sw,int sh){
    int termH=sh-TOPBAR_H-INPUT_H-2;
    int termY=TOPBAR_H;

    DrawRectangle(0,termY,sw,termH,TERM_BG);

    int visLines=termH/TERM_LINE_H;
    int totalLines=(int)termLines.size();

    // Auto-scroll to bottom unless user scrolled up
    int maxScroll=std::max(0,totalLines-visLines);
    if(scrollOffset>maxScroll)scrollOffset=maxScroll;

    int startLine=totalLines-visLines-scrollOffset;
    if(startLine<0)startLine=0;

    // Scissor
    BeginScissorMode(0,termY,sw,termH);
    for(int i=startLine;i<totalLines&&i<startLine+visLines+1;i++){
        int ry=termY+(i-startLine)*TERM_LINE_H+TERM_PAD_Y/2;
        if(!termLines[i].text.empty())
            DT(termLines[i].text.c_str(),TERM_PAD_X,ry,termLines[i].color);
    }

    // Draw cursor on last line if at bottom
    if(scrollOffset==0&&!termLines.empty()){
        // cursor is drawn in the input bar, not here
    }
    EndScissorMode();

    // Scrollbar
    if(totalLines>visLines){
        float frac=(float)(maxScroll-scrollOffset)/std::max(1,maxScroll);
        int sbH=std::max(20,(int)((float)visLines/totalLines*termH));
        int sbY=termY+(int)(frac*(termH-sbH));
        DrawRectangle(sw-4,termY,4,termH,{20,20,40,200});
        DrawRectangle(sw-4,sbY,4,sbH,{0,255,200,120});
    }

    // Scroll wheel
    float wh=GetMouseWheelMove();
    if(wh!=0){
        scrollOffset=std::max(0,std::min(maxScroll,scrollOffset+(int)wh*3));
    }
}

// ============================================================
//  Draw input bar
// ============================================================
static void DrawInputBar(int sw,int sh){
    int iy=sh-INPUT_H;
    DrawRectangle(0,iy-1,sw,1,{0,255,200,40});
    DrawRectangle(0,iy,sw,INPUT_H,INPUT_BG);

    // Prompt
    std::string prompt=GetHostPrompt();
    int pw=MT(prompt.c_str());
    DT(prompt.c_str(),TERM_PAD_X,iy+7,TERM_PROMPT);

    // Input text
    DT(inputBuf,TERM_PAD_X+pw+2,iy+7,TERM_CMD);

    // Blinking cursor
    if((int)(GetTime()*2)%2==0){
        int cw=MT(inputBuf);
        DrawRectangle(TERM_PAD_X+pw+2+cw,iy+5,2,TERM_LINE_H-2,TERM_CURSOR);
    }
}

// ============================================================
//  Handle input
// ============================================================
static void HandleInput(){
    // Character input
    int k=GetCharPressed();
    while(k>0){
        if(k>=32&&k<127&&inputLen<510){
            inputBuf[inputLen++]=(char)k;
            inputBuf[inputLen]='\0';
        }
        k=GetCharPressed();
    }

    // Backspace
    if((IsKeyPressed(KEY_BACKSPACE)||IsKeyPressedRepeat(KEY_BACKSPACE))&&inputLen>0)
        inputBuf[--inputLen]='\0';

    // Enter — execute
    if(IsKeyPressed(KEY_ENTER)){
        std::string cmd(inputBuf);
        memset(inputBuf,0,512);
        inputLen=0;
        scrollOffset=0; // snap to bottom
        ExecuteCommand(cmd);
    }

    // Tab — autocomplete
    if(IsKeyPressed(KEY_TAB)) DoTabComplete();

    // Up — history prev
    if(IsKeyPressed(KEY_UP)){
        if(!history.empty()){
            if(histIdx<0)histIdx=(int)history.size()-1;
            else histIdx=std::max(0,histIdx-1);
            strncpy(inputBuf,history[histIdx].c_str(),511);
            inputLen=(int)strlen(inputBuf);
        }
    }

    // Down — history next
    if(IsKeyPressed(KEY_DOWN)){
        if(histIdx>=0){
            histIdx++;
            if(histIdx>=(int)history.size()){
                histIdx=-1;
                memset(inputBuf,0,512);inputLen=0;
            } else {
                strncpy(inputBuf,history[histIdx].c_str(),511);
                inputLen=(int)strlen(inputBuf);
            }
        }
    }

    // Ctrl+C — cancel current input
    if((IsKeyDown(KEY_LEFT_CONTROL)||IsKeyDown(KEY_RIGHT_CONTROL))&&IsKeyPressed(KEY_C)){
        std::string cancelled=GetHostPrompt()+std::string(inputBuf)+"^C";
        if(!termLines.empty())termLines.back().text=cancelled;
        memset(inputBuf,0,512);inputLen=0;
        PushPrompt();
        scrollOffset=0;
    }

    // Ctrl+L — clear
    if((IsKeyDown(KEY_LEFT_CONTROL)||IsKeyDown(KEY_RIGHT_CONTROL))&&IsKeyPressed(KEY_L)){
        termLines.clear();
        PushPrompt();
        scrollOffset=0;
    }
}

// ============================================================
//  MAIN
// ============================================================
int main(){
    if(!RequestResources(APP_NAME,RAM_MB,HDD_MB,PRIORITY_NORMAL,1)){
        InitWindow(440,120,"Shell - Denied");SetTargetFPS(30);
        double t=GetTime();
        while(!WindowShouldClose()&&GetTime()-t<3.5){
            BeginDrawing();ClearBackground(BG_DEEP);
            DrawText("Insufficient resources.",18,40,FONT_NORMAL,NEON_PINK);EndDrawing();}
        CloseWindow();return 1;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(860,580,"NexOS Shell");
    SetTargetFPS(60);SetExitKey(KEY_NULL);
    SetWindowFocused();

    // Load monospace font with ASCII + required Unicode glyphs.
    shFontOK=false;
    if(FileExists("assets/fonts/JetBrainsMono-Regular.ttf")){
        std::vector<int> cps;
        cps.reserve(140);
        for(int cp=32; cp<=126; cp++) cps.push_back(cp); // full printable ASCII
        int extras[] = {
            0x2014, // —
            0x2191, // ↑
            0x2193, // ↓
            0x2192, // →
            0x2500, // ─
            0x2550, // ═
            0x2551, // ║
            0x2554, // ╔
            0x2557, // ╗
            0x255A, // ╚
            0x255D, // ╝
            0x2588  // █
        };
        cps.insert(cps.end(), std::begin(extras), std::end(extras));

        shFont = LoadFontEx(
            "assets/fonts/JetBrainsMono-Regular.ttf",
            28,
            cps.data(),
            (int)cps.size()
        );
        shFontOK=(shFont.texture.id>0);
        if(shFontOK)SetTextureFilter(shFont.texture,TEXTURE_FILTER_BILINEAR);
    }


    // Ensure hdd exists
    mkdir("hdd",0755);

    // Boot message
    PushLine("",TERM_OUTPUT);
    PushLine("  ===========================================", NEON_CYAN);
    PushLine("          N E X O S   S H E L L", NEON_CYAN);
    PushLine("  ===========================================", NEON_CYAN);
    PushLine("",TERM_OUTPUT);
    PushLine("  NexOS Shell v1.0 - A Mini Shell Inside Your OS",TERM_PROMPT);
    PushLine("  Type 'help' to see all commands.",TEXT_MUTED);
    PushLine("  Files are stored in: hdd/",TEXT_MUTED);
    PushLine("",TERM_OUTPUT);
    PushPrompt();

    while(!WindowShouldClose()&&appRunning){
        int sw=GetScreenWidth(),sh=GetScreenHeight();

        HandleInput();

        BeginDrawing();
        ClearBackground(TERM_BG);
        DrawTopBar(sw);
        DrawTerminal(sw,sh);
        DrawInputBar(sw,sh);
        EndDrawing();
    }

    if(shFontOK)UnloadFont(shFont);
    ReleaseResources(APP_NAME,RAM_MB,HDD_MB);
    CloseWindow();
    return 0;
}
