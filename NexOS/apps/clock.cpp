
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
#include <sstream>
#include <algorithm>

#define APP_NAME  "Clock"
#define RAM_MB    20
#define HDD_MB    1
#define WIN_W     900
#define WIN_H     640

// ── State path ────────────────────────────────────────────
static const char* STATE_PATH = "hdd/clock_state.txt";

// ── Day names ─────────────────────────────────────────────
static const int   DAYS = 7;
static const char* DAY_NAMES[DAYS] = {"S","M","T","W","T","F","S"};

// ── Structs ───────────────────────────────────────────────
struct WorldClock { std::string name; int utcOffsetMinutes; };

struct Alarm {
    int    hour=7, minute=0;
    bool   enabled=true;
    int    repeatMask=0;
    int    snoozeMinutes=10;
    std::string label="Alarm";
    bool   ringing=false;
    int    lastFireKey=-1;
    double snoozeUntil=0.0;
};

struct TimerState {
    bool   running=false, finished=false;
    double durationSeconds=300.0;
    double remainingSeconds=300.0;
    double startAt=0.0;
};

struct StopwatchState {
    bool   running=false;
    double elapsedSeconds=0.0;
    double startAt=0.0;
    std::vector<double> laps;
};

// ── Global state ──────────────────────────────────────────
static std::vector<WorldClock>   worldClocks;
static std::vector<Alarm>        alarms;
static TimerState                timerState;
static StopwatchState            swState;
static int                       currentTab   = 0;
static bool                      appRunning   = true;
static bool                      stateDirty   = false;

// ── Modal flags ───────────────────────────────────────────
static bool addCityModal   = false;
static bool addAlarmModal  = false;
static int  editAlarmIdx   = -1;
static int  activeRingIdx  = -1;

// ── Modal draft values ────────────────────────────────────
static std::string citySearch    = "";
static std::string alarmLabel    = "Alarm";
static int  modalHour   = 7;
static int  modalMinute = 0;
static int  modalSnooze = 10;
static int  modalRepeat = 0;

// ── Focus tracking ────────────────────────────────────────
static bool searchFocused = false;
static bool labelFocused  = false;

// ── Audio ─────────────────────────────────────────────────
static Sound  alarmTone   = {0};
static bool   toneLoaded  = false;
static double lastBeepAt  = 0.0;

// ── Notification ──────────────────────────────────────────
static bool showNotif = false;
static int  notifIdx  = -1; // -1=none  -2=timer  >=0=alarm index

// ── Background thread ─────────────────────────────────────
static volatile bool bgRunning = true;
static pthread_t     bgThread;
static pthread_mutex_t bgMutex = PTHREAD_MUTEX_INITIALIZER;

// ============================================================
//  City database
// ============================================================
static const struct CityInfo { const char* name; int utcMin; } CITY_DB[] = {
    {"Local Time",    0},  {"London",        0},  {"Paris",        60},
    {"Berlin",       60},  {"Rome",          60},  {"Cairo",       120},
    {"Dubai",       240},  {"Karachi",      300},  {"Lahore",      300},
    {"Delhi",       330},  {"Bangkok",      420},  {"Singapore",   480},
    {"Tokyo",       540},  {"Sydney",       600},  {"Honolulu",   -600},
    {"Los Angeles",-480},  {"Denver",      -420},  {"Chicago",    -360},
    {"New York",   -300},  {"Toronto",     -300},  {"Sao Paulo",  -180},
};
static const int CITY_COUNT = 21;

// ============================================================
//  Helpers
// ============================================================
static int CurrentUtcOffset() {
    time_t now=time(nullptr);
    tm lt=*localtime(&now), gt=*gmtime(&now);
    return (int)difftime(mktime(&lt),mktime(&gt))/60;
}

static std::string FmtTime(time_t t, const char* fmt="%H:%M:%S") {
    char buf[64]; strftime(buf,sizeof(buf),fmt,localtime(&t));
    return std::string(buf);
}

static std::string FmtDuration(double sec) {
    int s=(int)std::max(0.0,round(sec));
    int h=s/3600;s%=3600;int m=s/60,ss=s%60;
    char buf[32];
    if(h>0)snprintf(buf,sizeof(buf),"%02d:%02d:%02d",h,m,ss);
    else   snprintf(buf,sizeof(buf),"%02d:%02d",m,ss);
    return std::string(buf);
}

static std::string FmtShortMin(int min) {
    int h=min/60,m=min%60;
    char buf[16];
    if(h>0&&m>0)snprintf(buf,sizeof(buf),"%dh %dm",h,m);
    else if(h>0)snprintf(buf,sizeof(buf),"%dh",h);
    else        snprintf(buf,sizeof(buf),"%dm",m);
    return std::string(buf);
}

static std::string RepeatStr(int mask) {
    if(mask==0)return "Once";
    std::string s;
    for(int i=0;i<DAYS;i++)if(mask&(1<<i)){if(!s.empty())s+=" ";s+=DAY_NAMES[i];}
    return s;
}

static std::string OffsetStr(int min) {
    char buf[20];snprintf(buf,sizeof(buf),"UTC%+d:%02d",min/60,abs(min%60));
    return std::string(buf);
}

static bool ContainsI(const std::string& a,const std::string& b) {
    std::string la=a,lb=b;
    for(auto&c:la)c=tolower(c);
    for(auto&c:lb)c=tolower(c);
    return la.find(lb)!=std::string::npos;
}

