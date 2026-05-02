// ============================================================
//  NexOS — Brick Breaker
//  Main menu → Play → 3 lives → Game Over / Win → back
//  Power-ups: wide paddle, multi-ball, slow ball, laser, sticky
//  Procedural sound via raylib audio synthesis
//  Soft cyberpunk palette — bright but not harsh
// ============================================================
#include "raylib.h"
#include "../include/theme.h"
#include "../include/resources.h"
#include "../include/ipc.h"

#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <cmath>
#include <string>
#include <vector>
#include <algorithm>

#define APP_NAME  "Brick Breaker"
#define RAM_MB    70
#define HDD_MB    10

// ── Window / layout ───────────────────────────────────────
#define WIN_W   900
#define WIN_H   660
#define PLAY_X   10
#define PLAY_Y   50
#define PLAY_W  880
#define PLAY_H  580

// ── Brick grid ────────────────────────────────────────────
#define BRICK_COLS  12
#define BRICK_ROWS   7
#define BRICK_W     (PLAY_W / BRICK_COLS)
#define BRICK_H     22
#define BRICK_PAD    2
#define BRICK_TOP   (PLAY_Y + 60)

// ── Paddle ────────────────────────────────────────────────
#define PAD_H        12
#define PAD_Y        (PLAY_Y + PLAY_H - 30)
#define PAD_BASE_W   100
#define PAD_SPEED    520.0f

// ── Ball ──────────────────────────────────────────────────
#define BALL_R       7
#define BALL_SPEED   340.0f
#define BALL_SPEED_SLOW 210.0f

// ── Power-up drop ─────────────────────────────────────────
#define PU_W   28
#define PU_H   16
#define PU_SPEED 110.0f
#define POWERUP_DROP_PERCENT 14

// ── Game states ───────────────────────────────────────────
enum GameState { GS_MENU, GS_PLAYING, GS_PAUSED, GS_GAMEOVER, GS_WIN };
static GameState gState = GS_MENU;
static bool appRunning  = true;

// ── Soft cyberpunk palette ────────────────────────────────
// Muted-bright: saturated but with slight grey mixed in
static const Color COL_BG      = {  8,  8, 18, 255 };
static const Color COL_GRID    = { 20, 20, 44, 255 };
static const Color COL_PAD     = {130, 220, 210, 255 };  // soft teal
static const Color COL_PAD_GL  = {130, 220, 210,  55 };
static const Color COL_BALL    = {230, 220, 160, 255 };  // warm cream
static const Color COL_HEART   = {220, 100, 130, 255 };  // dusty rose

// Brick row colours (7 rows) — soft pastels with saturation
static const Color BRICK_COLS_C[BRICK_ROWS] = {
    {190, 110, 180, 255},  // muted violet
    {120, 160, 220, 255},  // soft blue
    {100, 200, 190, 255},  // teal
    {140, 210, 130, 255},  // sage green
    {220, 200, 110, 255},  // warm yellow
    {220, 150,  90, 255},  // peach
    {200, 100, 110, 255},  // dusty red
};

// Power-up colours
static const Color PU_WIDE_C   = {130, 210, 255, 220 };  // sky blue
static const Color PU_MULTI_C  = {200, 160, 255, 220 };  // lavender
static const Color PU_SLOW_C   = {160, 230, 180, 220 };  // mint
static const Color PU_LASER_C  = {255, 180, 120, 220 };  // peach
static const Color PU_STICK_C  = {255, 220, 140, 220 };  // soft gold

// ── Brick ─────────────────────────────────────────────────
struct Brick {
    bool alive;
    int  row, col;
    int  hp;       // 1 or 2 (gold bricks)
    bool isGold;
};

// ── Ball ──────────────────────────────────────────────────
struct Ball {
    float x, y, vx, vy;
    bool  alive;
    bool  stuck;   // for sticky power-up
    float stickOff; // offset from paddle center
};

// ── Power-up types ────────────────────────────────────────
enum PuType { PU_WIDE, PU_MULTI, PU_SLOW, PU_LASER, PU_STICKY };

struct PowerUp {
    float x, y;
    PuType type;
    bool  alive;
};

// ── Laser ─────────────────────────────────────────────────
struct Laser {
    float x, y;
    bool  alive;
};

// ── Game data ─────────────────────────────────────────────
static Brick    bricks[BRICK_ROWS][BRICK_COLS];
static std::vector<Ball>    balls;
static std::vector<PowerUp> powerups;
static std::vector<Laser>   lasers;

static float padX        = PLAY_X + PLAY_W/2.0f - PAD_BASE_W/2.0f;
static float padW        = PAD_BASE_W;
static int   lives       = 3;
static int   score       = 0;
static int   bricksLeft  = 0;
static float animTime    = 0;

