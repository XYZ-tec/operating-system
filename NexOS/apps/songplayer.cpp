// ============================================================
//  NexOS — Song Player
//  Reads songs from assets/songs/ folder
//  Supports mp3/ogg/wav, cover art (jpg/png), progress seek,
//  volume, shuffle, repeat, animated visualizer
// ============================================================
#include "raylib.h"
#include "../include/theme.h"
#include "../include/resources.h"
#include "../include/ipc.h"

#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>

#define APP_NAME  "Song Player"
#define RAM_MB    40
#define HDD_MB    20
#define WIN_W     860
#define WIN_H     580

#define SONGS_DIR   "assets/songs"
#define SIDEBAR_W   290
#define PLAYER_X    SIDEBAR_W
#define VIZ_BARS    38

// ── Song entry ────────────────────────────────────────────
struct Song {
    std::string filepath;   // full path e.g. assets/songs/zombie.mp3
    std::string filename;   // zombie.mp3
    std::string title;      // "Zombie"
    std::string artist;     // "The Cranberries"
    std::string coverPath;  // assets/songs/zombie.jpg or ""
};

// ── State ─────────────────────────────────────────────────
static std::vector<Song> songs;
static int       currentIdx  = -1;
static bool      isPlaying   = false;
static bool      isPaused    = false;
static bool      shuffleOn   = false;
static bool      repeatOn    = false;
static float     volume      = 0.8f;
static float     animTime    = 0.0f;
static bool      appRunning  = true;

// Raylib music handle
static Music     music       = {0};
static bool      musicLoaded = false;
static std::thread musicThread;
static std::atomic<bool> musicThreadRunning{false};
static std::mutex musicMutex;

// Cover texture
static Texture2D coverTex    = {0};
static bool      coverLoaded = false;
static int       coverSongIdx = -1; // which song the cover belongs to

// Sidebar scroll
static int       listScroll  = 0;
static int       hoveredItem = -1;

// Seeking
static bool      seeking     = false;

// Visualizer bar heights (smoothed)
static float     vizH[VIZ_BARS]    = {};
static float     vizTarget[VIZ_BARS]= {};

// ── Helpers ───────────────────────────────────────────────
static std::string ToLower(std::string s) {
    for (auto &c : s) { c = (char)tolower((unsigned char)c); }
    return s;
}

static std::string StripExt(const std::string& s) {
    size_t p=s.rfind('.'); return p!=std::string::npos?s.substr(0,p):s;
}

static std::string GetExt(const std::string& s) {
    size_t p=s.rfind('.'); return p!=std::string::npos?ToLower(s.substr(p+1)):"";
}

// "zombie" → "Zombie", "the_cranberries" → "The Cranberries"
static std::string PrettifyName(std::string s) {
    // replace underscores/hyphens/dots with spaces
    for (auto& c:s) if(c=='_'||c=='-'||c=='.') c=' ';
    // Capitalise each word
    bool cap=true;
    for (auto& c:s){
        if(c==' '){cap=true;}
        else if(cap){c=toupper(c);cap=false;}
    }
    return s;
}

// Normalize to lowercase alphanumeric only (used for fuzzy matching)
static std::string NormalizeAlnum(const std::string &s){
    std::string out;
    for(char c: s){ if(std::isalnum((unsigned char)c)) out.push_back((char)std::tolower((unsigned char)c)); }
    return out;
}