static void TextInput(std::string& s,int maxLen,bool focused,bool spaces=true) {
    if(!focused)return;
    int k=GetCharPressed();
    while(k>0){
        bool ok=(k>='a'&&k<='z')||(k>='A'&&k<='Z')||(k>='0'&&k<='9')||k=='-'||k=='_';
        if(spaces&&k==' ')ok=true;
        if(ok&&(int)s.size()<maxLen)s.push_back((char)k);
        k=GetCharPressed();
    }
    if(IsKeyPressed(KEY_BACKSPACE)&&!s.empty())s.pop_back();
}

// ============================================================
//  Audio
// ============================================================
static void InitTone() {
    if(toneLoaded)return;
    if(!IsAudioDeviceReady())InitAudioDevice();
    if(!IsAudioDeviceReady())return;
    const int SR=44100; const float SEC=0.35f; const float FREQ=880.0f;
    int n=(int)(SR*SEC);
    short* s=(short*)MemAlloc(n*sizeof(short));
    if(!s)return;
    for(int i=0;i<n;i++){float t=(float)i/SR,e=0.8f*(1.0f-t/SEC);if(e<0.15f)e=0.15f;s[i]=(short)(sinf(2*PI*FREQ*t)*30000*e);}
    Wave w={0};w.frameCount=n;w.sampleRate=SR;w.sampleSize=16;w.channels=1;w.data=s;
    alarmTone=LoadSoundFromWave(w);UnloadWave(w);
    if(alarmTone.stream.buffer){SetSoundVolume(alarmTone,0.65f);toneLoaded=true;}
}

static void PlayTone(){if(!toneLoaded)InitTone();if(toneLoaded)PlaySound(alarmTone);}

// ============================================================
//  Persist state
// ============================================================
static std::vector<std::string> Split(const std::string& s,char d){
    std::vector<std::string>v;std::stringstream ss(s);std::string t;
    while(std::getline(ss,t,d))v.push_back(t);return v;
}

static void SaveState(){
    mkdir("hdd",0755);
    FILE*f=fopen(STATE_PATH,"w");if(!f)return;
    fprintf(f,"WORLD %zu\n",worldClocks.size());
    for(auto&w:worldClocks)fprintf(f,"%s|%d\n",w.name.c_str(),w.utcOffsetMinutes);
    fprintf(f,"ALARMS %zu\n",alarms.size());
    for(auto&a:alarms)fprintf(f,"%d|%d|%d|%d|%d|%s\n",a.hour,a.minute,a.enabled?1:0,a.repeatMask,a.snoozeMinutes,a.label.c_str());
    fprintf(f,"TIMER %d|%.3f|%.3f|%d\n",timerState.running?1:0,timerState.durationSeconds,timerState.remainingSeconds,timerState.finished?1:0);
    fprintf(f,"SW %d|%.3f|%zu\n",swState.running?1:0,swState.elapsedSeconds,swState.laps.size());
    for(double lap:swState.laps)fprintf(f,"LAP|%.3f\n",lap);
    fclose(f);stateDirty=false;
}

static void LoadState(){
    FILE*f=fopen(STATE_PATH,"r");if(!f)return;
    worldClocks.clear();alarms.clear();swState.laps.clear();
    char line[512];
    enum class S{None,World,Alarm,Timer,SW}sec=S::None;
    while(fgets(line,sizeof(line),f)){
        std::string s(line);
        while(!s.empty()&&(s.back()=='\n'||s.back()=='\r'))s.pop_back();
        if(s.substr(0,6)=="WORLD ")  {sec=S::World;continue;}
        if(s.substr(0,7)=="ALARMS ") {sec=S::Alarm;continue;}
        if(s.substr(0,6)=="TIMER ")  {sec=S::Timer;continue;}
        if(s.substr(0,3)=="SW ")     {sec=S::SW;continue;}
        auto p=Split(s,'|');
        if(s.substr(0,4)=="LAP|"&&p.size()>=2){swState.laps.push_back(atof(p[1].c_str()));continue;}
        if(s.empty())continue;
        if(sec==S::World&&p.size()>=2) worldClocks.push_back({p[0],atoi(p[1].c_str())});
        else if(sec==S::Alarm&&p.size()>=6){Alarm a;a.hour=atoi(p[0].c_str());a.minute=atoi(p[1].c_str());a.enabled=atoi(p[2].c_str());a.repeatMask=atoi(p[3].c_str());a.snoozeMinutes=atoi(p[4].c_str());a.label=p[5];alarms.push_back(a);}
        else if(sec==S::Timer&&p.size()>=4){timerState.running=atoi(p[0].c_str());timerState.durationSeconds=atof(p[1].c_str());timerState.remainingSeconds=atof(p[2].c_str());timerState.finished=atoi(p[3].c_str());}
        else if(sec==S::SW&&p.size()>=3){swState.running=atoi(p[0].c_str());swState.elapsedSeconds=atof(p[1].c_str());}
    }
    fclose(f);
}

static void EnsureDefaults(){
    if(worldClocks.empty()){
        worldClocks.push_back({"Local Time",CurrentUtcOffset()});
        worldClocks.push_back({"London",0});
        worldClocks.push_back({"Lahore",300});
    }
    if(alarms.empty()){Alarm a;a.hour=7;a.minute=30;a.label="Wake up";alarms.push_back(a);}
}

