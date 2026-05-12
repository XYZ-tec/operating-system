// ============================================================
//  NexOS Chat  —  TCP peer-to-peer text chat
//  One partner runs HOST, the other runs JOIN.
//  Works on a LAN, ZeroTier/Tailscale VPN, or with port-fwd.
//  Port default: 9999 (editable in the setup screen).
// ============================================================
#include "raylib.h"
#include "../include/theme.h"
#include "../include/resources.h"
#include "../include/ipc.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <string>
#include <vector>
#include <mutex>
#include <atomic>
#include <algorithm>

#define APP_NAME  "Chat"
#define RAM_MB    20
#define HDD_MB    1
#define WIN_W     900
#define WIN_H     620
#define CHAT_PORT 9999
#define MAX_MSGS  300
#define MSG_MAX   480

// ── Font ──────────────────────────────────────────────────
static Font  gFont;
static bool  gFontOK = false;
static void  DT(const char* t,int x,int y,int sz,Color c){
    if(gFontOK) DrawTextEx(gFont,t,{(float)x,(float)y},(float)sz,1.2f,c);
    else DrawText(t,x,y,sz,c);}
static int   MT(const char* t,int sz){
    if(gFontOK) return (int)MeasureTextEx(gFont,t,(float)sz,1.2f).x;
    return MeasureText(t,sz);}

// ── Connection state machine ───────────────────────────────
enum ConnState { SETUP, CONNECTING, CONNECTED, DISCONNECTED, CONN_ERR };
static std::atomic<ConnState> connState{SETUP};
static std::atomic<bool>      appRunning{true};

static int listenFd = -1;   // server listen socket
static int peerFd   = -1;   // active peer socket

// ── Messages ──────────────────────────────────────────────
struct Msg {
    std::string name;
    std::string text;
    std::string ts;
    bool mine;
    bool sys;
    bool hist = false;
};
static std::vector<Msg> msgs;
static std::mutex       msgMtx;
static int              scrollOff = 0;

static std::string NowHHMM() {
    time_t t = time(nullptr);
    tm* lt   = localtime(&t);
    char b[8]; snprintf(b,sizeof(b),"%02d:%02d",lt->tm_hour,lt->tm_min);
    return b;
}
#define HIST_FILE    "hdd/chat_history.txt"
#define HIST_DELIM   '\x01'
#define MAX_HIST_LOAD 150
static void SaveMsg(const Msg& m){
    FILE* f=fopen(HIST_FILE,"a");
    if(!f) return;
    std::string safeText=m.text, safeName=m.name;
    for(char& c:safeText) if(c==HIST_DELIM||c=='\n'||c=='\r') c=' ';
    for(char& c:safeName) if(c==HIST_DELIM||c=='\n'||c=='\r') c=' ';
    char type=m.sys?'S':(m.mine?'M':'T');
    fprintf(f,"%c%c%s%c%s%c%s\n",
            type,HIST_DELIM,
            safeName.c_str(),HIST_DELIM,
            m.ts.c_str(),HIST_DELIM,
            safeText.c_str());
    fclose(f);
}
static void LoadHistory(){
    FILE* f=fopen(HIST_FILE,"r");
    if(!f) return;
    std::vector<std::string> lines;
    char buf[1024];
    while(fgets(buf,sizeof(buf),f)){
        size_t len=strlen(buf);
        while(len>0&&(buf[len-1]=='\n'||buf[len-1]=='\r')) buf[--len]='\0';
        if(len>0) lines.push_back(buf);
    }
    fclose(f);
    if(lines.empty()) return;
    if((int)lines.size()>MAX_HIST_LOAD)
        lines.erase(lines.begin(),lines.begin()+(int)lines.size()-MAX_HIST_LOAD);
    for(const auto& line:lines){
        // split on HIST_DELIM into 4 parts: TYPE, NAME, TS, TEXT
        std::vector<std::string> parts;
                std::string cur;
        for(char c:line){
            if(c==HIST_DELIM){ parts.push_back(cur); cur.clear(); }
            else cur+=c;
        }
        parts.push_back(cur);
        if((int)parts.size()<4) continue;
        char type=parts[0].empty()?'T':parts[0][0];
        Msg m;
        m.name=parts[1]; m.ts=parts[2]; m.text=parts[3];
        m.sys=(type=='S'); m.mine=(type=='M'); m.hist=true;
        msgs.push_back(m);
    }
    // separator between history and new session
    Msg sep;
    sep.text="session started "+NowHHMM();
    sep.sys=true; sep.mine=false; sep.hist=false;
    msgs.push_back(sep);
    scrollOff=0;
}