// ── Scan songs folder ─────────────────────────────────────
static void ScanSongs() {
    songs.clear();
    DIR* d=opendir(SONGS_DIR);
    if(!d) return;

    std::vector<std::string> audioFiles, imageFiles;
    struct dirent* e;
    while((e=readdir(d))){
        std::string n(e->d_name);
        if(n=="."||n=="..") continue;
        std::string ext=GetExt(n);
        if(ext=="mp3"||ext=="ogg"||ext=="wav"||ext=="flac")
            audioFiles.push_back(n);
        else if(ext=="jpg"||ext=="jpeg"||ext=="png")
            imageFiles.push_back(n);
    }
    closedir(d);
    std::sort(audioFiles.begin(),audioFiles.end());

    for(auto& af:audioFiles){
        Song s;
        s.filename = af;
        s.filepath = std::string(SONGS_DIR)+"/"+af;
        std::string base=StripExt(af);
        s.title  = PrettifyName(base);
        s.artist = "Unknown Artist";

        // Look for matching cover: same base name, any image ext
        for(auto& img:imageFiles){
            if(ToLower(StripExt(img))==ToLower(base)){
                s.coverPath=std::string(SONGS_DIR)+"/"+img;
                break;
            }
        }
        // If not found, try a relaxed match: compare normalized alnum-only names
        if(s.coverPath.empty()){
            std::string na = NormalizeAlnum(base);
            for(auto& img:imageFiles){
                std::string ni = NormalizeAlnum(StripExt(img));
                if(ni.find(na)!=std::string::npos || na.find(ni)!=std::string::npos){
                    s.coverPath=std::string(SONGS_DIR)+"/"+img;
                    break;
                }
            }
        }
        if(!s.coverPath.empty()){
            TraceLog(LOG_INFO, "Songplayer: matched cover '%s' -> '%s'", s.filename.c_str(), s.coverPath.c_str());
        }
        songs.push_back(s);
    }
}

// Control icons (optional)
static Texture2D iconPlay={0}, iconPause={0}, iconPrev={0}, iconNext={0};
static bool iconPlayLoaded=false, iconPauseLoaded=false, iconPrevLoaded=false, iconNextLoaded=false;

static void LoadControlIcons(){
    auto tryLoad=[&](Texture2D &t, bool &f, const char *name){
        std::string p = std::string("assets/icons/") + name + ".png";
        if(FileExists(p.c_str())){ t=LoadTexture(p.c_str()); f=(t.id>0); }
    };
    tryLoad(iconPlay,iconPlayLoaded,"play");
    tryLoad(iconPause,iconPauseLoaded,"pause");
    tryLoad(iconPrev,iconPrevLoaded,"prev");
    tryLoad(iconNext,iconNextLoaded,"next");
}

static void UnloadControlIcons(){
    if(iconPlayLoaded) UnloadTexture(iconPlay);
    if(iconPauseLoaded) UnloadTexture(iconPause);
    if(iconPrevLoaded) UnloadTexture(iconPrev);
    if(iconNextLoaded) UnloadTexture(iconNext);
    iconPlayLoaded=iconPauseLoaded=iconPrevLoaded=iconNextLoaded=false;
}

// ── Load / unload music ───────────────────────────────────
static void UnloadCurrent() {
    if(musicLoaded){
        std::lock_guard<std::mutex> lk(musicMutex);
        StopMusicStream(music); UnloadMusicStream(music); musicLoaded=false;
    }
    if(coverLoaded){ UnloadTexture(coverTex); coverLoaded=false; coverSongIdx=-1; }
    isPlaying=false; isPaused=false;
}

static void LoadSong(int idx) {
    if(idx<0||idx>=(int)songs.size()) return;
    UnloadCurrent();
    currentIdx=idx;
    {
        std::lock_guard<std::mutex> lk(musicMutex);
        music=LoadMusicStream(songs[idx].filepath.c_str());
        musicLoaded=(music.stream.buffer!=nullptr);
        if(musicLoaded){
            SetMusicVolume(music,volume);
            PlayMusicStream(music);
            isPlaying=true; isPaused=false;
        }
    }
    // Load cover if different song
    if(coverSongIdx!=idx){
        if(coverLoaded){ UnloadTexture(coverTex); coverLoaded=false; }
        if(!songs[idx].coverPath.empty() && FileExists(songs[idx].coverPath.c_str())){
            coverTex=LoadTexture(songs[idx].coverPath.c_str());
            coverLoaded=(coverTex.id>0);
            coverSongIdx=idx;
            TraceLog(LOG_INFO, "Songplayer: loading cover for '%s' -> %s", songs[idx].filename.c_str(), songs[idx].coverPath.c_str());
            if(!coverLoaded) TraceLog(LOG_WARNING, "Songplayer: failed to load cover texture: %s", songs[idx].coverPath.c_str());
        }
    }
}