// ============================================================
//  Alarm trigger check
// ============================================================
static void CheckAlarms(){
    time_t now=time(nullptr);
    tm lt=*localtime(&now);
    int weekday=lt.tm_wday;
    int key=(lt.tm_year+1900)*1440*366+lt.tm_yday*1440+lt.tm_hour*60+lt.tm_min;
    for(int i=0;i<(int)alarms.size();i++){
        Alarm&a=alarms[i];
        if(!a.enabled)continue;
        if(a.snoozeUntil>0.0&&GetTime()<a.snoozeUntil)continue;
        if(a.snoozeUntil>0.0&&GetTime()>=a.snoozeUntil)a.snoozeUntil=0.0;
        bool timeMatch=(lt.tm_hour==a.hour&&lt.tm_min==a.minute&&lt.tm_sec==0);
        bool repMatch=(a.repeatMask==0)||(a.repeatMask&(1<<weekday));
        if(timeMatch&&repMatch&&a.lastFireKey!=key){
            a.lastFireKey=key;a.ringing=true;activeRingIdx=i;
            if(!showNotif){showNotif=true;notifIdx=i;}
            stateDirty=true;
        }
    }
}

// ============================================================
//  Background thread — alarm checks + auto-save
// ============================================================
static void* BgThreadFn(void*){
    double lastSave=0;
    while(bgRunning){
        usleep(500000); // check every 0.5s
        pthread_mutex_lock(&bgMutex);
        CheckAlarms();
        // Timer finish check
        if(timerState.running){
            double rem=timerState.durationSeconds-(GetTime()-timerState.startAt);
            if(rem<=0){timerState.running=false;timerState.finished=true;timerState.remainingSeconds=0;if(!showNotif){showNotif=true;notifIdx=-2;}stateDirty=true;}
        }
        // Beep while notification showing
        if(showNotif&&GetTime()-lastBeepAt>0.8){PlayTone();lastBeepAt=GetTime();}
        // Auto-save every 10s
        if(stateDirty&&GetTime()-lastSave>10){SaveState();lastSave=GetTime();}
        pthread_mutex_unlock(&bgMutex);
    }
    return nullptr;
}

// ============================================================
//  UI: Toggle button
// ============================================================
static bool DrawToggle(Rectangle r,bool val){
    bool hov=CheckCollisionPointRec(GetMousePosition(),r);
    DrawRectangleRounded(r,0.45f,10,val?NEON_GREEN:BG_PANEL);
    DrawRectangleLinesEx(r,1.0f,hov?NEON_CYAN:BORDER_DIM);
    float kx=val?(r.x+r.width-r.height+2):(r.x+2);
    DrawCircle((int)(kx+(r.height-4)/2),(int)(r.y+r.height/2),(r.height-6)/2,RAYWHITE);
    return hov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
}

// ============================================================
//  UI: Top tab bar
// ============================================================
static void DrawTabs(int sw){
    const char* labels[]={"World","Alarms","Stopwatch","Timer"};
    int tabW=120,gap=10,total=tabW*4+gap*3,x=(sw-total)/2;
    for(int i=0;i<4;i++){
        Rectangle r={(float)(x+i*(tabW+gap)),10,(float)tabW,32};
        bool act=(i==currentTab);
        DrawRectangleRounded(r,0.3f,8,act?BG_PANEL:BG_PANEL);
        DrawRectangleLinesEx(r,1.0f,act?NEON_CYAN:BORDER_DIM);
        if(act)DrawLine((int)r.x,(int)(r.y+r.height-2),(int)(r.x+r.width),(int)(r.y+r.height-2),NEON_CYAN);
        int tw=MeasureText(labels[i],FONT_SMALL);
        DrawText(labels[i],(int)(r.x+(r.width-tw)/2),(int)(r.y+8),FONT_SMALL,act?NEON_CYAN:TEXT_MUTED);
        if(CheckCollisionPointRec(GetMousePosition(),r)&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON))currentTab=i;
    }
}

// ============================================================
//  WORLD TAB
// ============================================================
static void DrawWorldTab(Rectangle c,int sw){
    time_t now=time(nullptr);
    // Big clock
    std::string ts=FmtTime(now,"%H:%M:%S");
    int tw=MeasureText(ts.c_str(),FONT_TITLE);
    DrawText(ts.c_str(),(int)(c.x+(c.width-tw)/2),(int)c.y+18,FONT_TITLE,NEON_CYAN);
    std::string ds=FmtTime(now,"%A, %B %d, %Y");
    int dw=MeasureText(ds.c_str(),FONT_SMALL);
    DrawText(ds.c_str(),(int)(c.x+(c.width-dw)/2),(int)c.y+68,FONT_SMALL,TEXT_MUTED);

    // World clocks list
    Rectangle list={c.x+30,c.y+110,c.width-60,c.height-130};
    DrawRectangleRounded(list,0.08f,8,BG_PANEL);
    DrawRectangleLinesEx(list,1.0f,BORDER_DIM);
    DrawText("World Clocks",(int)list.x+14,(int)list.y+10,FONT_SMALL,TEXT_MUTED);
    if(DrawButton({list.x+list.width-108,list.y+8,94,24},"+ Add City",BG_PANEL,NEON_CYAN,FONT_TINY)){addCityModal=true;citySearch="";searchFocused=true;}

    int ry=(int)list.y+42;
    int utcOff=CurrentUtcOffset();
    for(int i=0;i<(int)worldClocks.size();i++){
        Rectangle row={list.x+10,(float)ry,list.width-20,50};
        DrawRectangleRounded(row,0.12f,8,BG_PANEL);
        DrawRectangleLinesEx(row,1.0f,BORDER_DIM);
        DrawText(worldClocks[i].name.c_str(),(int)row.x+12,(int)row.y+8,FONT_NORMAL,TEXT_PRIMARY);
        DrawText(OffsetStr(worldClocks[i].utcOffsetMinutes).c_str(),(int)row.x+12,(int)row.y+28,FONT_TINY,TEXT_MUTED);
        time_t ct=now+(worldClocks[i].utcOffsetMinutes-utcOff)*60;
        std::string ct2=FmtTime(ct,"%H:%M");
        int ctw=MeasureText(ct2.c_str(),FONT_LARGE);
        DrawText(ct2.c_str(),(int)(row.x+row.width-ctw-44),(int)row.y+12,FONT_LARGE,NEON_GOLD);
        if(DrawButton({row.x+row.width-30,row.y+14,22,22},"x",BG_DEEP,NEON_PINK,FONT_TINY)&&worldClocks.size()>1){worldClocks.erase(worldClocks.begin()+i);stateDirty=true;break;}
        ry+=58;
    }
}