// Power-up timers (seconds remaining)
static float puWideTimer  = 0;
static float puSlowTimer  = 0;
static float puLaserTimer = 0;
static bool  puSticky     = false;
static float laserCooldown= 0;

// ── Audio ─────────────────────────────────────────────────
static bool   audioReady  = false;
static Sound  sndBounce, sndBrick, sndLose, sndPowerup, sndLaser, sndWin;
static Texture2D heartTex;
static bool heartTexLoaded = false;

static Sound MakeTone(float freq, float dur, float vol, int waveType) {
    // waveType: 0=sine 1=square 2=triangle 3=noise
    int sr = 44100;
    int n  = (int)(sr * dur);
    short* buf = (short*)MemAlloc(n * sizeof(short));
    if (!buf) return Sound{};
    for (int i = 0; i < n; i++) {
        float t   = (float)i / sr;
        float env = 1.0f - t/dur;
        env = env * env; // quadratic fade
        float s = 0;
        switch (waveType) {
            case 0: s = sinf(2*PI*freq*t); break;
            case 1: s = sinf(2*PI*freq*t) > 0 ? 1.0f : -1.0f; break;
            case 2: { float ph = fmodf(freq*t,1.0f); s = ph<0.5f ? 4*ph-1 : 3-4*ph; } break;
            case 3: s = (float)(rand()%32768-16384)/16384.0f; env *= 0.3f; break;
        }
        buf[i] = (short)(s * env * vol * 28000.0f);
    }
    Wave w = {0}; w.frameCount=n; w.sampleRate=sr; w.sampleSize=16; w.channels=1; w.data=buf;
    Sound snd = LoadSoundFromWave(w);
    UnloadWave(w);
    return snd;
}

static Sound MakeMultiTone(float* freqs, int fcount, float dur, float vol) {
    int sr=44100, n=(int)(sr*dur);
    short* buf=(short*)MemAlloc(n*sizeof(short));
    if(!buf) return Sound{};
    for(int i=0;i<n;i++){
        float t=(float)i/sr, env=(1.0f-t/dur); env=env*env;
        float s=0;
        for(int f=0;f<fcount;f++) s+=sinf(2*PI*freqs[f]*t);
        s/=fcount;
        buf[i]=(short)(s*env*vol*28000.0f);
    }
    Wave w={0};w.frameCount=n;w.sampleRate=sr;w.sampleSize=16;w.channels=1;w.data=buf;
    Sound snd=LoadSoundFromWave(w); UnloadWave(w); return snd;
}

static void InitAudio_() {
    if (!IsAudioDeviceReady()) InitAudioDevice();
    if (!IsAudioDeviceReady()) return;
    sndBounce  = MakeTone(440,  0.06f, 0.4f, 0); // soft sine ping
    sndBrick   = MakeTone(320,  0.10f, 0.5f, 2); // triangle pop
    float winF[]={523,659,784,1047}; sndWin = MakeMultiTone(winF,4,0.8f,0.5f);
    float loseF[]={220,185,156}; sndLose = MakeMultiTone(loseF,3,0.6f,0.5f);
    sndPowerup = MakeTone(660,  0.18f, 0.5f, 0);
    sndLaser   = MakeTone(880,  0.08f, 0.35f,1);
    audioReady = true;
    SetSoundVolume(sndBounce, 0.45f);
    SetSoundVolume(sndBrick,  0.5f);
    SetSoundVolume(sndWin,    0.6f);
    SetSoundVolume(sndLose,   0.55f);
    SetSoundVolume(sndPowerup,0.55f);
    SetSoundVolume(sndLaser,  0.35f);
}

static void PlaySfx(Sound& s) { if (audioReady && s.stream.buffer) PlaySound(s); }

// ── Build brick grid ──────────────────────────────────────
static void BuildBricks() {
    bricksLeft = 0;
    for (int r=0; r<BRICK_ROWS; r++) {
        for (int c=0; c<BRICK_COLS; c++) {
            auto& b = bricks[r][c];
            b.row=r; b.col=c; b.alive=true;
            // Gold bricks: random ~15% chance
            b.isGold = (rand()%100 < 15);
            b.hp     = b.isGold ? 2 : 1;
            bricksLeft++;
        }
    }
}

static void ResetBall() {
    balls.clear();
    Ball b;
    b.x = PLAY_X + PLAY_W/2.0f;
    b.y = PAD_Y - BALL_R - 2;
    float spd = BALL_SPEED;
    float ang = -PI/2.0f + ((rand()%60)-30)*PI/180.0f;
    b.vx = cosf(ang)*spd; b.vy = sinf(ang)*spd;
    b.alive = true; b.stuck = true; b.stickOff = 0;
    balls.push_back(b);
}

static void StartGame() {
    lives=3; score=0;
    padX = PLAY_X + PLAY_W/2.0f - PAD_BASE_W/2.0f;
    padW = PAD_BASE_W;
    puWideTimer=puSlowTimer=puLaserTimer=laserCooldown=0;
    puSticky=false;
    powerups.clear(); lasers.clear();
    BuildBricks();
    ResetBall();
    gState = GS_PLAYING;
}