static void PushRaw(const std::string& name,const std::string& text,bool mine,bool sys){
    std::lock_guard<std::mutex> lk(msgMtx);
    if((int)msgs.size()>=MAX_MSGS) msgs.erase(msgs.begin());
       Msg m; m.name=name; m.text=text; m.ts=NowHHMM(); m.mine=mine; m.sys=sys; m.hist=false;
    msgs.push_back(m);
    if(!sys) SaveMsg(m);
    int total=(int)msgs.size();
    if(scrollOff==0||(total>1&&scrollOff==(total-2))) scrollOff=0;
}
static void PushSys(const std::string& t){ PushRaw("",t,false,true); }

// ── Network helpers ────────────────────────────────────────
static bool SendLine(const std::string& name,const std::string& text){
    if(peerFd<0) return false;
    std::string line = name+":"+text+"\n";
    ssize_t sent=0, total=(ssize_t)line.size();
    while(sent<total){
        ssize_t n=send(peerFd,line.c_str()+sent,total-sent,MSG_NOSIGNAL);
        if(n<=0) return false;
        sent+=n;
    }
    return true;
}

// ── Receiver thread ────────────────────────────────────────
static void* RecvThread(void*){
    char buf[2048];
    std::string partial;
    while(appRunning && peerFd>=0){
        ssize_t n=recv(peerFd,buf,sizeof(buf)-1,0);
        if(n<=0){
            if(appRunning){
                PushSys("Partner disconnected.");
                connState.store(DISCONNECTED);
            }
            break;
        }
        buf[n]='\0';
        partial+=buf;
        size_t pos;
        while((pos=partial.find('\n'))!=std::string::npos){
            std::string line=partial.substr(0,pos);
            partial=partial.substr(pos+1);
            if(line.empty()) continue;
            size_t c=line.find(':');
            if(c!=std::string::npos)
                PushRaw(line.substr(0,c),line.substr(c+1),false,false);
            else
                PushRaw("?",line,false,false);
        }
    }
    return nullptr;
}

// ── Server accept thread ───────────────────────────────────
static void* ServerThread(void*){
    struct sockaddr_in addr{}; socklen_t al=sizeof(addr);
    int client=accept(listenFd,(struct sockaddr*)&addr,&al);
    if(client<0){
        if(appRunning) PushSys("accept() failed — try restarting.");
        connState.store(DISCONNECTED);
        return nullptr;
    }
    char ip[INET_ADDRSTRLEN];
    inet_ntop(AF_INET,&addr.sin_addr,ip,sizeof(ip));
    peerFd=client;
    connState.store(CONNECTED);
    PushSys(std::string("Partner connected from ")+ip+"  —  you may now chat.");
    pthread_t rt; pthread_create(&rt,nullptr,RecvThread,nullptr); pthread_detach(rt);
    return nullptr;
}

// ── Client connect thread ──────────────────────────────────
static char  g_targetIP[64]="127.0.0.1";
static char  g_portBuf[8]  ="9999";