static void PlayPause() {
    if(!musicLoaded) return;
    if(isPaused){ ResumeMusicStream(music); isPaused=false; isPlaying=true; }
    else         { PauseMusicStream(music);  isPaused=true;  isPlaying=false; }
}

static void NextSong() {
    if(songs.empty()) return;
    int next;
    if(shuffleOn) next=rand()%(int)songs.size();
    else          next=(currentIdx+1)%(int)songs.size();
    LoadSong(next);
}

static void PrevSong() {
    if(songs.empty()) return;
    int prev;
    if(shuffleOn) prev=rand()%(int)songs.size();
    else          prev=(currentIdx-1+(int)songs.size())%(int)songs.size();
    LoadSong(prev);
}

// ── Update visualizer targets ─────────────────────────────
static void UpdateViz(float dt) {
    // Animate bars: when playing, drive with pseudo-random sine combos
    // that look like a real spectrum reacting to music
    for(int i=0;i<VIZ_BARS;i++){
        if(isPlaying&&!isPaused){
            // Each bar has its own frequency mix
            float fi=(float)i/VIZ_BARS;
            float t=animTime;
            // Low freq bars react slower and taller
            float s1=sinf(t*(1.2f+fi*2.0f)+i*0.7f)*0.5f+0.5f;
            float s2=sinf(t*(2.4f-fi*1.1f)+i*1.3f)*0.5f+0.5f;
            float s3=sinf(t*(0.8f+fi*3.5f)+i*0.4f)*0.5f+0.5f;
            // Bass hump on left, treble on right
            float bassEnv  =expf(-fi*3.0f)*0.6f;
            float midEnv   =expf(-powf((fi-0.35f)*4.0f,2.0f))*0.5f;
            float trebleEnv=fi*fi*0.4f;
            float h=(s1*bassEnv+s2*midEnv+s3*trebleEnv);
            h=h*0.85f+0.08f;
            // Random spike
            if(rand()%120==0) h=std::min(1.0f,h+0.35f);
            vizTarget[i]=h;
        } else {
            // Decay to flat when paused/stopped
            vizTarget[i]=0.04f;
        }
        // Smooth toward target (fast attack, slow decay)
        float speed=(vizTarget[i]>vizH[i])?18.0f:7.0f;
        vizH[i]+=(vizTarget[i]-vizH[i])*speed*dt;
    }
}

// ── Format time ───────────────────────────────────────────
static std::string FmtTime(float sec) {
    int s=(int)std::max(0.0f,sec);
    char buf[12]; snprintf(buf,12,"%d:%02d",s/60,s%60);
    return std::string(buf);
}

// ── Draw helpers ──────────────────────────────────────────
static void DrawTxtC(const char* t,int cx,int y,int sz,Color c){
    DrawText(t,cx-MeasureText(t,sz)/2,y,sz,c);
}

static void DrawTxtR(const char* t,int rx,int y,int sz,Color c){
    DrawText(t,rx-MeasureText(t,sz),y,sz,c);
}

// Rounded rect with soft glow
static void DrawCard(Rectangle r, Color fill, Color border, float glow=0){
    if(glow>0){
        DrawRectangle((int)(r.x-glow),(int)(r.y-glow),(int)(r.width+glow*2),(int)(r.height+glow*2),
            {border.r,border.g,border.b,18});
    }
    DrawRectangleRounded(r,0.12f,8,fill);
    DrawRectangleLinesEx(r,1.2f,border);
}