// ── Helpers ───────────────────────────────────────────────
static Rectangle BrickRect(int r, int c) {
    return {
        PLAY_X + c * BRICK_W + BRICK_PAD,
        BRICK_TOP + r * (BRICK_H + BRICK_PAD),
        BRICK_W - BRICK_PAD*2,
        BRICK_H
    };
}

static float Clamp(float v, float lo, float hi) { return v<lo?lo:v>hi?hi:v; }

// ── Draw helpers ──────────────────────────────────────────
static void DrawTxtC(const char* t, int cx, int y, int sz, Color c) {
    DrawText(t, cx - MeasureText(t,sz)/2, y, sz, c);
}

static void DrawHeartIcon(float x, float y, float scale, Color c) {
    float r = 5.8f * scale;
    float leftX  = x - 5.2f * scale;
    float rightX = x + 5.2f * scale;
    float topY   = y - 4.6f * scale;
    float midY   = y + 0.8f * scale;
    float botY   = y + 12.2f * scale;

    DrawCircleV({leftX, topY}, r, c);
    DrawCircleV({rightX, topY}, r, c);

    // Fill lower body so the icon reads as a clear heart, not two dots.
    DrawTriangle({x - 11.4f * scale, midY}, {x + 11.4f * scale, midY}, {x, botY}, c);
    DrawTriangle({x - 7.0f * scale, topY}, {x + 7.0f * scale, topY}, {x, midY + 4.0f * scale}, c);
}

static void DrawGlowCircle(float x, float y, float r, Color c) {
    DrawCircle((int)x,(int)y,(int)(r+4),{c.r,c.g,c.b,30});
    DrawCircle((int)x,(int)y,(int)(r+2),{c.r,c.g,c.b,60});
    DrawCircle((int)x,(int)y,(int)r,c);
}

static void DrawGlowRectSoft(Rectangle r, Color c, float glow) {
    DrawRectangle((int)(r.x-glow),(int)(r.y-glow),(int)(r.width+glow*2),(int)(r.height+glow*2),{c.r,c.g,c.b,22});
    DrawRectangleRec(r,c);
}

// ── Main menu ─────────────────────────────────────────────
static float menuAnim = 0;

static void DrawMenu(int sw, int sh) {
    menuAnim += GetFrameTime();

    // Animated brick background
    for (int r=0;r<BRICK_ROWS;r++) for (int c=0;c<BRICK_COLS;c++) {
        float pulse = sinf(menuAnim*1.2f + r*0.4f + c*0.3f)*0.18f + 0.55f;
        Color bc = BRICK_COLS_C[r];
        bc.a = (unsigned char)(pulse * 120);
        Rectangle br = BrickRect(r,c);
        DrawRectangleRec(br, bc);
    }

    // Title
    int cx = sw/2;
    // Glow behind title
    DrawRectangle(cx-220, sh/2-155, 440, 70, {130,220,210,14});
    const char* title = "BRICK BREAKER";
    int tw = MeasureText(title, 46);
    // Shadow
    DrawText(title, cx-tw/2+2, sh/2-148+2, 46, {0,0,0,120});
    DrawText(title, cx-tw/2,   sh/2-148,   46, COL_PAD);

    const char* sub = "N E X O S";
    DrawTxtC(sub, cx, sh/2-98, FONT_SMALL, {130,220,210,140});

    // Animated ball bouncing in title area
    float bx = cx + sinf(menuAnim*1.8f)*120;
    float by = sh/2 - 62 + sinf(menuAnim*2.4f)*12;
    DrawGlowCircle(bx, by, BALL_R, COL_BALL);

    // Menu buttons
    struct Btn { const char* label; int y; };
    Btn btns[] = {{"PLAY",sh/2+10},{"QUIT",sh/2+72}};
    for (auto& btn : btns) {
        Rectangle r={(float)(cx-110),(float)btn.y,220,44};
        bool hov = CheckCollisionPointRec(GetMousePosition(), r);
        float pulse2 = sinf(menuAnim*3.0f)*0.05f + (hov?1.0f:0.85f);
        Color fill = hov ? Color{50,65,80,255} : Color{22,28,42,255};
        DrawRectangleRounded(r, 0.3f, 8, fill);
        DrawRectangleLinesEx(r, hov?2.0f:1.2f, {COL_PAD.r,COL_PAD.g,COL_PAD.b,(unsigned char)(int)(pulse2*220)});
        if (hov) DrawRectangle((int)r.x,(int)(r.y+r.height-2),(int)r.width,2,{COL_PAD.r,COL_PAD.g,COL_PAD.b,120});
        int bw = MeasureText(btn.label, FONT_NORMAL);
        DrawText(btn.label, cx-bw/2, btn.y+13, FONT_NORMAL, hov?COL_PAD:Color{170,190,200,255});
        if (hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
            if (btn.label[0]=='P') StartGame();
            else appRunning = false;
        }
    }

    // Controls hint
    DrawTxtC("Arrow/Mouse to move   Space to launch   P to pause", cx, sh/2+138, FONT_TINY, {80,100,110,200});
    DrawTxtC("Power-ups drop from broken bricks", cx, sh/2+156, FONT_TINY, {60,80,90,160});
}