// ============================================================
//  ALARMS TAB
// ============================================================
static void DrawAlarmsTab(Rectangle c){
    // Ringing banner
    if(activeRingIdx>=0&&activeRingIdx<(int)alarms.size()&&alarms[activeRingIdx].ringing){
        Alarm&a=alarms[activeRingIdx];
        Rectangle ban={c.x+20,c.y+10,c.width-40,66};
        DrawRectangleRounded(ban,0.1f,8,{50,18,18,255});
        DrawRectangleLinesEx(ban,1.5f,NEON_PINK);
        char msg[64];snprintf(msg,sizeof(msg),"RINGING: %02d:%02d  %s",a.hour,a.minute,a.label.c_str());
        DrawText(msg,(int)ban.x+14,(int)ban.y+10,FONT_NORMAL,NEON_PINK);
        if(DrawButton({ban.x+ban.width-200,ban.y+22,90,26},"Snooze",BG_HOVER,NEON_GOLD,FONT_SMALL)){a.ringing=false;a.snoozeUntil=GetTime()+a.snoozeMinutes*60.0;activeRingIdx=-1;showNotif=false;stateDirty=true;}
        if(DrawButton({ban.x+ban.width-100,ban.y+22,86,26},"Stop",BG_HOVER,NEON_PINK,FONT_SMALL)){a.ringing=false;if(!a.repeatMask)a.enabled=false;activeRingIdx=-1;showNotif=false;stateDirty=true;}
    }

    Rectangle list={c.x+30,c.y+90,c.width-60,c.height-110};
    DrawRectangleRounded(list,0.08f,8,BG_HOVER);
    DrawRectangleLinesEx(list,1.0f,BORDER_DIM);
    DrawText("Alarms",(int)list.x+14,(int)list.y+10,FONT_SMALL,TEXT_MUTED);
    if(DrawButton({list.x+list.width-118,list.y+8,104,24},"+ New Alarm",BG_HOVER,NEON_CYAN,FONT_TINY)){editAlarmIdx=-1;modalHour=7;modalMinute=0;modalSnooze=10;modalRepeat=0;alarmLabel="Alarm";addAlarmModal=true;labelFocused=true;}

    int ry=(int)list.y+44;
    for(int i=0;i<(int)alarms.size();i++){
        Alarm&a=alarms[i];
        Rectangle row={list.x+10,(float)ry,list.width-20,60};
        DrawRectangleRounded(row,0.1f,8,a.ringing?Color{50,18,18,255}:BG_PANEL);
        DrawRectangleLinesEx(row,1.0f,a.ringing?NEON_PINK:BORDER_DIM);
        char tbuf[8];snprintf(tbuf,sizeof(tbuf),"%02d:%02d",a.hour,a.minute);
        DrawText(tbuf,(int)row.x+12,(int)row.y+8,FONT_TITLE,TEXT_PRIMARY);
        DrawText(a.label.c_str(),(int)row.x+12,(int)row.y+42,FONT_SMALL,TEXT_MUTED);
        DrawText(RepeatStr(a.repeatMask).c_str(),(int)row.x+200,(int)row.y+44,FONT_TINY,TEXT_DIM);
        if(DrawButton({row.x+row.width-154,row.y+18,44,26},"Edit",BG_HOVER,NEON_CYAN,FONT_TINY)){editAlarmIdx=i;modalHour=a.hour;modalMinute=a.minute;modalSnooze=a.snoozeMinutes;modalRepeat=a.repeatMask;alarmLabel=a.label;addAlarmModal=true;labelFocused=false;}
        if(DrawButton({row.x+row.width-102,row.y+18,44,26},"Del",BG_HOVER,NEON_PINK,FONT_TINY)){alarms.erase(alarms.begin()+i);stateDirty=true;break;}
        if(DrawToggle({row.x+row.width-50,row.y+20,40,22},a.enabled)){a.enabled=!a.enabled;stateDirty=true;}
        ry+=68;
    }
}