// ── Sidebar: song list ────────────────────────────────────
static void DrawSidebar(int sw, int sh) {
    // Background
    DrawRectangle(0,0,SIDEBAR_W,sh,{10,10,22,255});
    DrawLine(SIDEBAR_W,0,SIDEBAR_W,sh,{40,50,70,180});

    // Header
    DrawRectangle(0,0,SIDEBAR_W,44,{14,14,30,255});
    DrawLine(0,44,SIDEBAR_W,44,{40,50,70,160});
    DrawText("Library",14,14,FONT_NORMAL,{160,180,200,240});
    char cnt[16]; snprintf(cnt,16,"%d songs",(int)songs.size());
    DrawTxtR(cnt,SIDEBAR_W-10,16,FONT_TINY,{80,100,120,200});

    if(songs.empty()){
        DrawText("No songs found.",16,70,FONT_SMALL,{80,100,110,200});
        DrawText("Add .mp3/.ogg/.wav to",16,92,FONT_TINY,{60,80,90,180});
        DrawText("assets/songs/",16,108,FONT_TINY,{80,120,130,200});
        return;
    }

    int itemH=58, visCount=(sh-48)/itemH;
    hoveredItem=-1;
    Vector2 mouse=GetMousePosition();

    // Clamp scroll
    int maxScroll=std::max(0,(int)songs.size()-visCount);
    listScroll=std::max(0,std::min(listScroll,maxScroll));

    // Scroll via mouse wheel when over sidebar
    if(mouse.x<SIDEBAR_W){
        float wh=GetMouseWheelMove();
        listScroll-=(int)wh;
    }

    for(int i=listScroll;i<(int)songs.size()&&i<listScroll+visCount;i++){
        int iy=48+(i-listScroll)*itemH;
        Rectangle row={2,(float)iy,(float)(SIDEBAR_W-4),(float)(itemH-2)};
        bool hov=CheckCollisionPointRec(mouse,row);
        bool cur=(i==currentIdx);

        if(hov) hoveredItem=i;

        // Row background
        Color rowBg = cur ? Color{28,38,58,255} : (hov ? Color{20,26,42,255} : Color{12,14,26,255});
        DrawRectangleRec(row,rowBg);
        if(cur){
            DrawRectangle(2,(int)iy,3,itemH-2,{100,180,220,255}); // left accent
            DrawRectangleLinesEx(row,1.0f,{60,100,140,120});
        }

        // Small cover thumbnail or placeholder
        Rectangle thumb={(float)8,(float)(iy+8),40,40};
        bool hasCover=(!songs[i].coverPath.empty());
        if(cur && coverLoaded && coverSongIdx==i){
            // Draw cover thumbnail from loaded texture
            Rectangle src={0,0,(float)coverTex.width,(float)coverTex.height};
            DrawTexturePro(coverTex,src,thumb,{0,0},0,WHITE);
        } else {
            DrawRectangleRounded(thumb,0.2f,6,{30,40,60,255});
            DrawRectangleLinesEx(thumb,1.0f,{50,70,90,120});
            // Music note icon
            DrawText("♪",(int)(thumb.x+12),(int)(thumb.y+10),FONT_NORMAL,
                cur?Color{120,190,220,200}:Color{60,80,100,160});
        }

        // Title & artist
        int tx=56, tw2=SIDEBAR_W-tx-8;
        // Truncate title if too long
        std::string title=songs[i].title;
        while(title.size()>1&&MeasureText(title.c_str(),FONT_SMALL)>tw2-4)
            title.pop_back();
        if(title!=songs[i].title) title+="..";

        DrawText(title.c_str(),tx,iy+10,FONT_SMALL,
            cur?Color{210,230,240,255}:Color{160,180,195,230});
        DrawText(songs[i].artist.c_str(),tx,iy+30,FONT_TINY,
            cur?Color{120,160,180,200}:Color{80,100,115,180});

        // Now playing indicator
        if(cur&&isPlaying){
            float bx=(float)(SIDEBAR_W-18);
            for(int b=0;b<3;b++){
                float bh=4.0f+sinf(animTime*5.0f+b*1.1f)*4.0f;
                DrawRectangle((int)(bx+b*5),(int)(iy+itemH/2-bh/2),3,(int)bh,
                    {100,200,180,200});
            }
        }

        if(hov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) LoadSong(i);
    }

    // Scrollbar
    if((int)songs.size()>visCount){
        float frac=(float)listScroll/maxScroll;
        int sbH=std::max(30,(int)((float)visCount/songs.size()*(sh-48)));
        int sbY=48+(int)(frac*(sh-48-sbH));
        DrawRectangle(SIDEBAR_W-4,48,4,sh-48,{20,25,40,200});
        DrawRectangle(SIDEBAR_W-4,sbY,4,sbH,{80,120,150,200});
    }
}