// ── HUD ───────────────────────────────────────────────────
static void DrawHUD(int sw) {
    // Top bar
    DrawRectangle(0, 0, sw, PLAY_Y, {8,8,20,230});
    DrawLine(0, PLAY_Y, sw, PLAY_Y, {60,80,80,120});

    // Lives (hearts)
    for (int i=0; i<3; i++) {
        Color hc = i < lives ? COL_HEART : Color{40,30,36,255};
        float hx = 24.0f + i*32.0f, hy = 24.0f;
        if (heartTexLoaded) {
            Rectangle src = {0.0f, 0.0f, (float)heartTex.width, (float)heartTex.height};
            Rectangle dst = {hx - 13.0f, hy - 13.0f, 26.0f, 26.0f};
            DrawTexturePro(heartTex, src, dst, {0.0f, 0.0f}, 0.0f, hc);
        } else {
            DrawHeartIcon(hx, hy, 1.05f, hc);
        }
    }

    // Score
    char sStr[24]; snprintf(sStr,24,"SCORE  %06d",score);
    DrawText(sStr, sw/2 - MeasureText(sStr,FONT_NORMAL)/2, 12, FONT_NORMAL, {200,210,210,240});

    // Active power-up timers
    int px2 = sw - 16;
    auto drawTimer=[&](float t, const char* label, Color c){
        if(t <= 0) return;
        char buf[24]; snprintf(buf,24,"%s %.1fs",label,t);
        int tw = MeasureText(buf,FONT_TINY);
        px2 -= tw+12;
        DrawRectangle(px2-4, 8, tw+8, 20, {c.r,c.g,c.b,30});
        DrawText(buf, px2, 12, FONT_TINY, {c.r,c.g,c.b,220});
    };
    drawTimer(puWideTimer,  "WIDE",  PU_WIDE_C);
    drawTimer(puSlowTimer,  "SLOW",  PU_SLOW_C);
    drawTimer(puLaserTimer, "LASER", PU_LASER_C);
    if(puSticky) {
        const char* sl="STICKY";
        int tw=MeasureText(sl,FONT_TINY); px2-=tw+12;
        DrawRectangle(px2-4,8,tw+8,20,{PU_STICK_C.r,PU_STICK_C.g,PU_STICK_C.b,30});
        DrawText(sl,px2,12,FONT_TINY,{PU_STICK_C.r,PU_STICK_C.g,PU_STICK_C.b,220});
    }
}