static void* ClientThread(void*){
    int port=atoi(g_portBuf);
    if(port<=0||port>65535) port=CHAT_PORT;
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0){ PushSys("socket() failed."); connState.store(CONN_ERR); return nullptr; }
    struct timeval tv{10,0};
    setsockopt(fd,SOL_SOCKET,SO_SNDTIMEO,&tv,sizeof(tv));
    struct sockaddr_in sa{};
    sa.sin_family=AF_INET;
    sa.sin_port=htons(port);
    if(inet_pton(AF_INET,g_targetIP,&sa.sin_addr)<=0){
        close(fd); PushSys("Invalid IP address."); connState.store(CONN_ERR); return nullptr;
    }
    if(connect(fd,(struct sockaddr*)&sa,sizeof(sa))<0){
        close(fd);
        PushSys(std::string("Connect failed: ")+strerror(errno));
        connState.store(CONN_ERR); return nullptr;
    }
    peerFd=fd;
    connState.store(CONNECTED);
    PushSys("Connected to host  —  you may now chat.");
    pthread_t rt; pthread_create(&rt,nullptr,RecvThread,nullptr); pthread_detach(rt);
    return nullptr;
}

// ── Setup screen state ─────────────────────────────────────
static int   modeChoice  = 0;
static char  myName[32]  = "User";
static int   myNameLen   = 4;
static int   targetIPLen = 9;
static int   portLen     = 4;
static int   focusField  = 0;
static std::string errMsg;

// ── Input bar state ────────────────────────────────────────
static char inputBuf[MSG_MAX]="";
static int  inputLen=0;

// ── Reset to setup ─────────────────────────────────────────
static void ResetToSetup(){
    if(peerFd>=0){ close(peerFd); peerFd=-1; }
    if(listenFd>=0){ close(listenFd); listenFd=-1; }
    {std::lock_guard<std::mutex> lk(msgMtx); msgs.clear();}
    inputBuf[0]='\0'; inputLen=0;
    errMsg.clear();
    modeChoice=0;
    scrollOff=0;
    connState.store(SETUP);
}

// ── Start host ────────────────────────────────────────────
static void StartHost(){
    int port=atoi(g_portBuf);
    if(port<=0||port>65535) port=CHAT_PORT;
    int fd=socket(AF_INET,SOCK_STREAM,0);
    if(fd<0){ errMsg="socket() failed."; return; }
    int opt=1; setsockopt(fd,SOL_SOCKET,SO_REUSEADDR,&opt,sizeof(opt));
    struct sockaddr_in sa{}; sa.sin_family=AF_INET;
    sa.sin_addr.s_addr=INADDR_ANY; sa.sin_port=htons(port);
    if(bind(fd,(struct sockaddr*)&sa,sizeof(sa))<0){
        close(fd); errMsg="bind() failed — port already in use?"; return;
    }
    listen(fd,1);
    listenFd=fd;
    connState.store(CONNECTING);
    PushSys(std::string("Listening on port ")+g_portBuf+" — waiting for partner...");
    pthread_t th; pthread_create(&th,nullptr,ServerThread,nullptr); pthread_detach(th);
}

// ── Start join ────────────────────────────────────────────
static void StartJoin(){
    connState.store(CONNECTING);
    PushSys(std::string("Connecting to ")+g_targetIP+":"+g_portBuf+" ...");
    pthread_t th; pthread_create(&th,nullptr,ClientThread,nullptr); pthread_detach(th);
}

// ==========================================================
//  Drawing helpers
// ==========================================================
static void DrawGlow(int x,int y,int w,int h,Color c,float strength=4.0f){
    Color dim=c; dim.a=30;
    for(float r=strength;r>=1.0f;r-=1.0f){
        DrawRectangleLinesEx({(float)(x-(int)r),(float)(y-(int)r),
                              (float)(w+2*(int)r),(float)(h+2*(int)r)},1.0f,dim);
        dim.a=(unsigned char)(dim.a+10);
    }
}

// ==========================================================
//  SETUP SCREEN
// ==========================================================
static void HandleFieldInput(char* field,int& len,int maxLen){
    int key=GetCharPressed();
    while(key>0){
        if(key>=32&&key<127&&len<maxLen){ field[len++]=(char)key; field[len]='\0'; }
        key=GetCharPressed();
    }
    if(IsKeyPressed(KEY_BACKSPACE)&&len>0){ field[--len]='\0'; }
}