// ── Player panel ──────────────────────────────────────────
static void DrawPlayer(int sw, int sh) {
    int pw=sw-SIDEBAR_W;
    int px=SIDEBAR_W;

    // Background
    DrawRectangle(px,0,pw,sh,{8,10,20,255});

    if(currentIdx<0||songs.empty()){
        // Empty state
        DrawTxtC("Select a song to play",px+pw/2,sh/2-10,FONT_NORMAL,{60,80,95,180});
        DrawTxtC("♪",px+pw/2,sh/2-52,36,{40,65,80,140});
        return;
    }

    auto& song=songs[currentIdx];
    float duration=musicLoaded?GetMusicTimeLength(music):0;
    float elapsed =musicLoaded?GetMusicTimePlayed(music):0;
    float progress=(duration>0)?elapsed/duration:0;

    // ── Cover art ─────────────────────────────────────────
    int coverSz=180;
    int coverX=px+(pw-coverSz)/2;
    int coverY=28;

    Rectangle coverRect={(float)coverX,(float)coverY,(float)coverSz,(float)coverSz};

    if(coverLoaded&&coverSongIdx==currentIdx){
        // Draw cover with rounded corners via scissor trick
        DrawRectangleRounded({(float)(coverX-3),(float)(coverY-3),(float)(coverSz+6),(float)(coverSz+6)},
            0.08f,8,{20,30,50,255});
        Rectangle src={0,0,(float)coverTex.width,(float)coverTex.height};
        DrawTexturePro(coverTex,src,coverRect,{0,0},0,WHITE);
        DrawRectangleLinesEx(coverRect,1.5f,{60,90,120,180});
        // Subtle glow around cover when playing
        if(isPlaying){
            float pulse=sinf(animTime*1.8f)*0.3f+0.7f;
            DrawRectangleLinesEx(
                {(float)(coverX-5),(float)(coverY-5),(float)(coverSz+10),(float)(coverSz+10)},
                1.0f,{80,140,180,(unsigned char)(int)(pulse*60)});
        }
    } else {
        // Placeholder with music note
        DrawRectangleRounded(coverRect,0.1f,8,{18,24,40,255});
        DrawRectangleLinesEx(coverRect,1.5f,{40,60,80,160});
        DrawTxtC("♪",px+pw/2,coverY+60,56,{50,80,100,180});
    }

    // ── Song info ─────────────────────────────────────────
    int infoY=coverY+coverSz+18;
    int cx2=px+pw/2;

    DrawTxtC(song.title.c_str(),cx2,infoY,FONT_LARGE,{210,225,235,250});
    DrawTxtC(song.artist.c_str(),cx2,infoY+28,FONT_SMALL,{110,140,160,200});

    // ── Visualizer ────────────────────────────────────────
    int vizY=infoY+62;
    int vizW=pw-80;
    int vizX=px+40;
    int vizH2=52;
    int barW=vizW/VIZ_BARS-1;

    for(int i=0;i<VIZ_BARS;i++){
        float h=vizH[i]*vizH2;
        int bx=vizX+i*(barW+1);
        // Colour gradient: teal left → purple right
        float t=(float)i/VIZ_BARS;
        unsigned char r2=(unsigned char)(80+t*120);
        unsigned char g2=(unsigned char)(180-t*80);
        unsigned char b2=(unsigned char)(200+t*55);
        Color bc={r2,g2,b2,200};
        // Main bar
        DrawRectangle(bx,(int)(vizY+vizH2-h),barW,(int)h,bc);
        // Top cap glow
        DrawRectangle(bx,(int)(vizY+vizH2-h-1),barW,2,{255,255,255,80});
        // Reflection (faded below)
        DrawRectangle(bx,(int)(vizY+vizH2+2),barW,(int)(h*0.3f),{bc.r,bc.g,bc.b,50});
    }
    DrawLine(vizX,vizY+vizH2,vizX+vizW,vizY+vizH2,{40,55,70,160});

    // ── Progress bar ──────────────────────────────────────
    int progY=vizY+vizH2+22;
    int progX=px+40;
    int progW=pw-80;
    int progH=5;

    // Time labels
    DrawText(FmtTime(elapsed).c_str(),progX,progY-14,FONT_TINY,{90,115,130,220});
    DrawTxtR(FmtTime(duration).c_str(),progX+progW,progY-14,FONT_TINY,{90,115,130,220});

    // Track background
    DrawRectangle(progX,progY,progW,progH,{25,35,50,255});
    // Played portion
    int filled=(int)(progress*progW);
    if(filled>0){
        // Gradient fill
        for(int x=0;x<filled;x++){
            float f=(float)x/progW;
            Color fc={(unsigned char)(80+f*80),(unsigned char)(160+f*40),(unsigned char)(200-f*20),255};
            DrawRectangle(progX+x,progY,1,progH,fc);
        }
        // Playhead dot
        DrawCircle(progX+filled,progY+progH/2,7,{160,210,230,255});
        DrawCircle(progX+filled,progY+progH/2,4,{210,235,245,255});
    }
    DrawRectangleLinesEx({(float)progX,(float)progY,(float)progW,(float)progH},1.0f,{40,55,75,180});

    // Seek on click
    Rectangle progRect={(float)progX,(float)(progY-8),(float)progW,20};
    if(musicLoaded&&duration>0&&CheckCollisionPointRec(GetMousePosition(),progRect)){
        if(IsMouseButtonDown(MOUSE_LEFT_BUTTON)){
            seeking=true;
            float newPos=((GetMouseX()-progX)/(float)progW)*duration;
            newPos=std::max(0.0f,std::min(duration,newPos));
            SeekMusicStream(music,newPos);
        }
    }

    // ── Controls ──────────────────────────────────────────
    int ctrlY=progY+26;
    int ctrlCX=cx2;

    // Shuffle
    {
        Rectangle r={(float)(ctrlCX-170),(float)(ctrlY+6),40,28};
        bool hov=CheckCollisionPointRec(GetMousePosition(),r);
        Color c=shuffleOn?Color{120,200,180,255}:Color{70,95,110,200};
        DrawRectangleRounded(r,0.3f,6,shuffleOn?Color{20,45,40,255}:Color{14,18,28,255});
        DrawRectangleLinesEx(r,1.0f,shuffleOn?Color{80,160,150,200}:Color{40,55,70,160});
        DrawTxtC("⇄",ctrlCX-150,ctrlY+12,FONT_SMALL,c);
        if(hov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) shuffleOn=!shuffleOn;
    }

    // Prev
    {
        Rectangle r={(float)(ctrlCX-112),(float)(ctrlY+4),44,34};
        bool hov=CheckCollisionPointRec(GetMousePosition(),r);
        DrawRectangleRounded(r,0.3f,6,hov?Color{22,32,48,255}:Color{14,18,28,255});
        DrawRectangleLinesEx(r,1.0f,hov?Color{100,150,180,200}:Color{40,55,70,160});
        if(iconPrevLoaded){
            Texture2D &t=iconPrev;
            DrawTexturePro(t,{0,0,(float)t.width,(float)t.height},{(float)(ctrlCX-112),(float)(ctrlY+4),44,34},{0,0},0,WHITE);
        } else {
            DrawTxtC("◄◄",ctrlCX-90,ctrlY+11,FONT_SMALL,hov?Color{180,210,225,255}:Color{130,160,180,220});
        }
        if(hov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) PrevSong();
    }

    // Play/Pause (bigger)
    {
        int bsz=46;
        Rectangle r={(float)(ctrlCX-bsz/2),(float)(ctrlY),(float)bsz,(float)bsz};
        bool hov=CheckCollisionPointRec(GetMousePosition(),r);
        float pulse2=sinf(animTime*2.0f)*0.08f+(hov?1.0f:0.88f);
        // Glow ring when playing
        if(isPlaying){
            DrawCircle(ctrlCX,ctrlY+bsz/2,bsz/2+5,{80,160,190,(unsigned char)(int)(pulse2*50)});
        }
        DrawRectangleRounded(r,0.35f,10,hov?Color{30,55,70,255}:Color{20,38,52,255});
        DrawRectangleLinesEx(r,1.5f,hov?Color{120,195,220,255}:Color{80,140,170,200});
        // If pause/play icons loaded, draw them; otherwise draw glyphs
        if((isPlaying&&!isPaused && iconPauseLoaded) || (!isPlaying||isPaused) && iconPlayLoaded){
            Texture2D &tex = (isPlaying&&!isPaused) ? iconPause : iconPlay;
            DrawTexturePro(tex,{0,0,(float)tex.width,(float)tex.height},
                {(float)(ctrlCX-23),(float)ctrlY,46,46},{0,0},0,WHITE);
        } else {
            const char* sym=(isPlaying&&!isPaused)?"⏸":"▶";
            DrawTxtC(sym,ctrlCX,ctrlY+12,FONT_LARGE,{190,220,235,255});
        }
        if(hov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
            if(!musicLoaded&&currentIdx>=0) LoadSong(currentIdx);
            else PlayPause();
        }
    }

    // Next
    {
        Rectangle r={(float)(ctrlCX+68),(float)(ctrlY+4),44,34};
        bool hov=CheckCollisionPointRec(GetMousePosition(),r);
        DrawRectangleRounded(r,0.3f,6,hov?Color{22,32,48,255}:Color{14,18,28,255});
        DrawRectangleLinesEx(r,1.0f,hov?Color{100,150,180,200}:Color{40,55,70,160});
        if(iconNextLoaded){
            Texture2D &t=iconNext;
            DrawTexturePro(t,{0,0,(float)t.width,(float)t.height},{(float)(ctrlCX+68),(float)(ctrlY+4),44,34},{0,0},0,WHITE);
        } else {
            DrawTxtC("►►",ctrlCX+90,ctrlY+11,FONT_SMALL,hov?Color{180,210,225,255}:Color{130,160,180,220});
        }
        if(hov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) NextSong();
    }

    // Repeat
    {
        Rectangle r={(float)(ctrlCX+130),(float)(ctrlY+6),40,28};
        bool hov=CheckCollisionPointRec(GetMousePosition(),r);
        Color c=repeatOn?Color{120,200,180,255}:Color{70,95,110,200};
        DrawRectangleRounded(r,0.3f,6,repeatOn?Color{20,45,40,255}:Color{14,18,28,255});
        DrawRectangleLinesEx(r,1.0f,repeatOn?Color{80,160,150,200}:Color{40,55,70,160});
        DrawTxtC("↺",ctrlCX+150,ctrlY+12,FONT_SMALL,c);
        if(hov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) repeatOn=!repeatOn;
    }

    // ── Volume ────────────────────────────────────────────
    int volY=ctrlY+62;
    int volX=px+60;
    int volW=pw-120;

    DrawText("♪",volX,volY,FONT_SMALL,{60,90,110,180});
    DrawText("Vol",volX+22,volY+1,FONT_TINY,{70,95,115,180});

    // Slider track
    int slx=volX+52, slw=volW-80;
    DrawRectangle(slx,volY+5,slw,4,{22,32,48,255});
    int slFilled=(int)(volume*slw);
    if(slFilled>0){
        DrawRectangle(slx,volY+5,slFilled,4,{100,175,200,220});
        DrawCircle(slx+slFilled,volY+7,6,{160,210,225,255});
    }
    // Volume drag
    Rectangle volRect={(float)slx,(float)(volY-4),(float)slw,18};
    if(CheckCollisionPointRec(GetMousePosition(),volRect)&&IsMouseButtonDown(MOUSE_LEFT_BUTTON)){
        volume=std::clamp((GetMouseX()-slx)/(float)slw,0.0f,1.0f);
        if(musicLoaded) SetMusicVolume(music,volume);
    }

    // Volume % label
    char vStr[8]; snprintf(vStr,8,"%d%%",(int)(volume*100));
    DrawText(vStr,slx+slw+10,volY+1,FONT_TINY,{80,110,130,200});
}