// ── Draw game ─────────────────────────────────────────────
static void DrawGame(int sw, int sh) {
    // Play field border
    DrawRectangleLinesEx({PLAY_X,PLAY_Y,PLAY_W,PLAY_H},1,{40,60,60,180});

    // Bricks
    for (int r=0; r<BRICK_ROWS; r++) {
        for (int c=0; c<BRICK_COLS; c++) {
            auto& b=bricks[r][c]; if(!b.alive) continue;
            Rectangle br=BrickRect(r,c);
            Color bc = b.isGold ? Color{220,195,100,255} : BRICK_COLS_C[r];
            // HP=2 -> slightly brighter
            if (b.hp==2) { bc.r=std::min(255,bc.r+30);bc.g=std::min(255,bc.g+30);bc.b=std::min(255,bc.b+20); }
            DrawGlowRectSoft(br,bc,2);
            // Subtle top-edge highlight
            DrawLine((int)br.x+2,(int)br.y+1,(int)(br.x+br.width-2),(int)br.y+1,{255,255,255,40});
            if(b.hp==2) {
                DrawRectangleLinesEx(br,1.5f,{220,195,100,180});
                // Small star indicator
                DrawText("*",(int)(br.x+br.width-12),(int)br.y+3,FONT_TINY,{240,220,140,200});
            }
        }
    }

    // Power-ups
    for (auto& pu : powerups) {
        if (!pu.alive) continue;
        Color c; const char* lbl;
        switch(pu.type){
            case PU_WIDE:  c=PU_WIDE_C;  lbl="W"; break;
            case PU_MULTI: c=PU_MULTI_C; lbl="M"; break;
            case PU_SLOW:  c=PU_SLOW_C;  lbl="S"; break;
            case PU_LASER: c=PU_LASER_C; lbl="L"; break;
            case PU_STICKY:c=PU_STICK_C; lbl="K"; break;
        }
        Rectangle pr={pu.x-PU_W/2.0f,pu.y-PU_H/2.0f,(float)PU_W,(float)PU_H};
        DrawRectangleRounded(pr,0.4f,6,{c.r,c.g,c.b,60});
        DrawRectangleLinesEx(pr,1.2f,{c.r,c.g,c.b,180});
        int lw=MeasureText(lbl,FONT_TINY);
        DrawText(lbl,(int)(pu.x-lw/2),(int)(pu.y-FONT_TINY/2),FONT_TINY,{c.r,c.g,c.b,240});
    }

    // Lasers
    for (auto& l : lasers) {
        if (!l.alive) continue;
        DrawRectangle((int)(l.x-2),(int)l.y,4,18,{PU_LASER_C.r,PU_LASER_C.g,PU_LASER_C.b,200});
        DrawRectangle((int)(l.x-1),(int)l.y,2,18,{255,240,200,240});
    }

    // Paddle
    float pw  = puWideTimer>0 ? padW : padW;
    Rectangle pad={padX,PAD_Y,(float)pw,PAD_H};
    // Glow
    DrawRectangle((int)padX-3,(int)PAD_Y-2,(int)pw+6,(int)PAD_H+4,{COL_PAD.r,COL_PAD.g,COL_PAD.b,30});
    DrawRectangleRec(pad,COL_PAD);
    DrawLine((int)padX+2,(int)PAD_Y+2,(int)(padX+pw-2),(int)PAD_Y+2,{255,255,255,60});

    // Balls
    for (auto& b : balls) {
        if (!b.alive) continue;
        DrawGlowCircle(b.x, b.y, BALL_R, COL_BALL);
        // Subtle trail dot
        DrawCircle((int)(b.x-b.vx*0.02f),(int)(b.y-b.vy*0.02f),(int)(BALL_R*0.5f),{COL_BALL.r,COL_BALL.g,COL_BALL.b,60});
    }

    DrawHUD(sw);
}

// ── Overlay screens ───────────────────────────────────────
static void DrawOverlay(const char* title, Color titleC, int sw, int sh,
                         bool showRestart, bool showMenu) {
    DrawRectangle(0,0,sw,sh,{0,0,0,175});
    int cx=sw/2;
    DrawTxtC(title, cx, sh/2-60, 36, titleC);

    int btnY=sh/2+10;
    if (showRestart) {
        Rectangle r={(float)(cx-100),(float)btnY,200,40};
        bool hov=CheckCollisionPointRec(GetMousePosition(),r);
        DrawRectangleRounded(r,0.25f,8,hov?Color{40,60,60,255}:Color{20,30,30,255});
        DrawRectangleLinesEx(r,1.5f,{COL_PAD.r,COL_PAD.g,COL_PAD.b,180});
        DrawTxtC("PLAY AGAIN",cx,btnY+11,FONT_NORMAL,COL_PAD);
        if(hov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) StartGame();
        btnY+=54;
    }
    if (showMenu) {
        Rectangle r={(float)(cx-100),(float)btnY,200,40};
        bool hov=CheckCollisionPointRec(GetMousePosition(),r);
        DrawRectangleRounded(r,0.25f,8,hov?Color{40,50,70,255}:Color{18,22,36,255});
        DrawRectangleLinesEx(r,1.5f,{130,160,200,180});
        DrawTxtC("MAIN MENU",cx,btnY+11,FONT_NORMAL,{170,195,220,240});
        if(hov&&IsMouseButtonPressed(MOUSE_LEFT_BUTTON)){
            gState=GS_MENU; menuAnim=0;
            powerups.clear(); lasers.clear(); balls.clear();
        }
    }
}

// ── Spawn power-up ────────────────────────────────────────
static void TrySpawnPowerup(float bx, float by) {
    if (rand()%100 >= POWERUP_DROP_PERCENT) return; // 14% drop rate
    PowerUp pu;
    pu.x=bx; pu.y=by; pu.alive=true;
    int t=rand()%5;
    pu.type=(PuType)t;
    powerups.push_back(pu);
}