// ============================================================
//  STOPWATCH TAB
// ============================================================
static void DrawStopwatchTab(Rectangle c){
    double disp=swState.elapsedSeconds;
    if(swState.running)disp+=GetTime()-swState.startAt;
    std::string s=FmtDuration(disp);
    int sw=MeasureText(s.c_str(),FONT_TITLE);
    DrawText(s.c_str(),(int)(c.x+(c.width-sw)/2),(int)c.y+30,FONT_TITLE,NEON_CYAN);

    float btnY=c.y+100,btnCX=c.x+c.width/2;
    if(DrawButton({btnCX-180,btnY,110,34},swState.running?"Pause":"Start",BG_HOVER,swState.running?NEON_ORANGE:NEON_GREEN,FONT_SMALL)){
        if(swState.running){swState.elapsedSeconds+=GetTime()-swState.startAt;swState.running=false;}
        else{swState.startAt=GetTime();swState.running=true;}
        stateDirty=true;
    }
    if(DrawButton({btnCX-60,btnY,100,34},"Lap",BG_HOVER,NEON_CYAN,FONT_SMALL)){if(disp>0.01)swState.laps.push_back(disp);stateDirty=true;}
    if(DrawButton({btnCX+50,btnY,110,34},"Reset",BG_HOVER,NEON_PINK,FONT_SMALL)){swState.running=false;swState.elapsedSeconds=0;swState.laps.clear();stateDirty=true;}

    Rectangle la={c.x+40,c.y+152,c.width-80,c.height-175};
    DrawRectangleRounded(la,0.1f,8,BG_HOVER);DrawRectangleLinesEx(la,1.0f,BORDER_DIM);
    DrawText("Laps",(int)la.x+14,(int)la.y+10,FONT_SMALL,TEXT_MUTED);
    int ry=(int)la.y+40;
    for(int i=(int)swState.laps.size()-1;i>=0&&ry<(int)(la.y+la.height-20);i--){
        char lbl[16];snprintf(lbl,sizeof(lbl),"Lap %d",(int)swState.laps.size()-i);
        DrawText(lbl,(int)la.x+14,ry,FONT_SMALL,TEXT_MUTED);
        std::string lt=FmtDuration(swState.laps[i]);
        int lw=MeasureText(lt.c_str(),FONT_NORMAL);
        DrawText(lt.c_str(),(int)(la.x+la.width-lw-14),ry,FONT_NORMAL,TEXT_PRIMARY);
        ry+=28;
    }
}

// ============================================================
//  TIMER TAB
// ============================================================
static void DrawTimerTab(Rectangle c){
    double rem=timerState.remainingSeconds;
    if(timerState.running)rem=std::max(0.0,timerState.durationSeconds-(GetTime()-timerState.startAt));

    std::string ts=FmtDuration(rem);
    int tw=MeasureText(ts.c_str(),FONT_TITLE);
    DrawText(ts.c_str(),(int)(c.x+(c.width-tw)/2),(int)c.y+22,FONT_TITLE,timerState.finished?NEON_PINK:NEON_ORANGE);

    // Quick presets
    const int pv[]={60,120,180,300,600,900,1800,3600};
    const char* pl[]={"1m","2m","3m","5m","10m","15m","30m","1h"};
    float qx=c.x+(c.width-8*70-7*8)/2,qy=c.y+100;
    for(int i=0;i<8;i++){
        Rectangle b={qx+i*78,(float)qy,66,26};
        if(DrawButton(b,pl[i],BG_HOVER,TEXT_MUTED,FONT_SMALL)){timerState.durationSeconds=pv[i];timerState.remainingSeconds=pv[i];timerState.running=false;timerState.finished=false;stateDirty=true;}
    }

    // H/M/S steppers
    int h=(int)timerState.durationSeconds/3600,m=((int)timerState.durationSeconds%3600)/60,s=(int)timerState.durationSeconds%60;
    float sx=c.x+(c.width-380)/2,sy=c.y+142;
    auto stepper=[&](float x,const char* lbl,int val)->int{
        Rectangle up={x,sy,90,24},dn={x,sy+52,90,24},disp={x,sy+24,90,28};
        if(DrawButton(up,"▲",BG_HOVER,NEON_CYAN,FONT_SMALL))return 1;
        DrawRectangleRounded(disp,0.1f,8,BG_PANEL);
        DrawRectangleLinesEx(disp,1.0f,BORDER_DIM);
        char buf[8];snprintf(buf,sizeof(buf),"%02d",val);
        int bw=MeasureText(buf,FONT_LARGE);
        DrawText(buf,(int)(disp.x+(disp.width-bw)/2),(int)disp.y+4,FONT_LARGE,TEXT_PRIMARY);
        DrawText(lbl,(int)x+2,(int)(sy+80),FONT_TINY,TEXT_DIM);
        if(DrawButton(dn,"▼",BG_HOVER,NEON_CYAN,FONT_SMALL))return -1;
        return 0;
    };
    int hd=stepper(sx,"Hours",h),md=stepper(sx+130,"Minutes",m),sd=stepper(sx+260,"Seconds",s);
    if(hd||md||sd){
        int tot=std::max(1,h*3600+m*60+s+hd*3600+md*60+sd);
        timerState.durationSeconds=tot;timerState.remainingSeconds=tot;
        timerState.running=false;timerState.finished=false;stateDirty=true;
    }

    // Controls
    float by=c.y+c.height-70,bcx=c.x+c.width/2;
    if(DrawButton({bcx-180,by,110,34},timerState.running?"Running":"Start",BG_HOVER,timerState.running?NEON_GREEN:NEON_CYAN,FONT_SMALL)){
        if(!timerState.running){if(timerState.remainingSeconds<=0)timerState.remainingSeconds=timerState.durationSeconds;timerState.startAt=GetTime();timerState.running=true;timerState.finished=false;stateDirty=true;}
    }
    if(DrawButton({bcx-60,by,110,34},"Pause",BG_HOVER,NEON_ORANGE,FONT_SMALL)){
        if(timerState.running){timerState.remainingSeconds=std::max(0.0,timerState.durationSeconds-(GetTime()-timerState.startAt));timerState.running=false;stateDirty=true;}
    }
    if(DrawButton({bcx+60,by,110,34},"Reset",BG_HOVER,NEON_PINK,FONT_SMALL)){timerState.running=false;timerState.finished=false;timerState.remainingSeconds=timerState.durationSeconds;stateDirty=true;}

    if(timerState.finished){
        Rectangle done={c.x+60,by-46,c.width-120,36};
        DrawRectangleRounded(done,0.15f,8,{40,15,15,255});
        DrawRectangleLinesEx(done,1.0f,NEON_PINK);
        DrawText("Timer complete!",(int)done.x+14,(int)done.y+9,FONT_SMALL,NEON_PINK);
        if(DrawButton({done.x+done.width-100,done.y+6,88,24},"Dismiss",BG_HOVER,NEON_PINK,FONT_TINY))timerState.finished=false;
    }
}