static void DrawField(const char* label,char* field,int& len,int maxLen,
                      int fieldIdx,float x,float y,float w){
    bool focused=(focusField==fieldIdx);
    DrawRectangleRounded({x,y,w,38},0.15f,8,BG_PANEL);
    DrawRectangleLinesEx({x,y,w,38},focused?1.5f:1.0f,focused?NEON_CYAN:BORDER_DIM);
    if(focused) DrawGlow((int)x,(int)y,(int)w,38,NEON_CYAN,3.0f);
    DT(label,(int)x,(int)(y-20),FONT_SMALL,TEXT_MUTED);
    DT(field,(int)(x+12),(int)(y+10),FONT_NORMAL,TEXT_PRIMARY);
    if(focused&&(int)(GetTime()*2)%2==0){
        int cx=(int)(x+12+MT(field,FONT_NORMAL));
        DT("|",cx,(int)(y+10),FONT_NORMAL,NEON_CYAN);
    }
    if(CheckCollisionPointRec(GetMousePosition(),{x,y,w,38})&&
       IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) focusField=fieldIdx;
    if(focused) HandleFieldInput(field,len,maxLen);
}

static void DrawSetupScreen(int sw,int sh){
    const char* title=">_ NexOS Chat";
    DT(title,sw/2-MT(title,FONT_TITLE)/2,28,FONT_TITLE,NEON_CYAN);
    const char* sub="Direct TCP chat — no server required";
    DT(sub,sw/2-MT(sub,FONT_SMALL)/2,64,FONT_SMALL,TEXT_MUTED);

    float bw=200,bh=56,gap=16;
    float bx1=sw/2.0f-bw-gap/2, bx2=sw/2.0f+gap/2, by=104;

    auto drawModeBtn=[&](float bx,int mode,const char* top,const char* bot,Color hi){
        bool sel=(modeChoice==mode);
        bool hov=CheckCollisionPointRec(GetMousePosition(),{bx,by,bw,bh});
        Color bg=sel?Color{(unsigned char)(hi.r/5),(unsigned char)(hi.g/5),
                          (unsigned char)(hi.b/5),180}:(hov?BG_HOVER:BG_PANEL);
        DrawRectangleRounded({bx,by,bw,bh},0.18f,8,bg);
        DrawRectangleLinesEx({bx,by,bw,bh},sel?2.0f:1.0f,sel?hi:BORDER_DIM);
        if(sel) DrawGlow((int)bx,(int)by,(int)bw,(int)bh,hi,3.0f);
        DT(top,(int)(bx+bw/2-MT(top,FONT_NORMAL)/2),(int)(by+10),FONT_NORMAL,sel?hi:TEXT_PRIMARY);
        DT(bot,(int)(bx+bw/2-MT(bot,FONT_TINY)/2),(int)(by+34),FONT_TINY,TEXT_MUTED);
        if(hov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
            modeChoice=mode; focusField=(mode==1)?0:1;
        }
    };
    drawModeBtn(bx1,1,"HOST","(you wait for partner)",NEON_CYAN);
    drawModeBtn(bx2,2,"JOIN","(partner started first)",NEON_PINK);

    float fw=360, fx=sw/2.0f-fw/2, fy=200;
    DrawField("Your display name:",myName,myNameLen,31,0,fx,fy,fw);
    fy+=72;
    if(modeChoice==2){
        DrawField("Host IP address:",g_targetIP,targetIPLen,63,1,fx,fy,fw);
        fy+=72;
    }
    DrawField("Port:",g_portBuf,portLen,5,2,fx,fy,fw);
    fy+=72;

    bool canGo=(modeChoice!=0);
    Color btnEdge=canGo?NEON_CYAN:BORDER_DIM;
    bool hovGo=canGo&&CheckCollisionPointRec(GetMousePosition(),{fx,fy,fw,46});
    DrawRectangleRounded({fx,fy,fw,46},0.2f,8,hovGo?Color{0,200,160,50}:Color{0,180,150,20});
    DrawRectangleLinesEx({fx,fy,fw,46},1.5f,btnEdge);
    if(canGo&&hovGo) DrawGlow((int)fx,(int)fy,(int)fw,46,NEON_CYAN,3.0f);
    const char* btnLabel=modeChoice==1?"Start as Host  →":
                         modeChoice==2?"Connect to Host  →":
                                       "Choose HOST or JOIN above";
    DT(btnLabel,(int)(fx+fw/2-MT(btnLabel,FONT_NORMAL)/2),(int)(fy+13),FONT_NORMAL,btnEdge);

    auto doGo=[&](){
        if(!canGo) return;
        if(myNameLen==0){ strcpy(myName,"User"); myNameLen=4; }
        if(modeChoice==1) StartHost();
        else              StartJoin();
    };
    if(hovGo&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) doGo();
    if(IsKeyPressed(KEY_ENTER)) doGo();
    if(IsKeyPressed(KEY_TAB)){
        int maxF=(modeChoice==2)?3:2;
        focusField=(focusField+1)%maxF;
    }

    if(!errMsg.empty())
        DT(errMsg.c_str(),sw/2-MT(errMsg.c_str(),FONT_SMALL)/2,sh-46,FONT_SMALL,NEON_PINK);

    const char* hint="TAB = next field    ENTER = connect    ESC = quit";
    DT(hint,sw/2-MT(hint,FONT_TINY)/2,sh-20,FONT_TINY,TEXT_DIM);
}