// ── Ball-brick collision ──────────────────────────────────
static void HandleBallBrick(Ball& ball) {
    for (int r=0;r<BRICK_ROWS;r++) {
        for (int c=0;c<BRICK_COLS;c++) {
            auto& b=bricks[r][c]; if(!b.alive) continue;
            Rectangle br=BrickRect(r,c);
            float bx=ball.x, by=ball.y;
            // AABB circle test
            float nearX=Clamp(bx,br.x,br.x+br.width);
            float nearY=Clamp(by,br.y,br.y+br.height);
            float dx=bx-nearX, dy=by-nearY;
            if(dx*dx+dy*dy > BALL_R*BALL_R) continue;

            // Which side?
            bool fromTop    = by < br.y+br.height/2 && fabsf(dy)>fabsf(dx);
            bool fromBottom = by > br.y+br.height/2 && fabsf(dy)>fabsf(dx);
            bool fromLeft   = bx < br.x+br.width/2  && fabsf(dx)>=fabsf(dy);
            bool fromRight  = bx > br.x+br.width/2  && fabsf(dx)>=fabsf(dy);

            if((fromTop||fromBottom)) ball.vy=-ball.vy;
            else ball.vx=-ball.vx;

            b.hp--;
            PlaySfx(sndBrick);
            if(b.hp<=0){
                b.alive=false; bricksLeft--;
                score += b.isGold ? 30 : 10;
                float cx=(br.x+br.width/2), cy=(br.y+br.height/2);
                TrySpawnPowerup(cx,cy);
            }
            return; // one brick per frame per ball
        }
    }
}