// ============================================================
//  ADD CITY MODAL
// ============================================================
static void DrawAddCityModal(int sw,int sh){
    DrawRectangle(0,0,sw,sh,{0,0,0,190});
    int pw=660,ph=400,px=(sw-pw)/2,py=(sh-ph)/2;
    DrawRectangle(px,py,pw,ph,BG_PANEL);
    DrawGlowRect({(float)px,(float)py,(float)pw,(float)ph},NEON_CYAN,5);
    DrawText("Add City",px+16,py+14,FONT_LARGE,NEON_CYAN);
    DrawLine(px,py+40,px+pw,py+40,BORDER_DIM);

    // Search box
    Rectangle sbox={px+16.0f,py+50.0f,pw-32.0f,32.0f};
    DrawRectangleRec(sbox,BG_DEEP);
    DrawRectangleLinesEx(sbox,1.5f,searchFocused?NEON_CYAN:BORDER_DIM);
    DrawText(citySearch.c_str(),(int)sbox.x+8,(int)sbox.y+8,FONT_SMALL,TEXT_PRIMARY);
    if(searchFocused&&(int)(GetTime()*2)%2==0){int cw=MeasureText(citySearch.c_str(),FONT_SMALL);DrawText("|",(int)sbox.x+10+cw,(int)sbox.y+8,FONT_SMALL,NEON_CYAN);}
    if(IsMouseButtonPressed(MOUSE_LEFT_BUTTON)&&CheckCollisionPointRec(GetMousePosition(),sbox))searchFocused=true;
    TextInput(citySearch,24,searchFocused);

    // City list
    int ry=py+96;
    for(int i=0;i<CITY_COUNT;i++){
        if(!ContainsI(CITY_DB[i].name,citySearch))continue;
        Rectangle row={(float)(px+12),(float)ry,(float)(pw-24),30};
        bool hov=CheckCollisionPointRec(GetMousePosition(),row);
        if(hov)DrawRectangleRec(row,BG_HOVER);
        DrawText(CITY_DB[i].name,(int)row.x+10,(int)row.y+7,FONT_SMALL,TEXT_PRIMARY);
        DrawText(OffsetStr(CITY_DB[i].utcMin).c_str(),(int)(row.x+row.width-120),(int)row.y+7,FONT_TINY,TEXT_MUTED);
        if(DrawButton({row.x+row.width-58,row.y+4,50,22},"Add",hov?NEON_CYAN:BG_HOVER,hov?BG_DEEP:TEXT_PRIMARY,FONT_TINY)){
            worldClocks.push_back({CITY_DB[i].name,CITY_DB[i].utcMin});
            stateDirty=true;addCityModal=false;searchFocused=false;break;
        }
        ry+=36;
        if(ry>py+ph-50)break;
    }

    if(DrawButton({(float)(px+pw-96),(float)(py+ph-40),86,28},"Cancel",BG_HOVER,NEON_PINK,FONT_SMALL)){addCityModal=false;searchFocused=false;}
    if(IsKeyPressed(KEY_ESCAPE)){addCityModal=false;searchFocused=false;}
}