// ==========================================================
//  CONNECTING SCREEN
// ==========================================================
static void DrawConnectingScreen(int sw,int sh){
    float t=(float)GetTime();
    for(int i=0;i<12;i++){
        float a=t*3.0f+i*(3.14159f*2.0f/12.0f);
        int px=(int)(sw/2+cosf(a)*28.0f);
        int py=(int)(sh/2-30+sinf(a)*28.0f);
        DrawCircle(px,py,4.5f-(float)i*0.3f,{0,255,200,(unsigned char)(60+i*16)});
    }
    const char* m=modeChoice==1?"Waiting for partner to connect...":"Connecting...";
    DT(m,sw/2-MT(m,FONT_LARGE)/2,sh/2+14,FONT_LARGE,NEON_CYAN);
    {
        std::lock_guard<std::mutex> lk(msgMtx);
        if(!msgs.empty())
            DT(msgs.back().text.c_str(),
               sw/2-MT(msgs.back().text.c_str(),FONT_SMALL)/2,
               sh/2+52,FONT_SMALL,TEXT_MUTED);
    }
    DT("ESC to cancel",sw/2-MT("ESC to cancel",FONT_TINY)/2,sh-24,FONT_TINY,TEXT_DIM);
}

// ==========================================================
//  CHAT SCREEN
// ==========================================================
static void DrawChatScreen(int sw,int sh){
    int barH=44;
    DrawRectangle(0,0,sw,barH,BG_PANEL);
    DrawLineEx({0.0f,(float)barH},{(float)sw,(float)barH},1.0f,BORDER_DIM);

    bool live=(connState.load()==CONNECTED);
    DrawCircle(16,barH/2,6,live?NEON_CYAN:NEON_PINK);
    DT(live?"LIVE":"OFFLINE",28,barH/2-FONT_TINY/2,FONT_TINY,live?NEON_CYAN:NEON_PINK);

    std::string chatTitle=std::string(">_ ")+myName+" — NexOS Chat";
    DT(chatTitle.c_str(),sw/2-MT(chatTitle.c_str(),FONT_NORMAL)/2,
       barH/2-FONT_NORMAL/2,FONT_NORMAL,TEXT_PRIMARY);
    DT("ESC = disconnect",sw-MT("ESC = disconnect",FONT_TINY)-12,
       barH/2-FONT_TINY/2,FONT_TINY,TEXT_DIM);

    int inH=54, inY=sh-inH;
    DrawRectangle(0,inY,sw,inH,BG_PANEL);
    DrawLineEx({0.0f,(float)inY},{(float)sw,(float)inY},1.0f,BORDER_DIM);

    float ibx=14,iby=(float)(inY+9),ibw=(float)(sw-120),ibh=36.0f;
    DrawRectangleRounded({ibx,iby,ibw,ibh},0.15f,8,BG_DEEP);
    DrawRectangleLinesEx({ibx,iby,ibw,ibh},1.0f,live?NEON_CYAN:BORDER_DIM);
    DT(inputBuf,(int)(ibx+10),(int)(iby+9),FONT_NORMAL,TEXT_PRIMARY);
    if(live&&(int)(GetTime()*2)%2==0){
        DT("|",(int)(ibx+10+MT(inputBuf,FONT_NORMAL)),(int)(iby+9),FONT_NORMAL,NEON_CYAN);
    }

    float sbx=(float)(sw-106),sby=(float)(inY+9);
    bool hovSend=live&&CheckCollisionPointRec(GetMousePosition(),{sbx,sby,92,36});
    DrawRectangleRounded({sbx,sby,92,36},0.2f,8,hovSend?Color{0,220,180,70}:Color{0,180,150,30});
    DrawRectangleLinesEx({sbx,sby,92,36},1.0f,live?NEON_CYAN:BORDER_DIM);
    DT("SEND",(int)(sbx+46-MT("SEND",FONT_NORMAL)/2),(int)(sby+9),FONT_NORMAL,live?NEON_CYAN:BORDER_DIM);

    auto doSend=[&](){
        if(inputLen==0||!live) return;
        std::string name(myName), text(inputBuf);
        if(SendLine(name,text)) PushRaw(name,text,true,false);
        else PushSys("Send failed — partner may have disconnected.");
        inputBuf[0]='\0'; inputLen=0;
    };
    if((hovSend&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON))||IsKeyPressed(KEY_ENTER)) doSend();

    if(live){
        int key=GetCharPressed();
        while(key>0){
            if(key>=32&&key<127&&inputLen<MSG_MAX-1){
                inputBuf[inputLen++]=(char)key; inputBuf[inputLen]='\0';
            }
            key=GetCharPressed();
        }
        if(IsKeyPressed(KEY_BACKSPACE)&&inputLen>0) inputBuf[--inputLen]='\0';
    }

    int chatY=barH+4, chatH=inY-chatY-4;
    int lineH=44, visible=chatH/lineH;
    float wheel=GetMouseWheelMove();

    {
        std::lock_guard<std::mutex> lk(msgMtx);
        int total=(int)msgs.size();
        if(wheel!=0.0f){
            scrollOff+=(int)(wheel*-2);
            if(scrollOff<0) scrollOff=0;
            if(scrollOff>total-1) scrollOff=total-1;
        }
        int endIdx=total-scrollOff, startIdx=endIdx-visible;
        if(startIdx<0) startIdx=0;
        if(endIdx>total) endIdx=total;

        for(int i=startIdx;i<endIdx;i++){
            const Msg& m=msgs[i];
            int my=chatY+(i-startIdx)*lineH+4;
            if(m.sys){
                std::string lab="— "+m.text+" —";
                               Color sc=m.hist?TEXT_DIM:TEXT_MUTED;
                DT(lab.c_str(),sw/2-MT(lab.c_str(),FONT_SMALL)/2,my+14,FONT_SMALL,sc);
                continue;
            }
            int textW=MT(m.text.c_str(),FONT_NORMAL);
            int nameW=MT(m.name.c_str(),FONT_TINY);
            int tsW  =MT(m.ts.c_str(),FONT_TINY);
            int bubW =std::min(std::max(textW,nameW)+24,sw-80);
                        Color txtCol  = m.hist ? TEXT_MUTED   : TEXT_PRIMARY;
            Color metaCol = m.hist ? TEXT_DIM      : TEXT_DIM;
            if(m.mine){
                int bx=sw-bubW-16;
                                Color fill = m.hist ? Color{0,50,40,80}  : Color{0,100,80,130};
                Color edge = m.hist ? Color{0,120,100,60}: Color{0,220,180,110};
                DrawRectangleRounded({(float)bx,(float)(my+2),(float)bubW,36},0.2f,8,fill);
                DrawRectangleLinesEx({(float)bx,(float)(my+2),(float)bubW,36},1.0f,edge);
                DT(m.text.c_str(),bx+10,my+8,FONT_NORMAL,txtCol);
                DT(m.name.c_str(),bx+10,my+28,FONT_TINY,metaCol);
                DT(m.ts.c_str(),bx+bubW-tsW-8,my+28,FONT_TINY,metaCol);
            } else {
                               Color fill = m.hist ? Color{25,0,45,80}   : Color{60,0,100,130};
                Color edge = m.hist ? Color{100,30,140,60} : Color{180,60,220,110};
                DrawRectangleRounded({16.0f,(float)(my+2),(float)bubW,36},0.2f,8,fill);
                DrawRectangleLinesEx({16.0f,(float)(my+2),(float)bubW,36},1.0f,edge);
                DT(m.text.c_str(),26,my+8,FONT_NORMAL,txtCol);
                DT(m.name.c_str(),26,my+28,FONT_TINY,metaCol);
                DT(m.ts.c_str(),bubW+22,my+28,FONT_TINY,metaCol);
            }
        }
        if(scrollOff>0){
            char sb[24]; snprintf(sb,sizeof(sb),"^ %d older",scrollOff);
            DT(sb,sw-MT(sb,FONT_TINY)-12,barH+8,FONT_TINY,NEON_GOLD);
        }
    }
}