// ── Update game ───────────────────────────────────────────
static void UpdateGame(float dt) {
    if (gState != GS_PLAYING) return;
    animTime += dt;

    // Paddle movement
    float spd = PAD_SPEED * dt;
    if (IsKeyDown(KEY_LEFT)||IsKeyDown(KEY_A))   padX -= spd;
    if (IsKeyDown(KEY_RIGHT)||IsKeyDown(KEY_D))  padX += spd;
    // Mouse
    float mx = GetMouseX();
    if (mx > PLAY_X+padW/2 && mx < PLAY_X+PLAY_W-padW/2)
        padX = mx - padW/2;
    padX = Clamp(padX, PLAY_X, PLAY_X+PLAY_W-padW);

    // Timers
    puWideTimer  = std::max(0.0f, puWideTimer  - dt);
    puSlowTimer  = std::max(0.0f, puSlowTimer  - dt);
    puLaserTimer = std::max(0.0f, puLaserTimer - dt);
    laserCooldown= std::max(0.0f, laserCooldown - dt);
    if(puWideTimer  <= 0 && padW > PAD_BASE_W) padW = PAD_BASE_W;

    // Launch stuck ball
    if (IsKeyPressed(KEY_SPACE)||IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        for (auto& b : balls) {
            if (b.stuck) {
                b.stuck=false;
                float spd2 = puSlowTimer>0 ? BALL_SPEED_SLOW : BALL_SPEED;
                float ang = -PI/2.0f + ((rand()%40)-20)*PI/180.0f;
                b.vx=cosf(ang)*spd2; b.vy=sinf(ang)*spd2;
            }
        }
        // Laser shoot
        if (puLaserTimer>0 && laserCooldown<=0) {
            Laser l1,l2;
            l1.x=padX+12; l1.y=PAD_Y; l1.alive=true;
            l2.x=padX+padW-12; l2.y=PAD_Y; l2.alive=true;
            lasers.push_back(l1); lasers.push_back(l2);
            laserCooldown=0.28f;
            PlaySfx(sndLaser);
        }
    }

    // Pause
    if (IsKeyPressed(KEY_P)||IsKeyPressed(KEY_ESCAPE)) {
        gState=GS_PAUSED; return;
    }

    // Update balls
    float spd2factor = puSlowTimer>0 ? BALL_SPEED_SLOW/BALL_SPEED : 1.0f;
    for (auto& ball : balls) {
        if (!ball.alive) continue;
        if (ball.stuck) {
            ball.x = padX + padW/2 + ball.stickOff;
            ball.y = PAD_Y - BALL_R - 1;
            continue;
        }

        ball.x += ball.vx * dt;
        ball.y += ball.vy * dt;

        // Wall bounces
        if(ball.x-BALL_R < PLAY_X)      { ball.x=PLAY_X+BALL_R;         ball.vx=fabsf(ball.vx);  PlaySfx(sndBounce); }
        if(ball.x+BALL_R > PLAY_X+PLAY_W){ ball.x=PLAY_X+PLAY_W-BALL_R; ball.vx=-fabsf(ball.vx); PlaySfx(sndBounce); }
        if(ball.y-BALL_R < PLAY_Y)       { ball.y=PLAY_Y+BALL_R;         ball.vy=fabsf(ball.vy);  PlaySfx(sndBounce); }

        // Paddle hit
        Rectangle pad={padX,PAD_Y,padW,PAD_H};
        if (ball.vy>0 &&
            ball.x>=padX && ball.x<=padX+padW &&
            ball.y+BALL_R>=PAD_Y && ball.y+BALL_R<=PAD_Y+PAD_H+8) {
            PlaySfx(sndBounce);
            ball.y = PAD_Y - BALL_R;
            ball.vy = -fabsf(ball.vy);
            // Angle based on hit position
            float rel = (ball.x-(padX+padW/2))/(padW/2);
            float ang2 = rel * 65.0f * PI/180.0f - PI/2.0f;
            float spd3 = puSlowTimer>0 ? BALL_SPEED_SLOW : BALL_SPEED;
            ball.vx = cosf(ang2)*spd3;
            ball.vy = sinf(ang2)*spd3;
            if (puSticky) {
                ball.stuck=true;
                ball.stickOff=ball.x-(padX+padW/2);
            }
        }

        // Out of bounds (lost)
        if (ball.y - BALL_R > PLAY_Y + PLAY_H) {
            ball.alive=false;
        }

        // Brick collisions
        HandleBallBrick(ball);
    }

    // Remove dead balls
    int aliveBalls=0;
    for(auto&b:balls)if(b.alive)aliveBalls++;
    balls.erase(std::remove_if(balls.begin(),balls.end(),[](Ball&b){return!b.alive;}),balls.end());

    if (aliveBalls==0 || balls.empty()) {
        lives--;
        PlaySfx(sndLose);
        if(lives<=0){ gState=GS_GAMEOVER; }
        else {
            puWideTimer=puSlowTimer=puLaserTimer=0; puSticky=false; padW=PAD_BASE_W;
            lasers.clear(); powerups.clear();
            ResetBall();
        }
    }

    // Update power-ups
    for (auto& pu : powerups) {
        if (!pu.alive) continue;
        pu.y += PU_SPEED * dt;
        // Collect
        if (pu.y+PU_H/2 >= PAD_Y && pu.y-PU_H/2 <= PAD_Y+PAD_H &&
            pu.x >= padX && pu.x <= padX+padW) {
            pu.alive=false;
            PlaySfx(sndPowerup);
            switch(pu.type){
                case PU_WIDE:  puWideTimer=12.0f; padW=PAD_BASE_W*1.7f; break;
                case PU_MULTI: {
                    // Spawn 2 extra balls from existing ones
                    std::vector<Ball> extras;
                    for(auto&b:balls){
                        if(!b.alive)continue;
                        Ball nb=b; nb.stuck=false;
                        float ang3=atan2f(b.vy,b.vx)+PI/6.0f;
                        float spd3=puSlowTimer>0?BALL_SPEED_SLOW:BALL_SPEED;
                        nb.vx=cosf(ang3)*spd3; nb.vy=sinf(ang3)*spd3;
                        extras.push_back(nb);
                        Ball nb2=b; nb2.stuck=false;
                        float ang4=atan2f(b.vy,b.vx)-PI/6.0f;
                        nb2.vx=cosf(ang4)*spd3; nb2.vy=sinf(ang4)*spd3;
                        extras.push_back(nb2);
                    }
                    for(auto&nb:extras)balls.push_back(nb);
                    break;
                }
                case PU_SLOW:  puSlowTimer=10.0f;
                    for(auto&b:balls){if(!b.alive)continue;
                        float ang5=atan2f(b.vy,b.vx);
                        b.vx=cosf(ang5)*BALL_SPEED_SLOW; b.vy=sinf(ang5)*BALL_SPEED_SLOW;
                    } break;
                case PU_LASER: puLaserTimer=14.0f; break;
                case PU_STICKY:puSticky=true; break;
            }
        }
        if(pu.y>PLAY_Y+PLAY_H) pu.alive=false;
    }
    powerups.erase(std::remove_if(powerups.begin(),powerups.end(),[](PowerUp&p){return!p.alive;}),powerups.end());

    // Update lasers
    for(auto&l:lasers){
        if(!l.alive)continue;
        l.y-=520.0f*dt;
        if(l.y<PLAY_Y){l.alive=false;continue;}
        // Hit bricks
        for(int r2=0;r2<BRICK_ROWS;r2++){
            for(int c2=0;c2<BRICK_COLS;c2++){
                auto&b=bricks[r2][c2]; if(!b.alive)continue;
                Rectangle br2=BrickRect(r2,c2);
                if(l.x>=br2.x&&l.x<=br2.x+br2.width&&l.y>=br2.y&&l.y<=br2.y+br2.height){
                    b.hp--; l.alive=false;
                    PlaySfx(sndBrick);
                    if(b.hp<=0){b.alive=false;bricksLeft--;score+=b.isGold?30:10;}
                    goto nextLaser;
                }
            }
        }
        nextLaser:;
    }
    lasers.erase(std::remove_if(lasers.begin(),lasers.end(),[](Laser&l){return!l.alive;}),lasers.end());

    // Win check
    if (bricksLeft <= 0) {
        score+=lives*200; // bonus
        gState=GS_WIN;
        PlaySfx(sndWin);
    }
}

// ── Draw particles / effects ──────────────────────────────
// Simple screen-space star background
static void DrawStars(int sw, int sh) {
    srand(42);
    for (int i=0;i<80;i++){
        int sx=rand()%sw, sy=rand()%sh;
        float br=0.3f+sinf(animTime*1.2f+i*0.4f)*0.2f;
        unsigned char a=(unsigned char)(br*160);
        DrawPixel(sx,sy,{180,200,210,a});
    }
    srand((unsigned)time(nullptr));
}