// ============================================================
//  ALARM EDITOR MODAL
// ============================================================
static void DrawAlarmModal(int sw,int sh){
    DrawRectangle(0,0,sw,sh,{0,0,0,195});
    int pw=640,ph=440,px=(sw-pw)/2,py=(sh-ph)/2;
    DrawRectangle(px,py,pw,ph,BG_PANEL);
    DrawGlowRect({(float)px,(float)py,(float)pw,(float)ph},NEON_CYAN,5);
    DrawText(editAlarmIdx>=0?"Edit Alarm":"New Alarm",px+16,py+12,FONT_LARGE,NEON_CYAN);
    DrawLine(px,py+40,px+pw,py+40,BORDER_DIM);

    // Time picker
    DrawText("Time",(int)(px+16),(int)(py+52),FONT_SMALL,TEXT_MUTED);
    float tcx=px+(pw-260)/2,tcy=py+80;
    // Hour stepper
    if(DrawButton({tcx,tcy,54,26},"  +",BG_HOVER,NEON_CYAN,FONT_SMALL))modalHour=(modalHour+1)%24;
    Rectangle hbox={tcx,tcy+28,54,40};
    DrawRectangleRounded(hbox,0.1f,8,BG_DEEP);DrawRectangleLinesEx(hbox,1.0f,BORDER_DIM);
    char hb[4];snprintf(hb,sizeof(hb),"%02d",modalHour);
    int hbw=MeasureText(hb,FONT_LARGE);DrawText(hb,(int)(hbox.x+(hbox.width-hbw)/2),(int)hbox.y+8,FONT_LARGE,NEON_GOLD);
    if(DrawButton({tcx,tcy+70,54,26},"  -",BG_HOVER,NEON_CYAN,FONT_SMALL))modalHour=(modalHour+23)%24;
    // Colon
    DrawText(":",(int)(tcx+62),(int)(tcy+42),FONT_TITLE,TEXT_PRIMARY);
    // Minute stepper
    float mx2=tcx+80;
    if(DrawButton({mx2,tcy,54,26},"  +",BG_HOVER,NEON_CYAN,FONT_SMALL))modalMinute=(modalMinute+1)%60;
    Rectangle mbox={mx2,tcy+28,54,40};
    DrawRectangleRounded(mbox,0.1f,8,BG_DEEP);DrawRectangleLinesEx(mbox,1.0f,BORDER_DIM);
    char mb[4];snprintf(mb,sizeof(mb),"%02d",modalMinute);
    int mbw=MeasureText(mb,FONT_LARGE);DrawText(mb,(int)(mbox.x+(mbox.width-mbw)/2),(int)mbox.y+8,FONT_LARGE,NEON_GOLD);
    if(DrawButton({mx2,tcy+70,54,26},"  -",BG_HOVER,NEON_CYAN,FONT_SMALL))modalMinute=(modalMinute+59)%60;

    // Label
    DrawText("Label",(int)(px+16),(int)(py+188),FONT_SMALL,TEXT_MUTED);
    Rectangle lbox={(float)(px+16),(float)(py+210),(float)(pw-32),34};
    DrawRectangleRec(lbox,BG_DEEP);DrawRectangleLinesEx(lbox,1.5f,labelFocused?NEON_CYAN:BORDER_DIM);
    DrawText(alarmLabel.c_str(),(int)lbox.x+8,(int)lbox.y+9,FONT_SMALL,TEXT_PRIMARY);
    if(labelFocused&&(int)(GetTime()*2)%2==0){int lw2=MeasureText(alarmLabel.c_str(),FONT_SMALL);DrawText("|",(int)lbox.x+10+lw2,(int)lbox.y+9,FONT_SMALL,NEON_CYAN);}
    if(IsMouseButtonPressed(MOUSE_LEFT_BUTTON)&&CheckCollisionPointRec(GetMousePosition(),lbox)){labelFocused=true;searchFocused=false;}
    TextInput(alarmLabel,28,labelFocused);

    // Repeat days
    DrawText("Repeat",(int)(px+16),(int)(py+260),FONT_SMALL,TEXT_MUTED);
    int chipX=px+16,chipY=py+282;
    for(int i=0;i<DAYS;i++){
        Rectangle chip={(float)(chipX+i*44),(float)chipY,38,26};
        bool act=(modalRepeat&(1<<i))!=0;
        if(DrawButton(chip,DAY_NAMES[i],act?NEON_CYAN:BG_HOVER,act?BG_DEEP:TEXT_PRIMARY,FONT_SMALL))modalRepeat^=(1<<i);
    }
    DrawText("(no days = fires once)",(int)(px+16),(int)(py+316),FONT_TINY,TEXT_DIM);

    // Snooze
    DrawText("Snooze",(int)(px+16),(int)(py+344),FONT_SMALL,TEXT_MUTED);
    if(DrawButton({(float)(px+16),(float)(py+366),36,26},"-",BG_HOVER,NEON_CYAN,FONT_SMALL))modalSnooze=std::max(1,modalSnooze-1);
    Rectangle sdisp={(float)(px+58),(float)(py+366),80,26};
    DrawRectangleRounded(sdisp,0.1f,8,BG_DEEP);DrawRectangleLinesEx(sdisp,1.0f,BORDER_DIM);
    std::string st=FmtShortMin(modalSnooze);int sw2=MeasureText(st.c_str(),FONT_SMALL);
    DrawText(st.c_str(),(int)(sdisp.x+(sdisp.width-sw2)/2),(int)sdisp.y+5,FONT_SMALL,TEXT_PRIMARY);
    if(DrawButton({(float)(px+144),(float)(py+366),36,26},"+",BG_HOVER,NEON_CYAN,FONT_SMALL))modalSnooze=std::min(60,modalSnooze+1);

    // Buttons
    auto doSave=[&](){
        Alarm a;a.hour=modalHour;a.minute=modalMinute;a.enabled=true;
        a.repeatMask=modalRepeat;a.snoozeMinutes=modalSnooze;
        a.label=alarmLabel.empty()?"Alarm":alarmLabel;
        if(editAlarmIdx>=0&&editAlarmIdx<(int)alarms.size())alarms[editAlarmIdx]=a;
        else alarms.push_back(a);
        addAlarmModal=false;editAlarmIdx=-1;labelFocused=false;stateDirty=true;
    };
    if(DrawButton({(float)(px+pw-210),(float)(py+ph-44),100,32},"Save",BG_HOVER,NEON_CYAN,FONT_NORMAL))doSave();
    if(DrawButton({(float)(px+pw-102),(float)(py+ph-44),96,32},"Cancel",BG_HOVER,NEON_PINK,FONT_NORMAL)){addAlarmModal=false;editAlarmIdx=-1;labelFocused=false;}
    if(labelFocused&&IsKeyPressed(KEY_ENTER))doSave();
    if(IsKeyPressed(KEY_ESCAPE)&&!labelFocused){addAlarmModal=false;editAlarmIdx=-1;}
}