// ==========================================================
//  DISCONNECTED SCREEN
// ==========================================================
static void DrawDisconnectedScreen(int sw,int sh){
    DT("Connection lost",sw/2-MT("Connection lost",FONT_LARGE)/2,sh/2-28,FONT_LARGE,NEON_PINK);
    DT("Press ESC to return to the setup screen",
       sw/2-MT("Press ESC to return to the setup screen",FONT_NORMAL)/2,
       sh/2+18,FONT_NORMAL,TEXT_MUTED);
}

// ==========================================================
//  MAIN
// ==========================================================
int main(){
    if(!RequestResources(APP_NAME,RAM_MB,HDD_MB,PRIORITY_NORMAL,1)){
        InitWindow(440,120,APP_NAME" - Denied");
        SetTargetFPS(30);
        double t=GetTime();
        while(!WindowShouldClose()&&GetTime()-t<3.5){
            BeginDrawing(); ClearBackground(BG_DEEP);
            DrawText("Insufficient resources.",20,40,FONT_NORMAL,NEON_PINK);
            EndDrawing();
        }
        CloseWindow(); return 1;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(WIN_W,WIN_H,"NexOS Chat");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    SetWindowFocused();

    gFontOK=false;
    if(FileExists("assets/fonts/DejaVuSans-Bold.ttf")){
        gFont=LoadFontEx("assets/fonts/DejaVuSans-Bold.ttf",20,nullptr,0);
        gFontOK=(gFont.texture.id>0);
        if(gFontOK) SetTextureFilter(gFont.texture,TEXTURE_FILTER_BILINEAR);
    }
    mkdir("hdd",0755);
    LoadHistory();
    while(!WindowShouldClose()&&appRunning){
        int sw=GetScreenWidth(), sh=GetScreenHeight();
        ConnState cs=connState.load();

        if(IsKeyPressed(KEY_ESCAPE)){
            if(cs==SETUP) appRunning=false;
            else          ResetToSetup();
        }
        if(cs==CONN_ERR) connState.store(SETUP);

        BeginDrawing();
        ClearBackground(BG_DEEP);
        DrawCyberpunkGrid(sw,sh);

        cs=connState.load();
        switch(cs){
            case SETUP:        DrawSetupScreen(sw,sh);        break;
            case CONNECTING:   DrawConnectingScreen(sw,sh);   break;
            case CONNECTED:    DrawChatScreen(sw,sh);         break;
            case DISCONNECTED: DrawDisconnectedScreen(sw,sh); break;
            default: break;
        }
        EndDrawing();
    }

    appRunning=false;
    if(peerFd>=0)   close(peerFd);
    if(listenFd>=0) close(listenFd);
    if(gFontOK)     UnloadFont(gFont);
    ReleaseResources(APP_NAME,RAM_MB,HDD_MB);
    CloseWindow();
    return 0;
}