// ── Pause overlay ─────────────────────────────────────────
static void DrawPause(int sw, int sh) {
    DrawRectangle(0,0,sw,sh,{0,0,0,150});
    int cx=sw/2;
    DrawTxtC("PAUSED", cx, sh/2-50, 36, {200,215,225,240});
    DrawTxtC("Press P or ESC to resume", cx, sh/2+10, FONT_SMALL, {120,140,150,200});
    if(IsKeyPressed(KEY_P)||IsKeyPressed(KEY_ESCAPE)) gState=GS_PLAYING;
}

// ── Legend / power-up key ─────────────────────────────────
static void DrawLegend(int sw, int sh) {
    // Tiny legend at bottom right
    struct{const char*l;Color c;}items[]={
        {"W Wide",PU_WIDE_C},{"M Multi",PU_MULTI_C},
        {"S Slow",PU_SLOW_C},{"L Laser",PU_LASER_C},{"K Sticky",PU_STICK_C}
    };
    int lx=sw-5;
    for(auto&it:items){
        int tw=MeasureText(it.l,FONT_TINY);
        lx-=(tw+14);
        DrawRectangle(lx-2,sh-PLAY_Y+4,tw+4,14,{it.c.r,it.c.g,it.c.b,30});
        DrawText(it.l,lx,sh-PLAY_Y+5,FONT_TINY,{it.c.r,it.c.g,it.c.b,160});
    }
}

// ============================================================
//  MAIN
// ============================================================
int main() {
    if (!RequestResources(APP_NAME, RAM_MB, HDD_MB, PRIORITY_LOW, 1)) {
        InitWindow(440,120,"Brick Breaker — Denied"); SetTargetFPS(30);
        double t=GetTime();
        while(!WindowShouldClose()&&GetTime()-t<3.5){
            BeginDrawing();ClearBackground(COL_BG);
            DrawText("Insufficient resources.",18,40,FONT_NORMAL,NEON_PINK);
            EndDrawing();
        }
        CloseWindow(); return 1;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(WIN_W, WIN_H, "NexOS — Brick Breaker");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    SetWindowFocused();

    srand((unsigned)time(nullptr));
    InitAudio_();
    if (FileExists("assets/icons/Heart.PNG")) {
        heartTex = LoadTexture("assets/icons/Heart.PNG");
        heartTexLoaded = heartTex.id > 0;
        if (heartTexLoaded) SetTextureFilter(heartTex, TEXTURE_FILTER_BILINEAR);
    }

    gState   = GS_MENU;
    menuAnim = 0;
    animTime = 0;

    while (!WindowShouldClose() && appRunning) {
        int sw = GetScreenWidth(), sh = GetScreenHeight();
        float dt = GetFrameTime();
        if(dt>0.05f) dt=0.05f; // cap delta

        animTime += dt;
        UpdateGame(dt);

        BeginDrawing();
        ClearBackground(COL_BG);

        // Faint grid
        for(int x=0;x<sw;x+=40) DrawLine(x,0,x,sh,{20,25,40,60});
        for(int y=0;y<sh;y+=40) DrawLine(0,y,sw,y,{20,25,40,60});
        DrawStars(sw,sh);

        switch (gState) {
            case GS_MENU:
                DrawMenu(sw,sh);
                break;
            case GS_PLAYING:
            case GS_PAUSED:
                DrawGame(sw,sh);
                DrawLegend(sw,sh);
                if(gState==GS_PAUSED) DrawPause(sw,sh);
                break;
            case GS_GAMEOVER: {
                DrawGame(sw,sh);
                char gStr[32]; snprintf(gStr,32,"SCORE: %d",score);
                DrawTxtC(gStr,sw/2,sh/2-18,FONT_SMALL,{180,180,190,220});
                DrawOverlay("GAME OVER",{220,100,110,255},sw,sh,true,true);
                break;
            }
            case GS_WIN: {
                DrawGame(sw,sh);
                char wStr[40]; snprintf(wStr,40,"YOU WIN!   SCORE: %d",score);
                DrawTxtC(wStr,sw/2,sh/2-18,FONT_SMALL,{180,220,200,220});
                DrawOverlay("CLEARED!",{130,220,190,255},sw,sh,true,true);
                break;
            }
        }

        EndDrawing();
    }

    if(audioReady){
        UnloadSound(sndBounce); UnloadSound(sndBrick);
        UnloadSound(sndWin);    UnloadSound(sndLose);
        UnloadSound(sndPowerup);UnloadSound(sndLaser);
        CloseAudioDevice();
    }
    if (heartTexLoaded) UnloadTexture(heartTex);
    ReleaseResources(APP_NAME, RAM_MB, HDD_MB);
    CloseWindow();
    return 0;
}