// ── Top bar ───────────────────────────────────────────────
static void DrawTopBar(int sw) {
    DrawRectangle(0,0,sw,0,{10,12,24,255}); // purely decorative — height 0
}

// ── Keyboard shortcuts ────────────────────────────────────
static void HandleKeys() {
    if(IsKeyPressed(KEY_SPACE))  { if(!musicLoaded&&currentIdx>=0)LoadSong(currentIdx); else PlayPause(); }
    if(IsKeyPressed(KEY_RIGHT))  NextSong();
    if(IsKeyPressed(KEY_LEFT))   PrevSong();
    if(IsKeyPressed(KEY_S))      shuffleOn=!shuffleOn;
    if(IsKeyPressed(KEY_R))      repeatOn=!repeatOn;
    if(IsKeyPressed(KEY_UP))     { volume=std::min(1.0f,volume+0.05f); if(musicLoaded)SetMusicVolume(music,volume); }
    if(IsKeyPressed(KEY_DOWN))   { volume=std::max(0.0f,volume-0.05f); if(musicLoaded)SetMusicVolume(music,volume); }
}

// ============================================================
//  MAIN
// ============================================================
int main() {
    if(!RequestResources(APP_NAME,RAM_MB,HDD_MB,PRIORITY_NORMAL,1)){
        InitWindow(440,120,"Song Player — Denied"); SetTargetFPS(30);
        double t=GetTime();
        while(!WindowShouldClose()&&GetTime()-t<3.5){
            BeginDrawing();ClearBackground(BG_DEEP);
            DrawText("Insufficient resources.",18,40,FONT_NORMAL,NEON_PINK);
            EndDrawing();
        }
        CloseWindow(); return 1;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(WIN_W,WIN_H,"NexOS — Song Player");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    SetWindowFocused();

    if(!IsAudioDeviceReady()) InitAudioDevice();
    srand((unsigned)time(nullptr));

    ScanSongs();

    // Auto-play first song
    if(!songs.empty()) LoadSong(0);
    // Load optional control icons
    LoadControlIcons();

    // Start background music streaming thread so audio continues when minimized
    musicThreadRunning = true;
    musicThread = std::thread([](){
        while(musicThreadRunning){
            if(musicLoaded && isPlaying && !isPaused){
                std::lock_guard<std::mutex> lk(musicMutex);
                UpdateMusicStream(music);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    while(!WindowShouldClose()&&appRunning){
        int sw=GetScreenWidth(), sh=GetScreenHeight();
        float dt=GetFrameTime();
        animTime+=dt;

        // Auto-next when song ends (streaming occurs in background thread)
        if(musicLoaded&&isPlaying&&!isPaused){
            float played, length;
            {
                std::lock_guard<std::mutex> lk(musicMutex);
                played = GetMusicTimePlayed(music);
                length = GetMusicTimeLength(music);
            }
            if(played>=length-0.1f){
                if(repeatOn) {
                    std::lock_guard<std::mutex> lk(musicMutex);
                    SeekMusicStream(music,0);
                }
                else NextSong();
            }
        }

        UpdateViz(dt);
        HandleKeys();

        BeginDrawing();
        ClearBackground({8,10,20,255});

        // Subtle grid
        for(int x=0;x<sw;x+=44) DrawLine(x,0,x,sh,{16,20,36,50});
        for(int y=0;y<sh;y+=44) DrawLine(0,y,sw,y,{16,20,36,50});

        DrawSidebar(sw,sh);
        DrawPlayer(sw,sh);

        // Keyboard hint bar at bottom
        DrawRectangle(SIDEBAR_W,sh-20,sw-SIDEBAR_W,20,{8,10,20,220});
        DrawLine(SIDEBAR_W,sh-20,sw,sh-20,{30,40,55,160});
        const char* hints="Space Play/Pause   ← Prev   → Next   ↑↓ Volume   S Shuffle   R Repeat";
        DrawText(hints,SIDEBAR_W+16,sh-15,FONT_TINY,{55,75,90,200});

        EndDrawing();
    }

    appRunning=false;
    // Stop music thread
    musicThreadRunning = false;
    if(musicThread.joinable()) musicThread.join();
    if(musicLoaded){ std::lock_guard<std::mutex> lk(musicMutex); StopMusicStream(music); UnloadMusicStream(music); }
    if(coverLoaded)  UnloadTexture(coverTex);
    UnloadControlIcons();
    if(IsAudioDeviceReady()) CloseAudioDevice();
    ReleaseResources(APP_NAME,RAM_MB,HDD_MB);
    CloseWindow();
    return 0;
}