// ============================================================
//  NOTIFICATION MODAL
// ============================================================
static void DrawNotifModal(int sw,int sh){
    if(!showNotif)return;
    DrawRectangle(0,0,sw,sh,{0,0,0,210});
    int pw=520,ph=230,px=(sw-pw)/2,py=(sh-ph)/2;
    DrawRectangle(px,py,pw,ph,{50,15,15,255});
    DrawGlowRect({(float)px,(float)py,(float)pw,(float)ph},NEON_PINK,6);

    if(notifIdx==-2){
        // Timer
        DrawText("TIMER FINISHED!",(int)(px+16),(int)(py+18),FONT_LARGE,NEON_PINK);
        DrawText("Your timer has ended.",(int)(px+16),(int)(py+65),FONT_NORMAL,TEXT_PRIMARY);
        if(DrawButton({(float)(px+pw-120),(float)(py+ph-50),106,34},"Dismiss",BG_HOVER,NEON_PINK,FONT_NORMAL)){timerState.finished=false;showNotif=false;notifIdx=-1;}
    } else if(notifIdx>=0&&notifIdx<(int)alarms.size()){
        Alarm&a=alarms[notifIdx];
        DrawText("ALARM!",(int)(px+16),(int)(py+18),FONT_LARGE,NEON_PINK);
        char tb[12];snprintf(tb,sizeof(tb),"%02d:%02d",a.hour,a.minute);
        DrawText(tb,(int)(px+16),(int)(py+58),FONT_TITLE,NEON_GOLD);
        DrawText(a.label.c_str(),(int)(px+16),(int)(py+108),FONT_NORMAL,TEXT_PRIMARY);
        if(DrawButton({(float)(px+pw-230),(float)(py+ph-50),106,34},"Snooze",BG_HOVER,NEON_GOLD,FONT_NORMAL)){a.ringing=false;a.snoozeUntil=GetTime()+a.snoozeMinutes*60.0;activeRingIdx=-1;showNotif=false;notifIdx=-1;stateDirty=true;}
        if(DrawButton({(float)(px+pw-116),(float)(py+ph-50),106,34},"Stop",BG_HOVER,NEON_PINK,FONT_NORMAL)){a.ringing=false;if(!a.repeatMask)a.enabled=false;activeRingIdx=-1;showNotif=false;notifIdx=-1;stateDirty=true;}
    } else {
        showNotif=false;notifIdx=-1;
    }
}

// ============================================================
//  MAIN
// ============================================================
int main(){
    if(!RequestResources(APP_NAME,RAM_MB,HDD_MB,PRIORITY_HIGH,0)){
        InitWindow(440,120,"Clock — Denied");SetTargetFPS(30);
        double t=GetTime();
        while(!WindowShouldClose()&&GetTime()-t<3.5){BeginDrawing();ClearBackground(BG_DEEP);DrawText("Insufficient resources.",18,40,FONT_NORMAL,NEON_PINK);EndDrawing();}
        CloseWindow();return 1;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(WIN_W,WIN_H,"NexOS Clock");
    SetTargetFPS(60);SetExitKey(KEY_NULL);
    SetWindowFocused();
    InitTone();
    LoadState();EnsureDefaults();

    pthread_create(&bgThread,nullptr,BgThreadFn,nullptr);

    while(!WindowShouldClose()&&appRunning){
        int sw=GetScreenWidth(),sh=GetScreenHeight();
        Rectangle c={20,52,(float)(sw-40),(float)(sh-72)};

        BeginDrawing();
        ClearBackground(BG_DEEP);
        DrawCyberpunkGrid(sw,sh);
        DrawTabs(sw);
        DrawRectangleRounded(c,0.05f,8,BG_PANEL);
        DrawRectangleLinesEx(c,1.0f,BORDER_DIM);

        if(currentTab==0)DrawWorldTab(c,sw);
        else if(currentTab==1)DrawAlarmsTab(c);
        else if(currentTab==2)DrawStopwatchTab(c);
        else DrawTimerTab(c);

        if(addCityModal)DrawAddCityModal(sw,sh);
        if(addAlarmModal)DrawAlarmModal(sw,sh);
        DrawNotifModal(sw,sh);

        EndDrawing();
    }

    bgRunning=false;
    pthread_join(bgThread,nullptr);
    SaveState();
    if(toneLoaded)UnloadSound(alarmTone);
    ReleaseResources(APP_NAME,RAM_MB,HDD_MB);
    CloseWindow();
    return 0;
}