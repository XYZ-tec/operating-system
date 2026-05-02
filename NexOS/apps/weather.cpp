// ============================================================
//  NexOS — Weather App
//  Simulated weather with realistic data, animated visuals,
//  7-day forecast, hourly breakdown, and multi-city support.
//  Follows the exact same IPC + theme pattern as notepad/alarm.
// ============================================================
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

#define APP_NAME  "Weather"
#define RAM_MB    30
#define HDD_MB     5
#define WIN_W     960
#define WIN_H     660

// ── City data ─────────────────────────────────────────────
struct CityWeather {
    std::string name;
    int   utcOffsetMin;
    float tempC;        // current temp
    int   humidity;     // %
    float windKph;
    int   condCode;     // 0=sunny 1=cloudy 2=rainy 3=stormy 4=snowy 5=partly
    std::string condStr;
    float feelsLike;
    int   uvIndex;
    float visibilityKm;
};

// 7-day forecast entry
struct DayForecast {
    std::string dayName;
    int condCode;
    float hiC, loC;
    int   precipPct;
};

// Hourly entry
struct HourForecast {
    int hour;
    float tempC;
    int condCode;
    float precipPct;
};

// ── State ─────────────────────────────────────────────────
static std::vector<CityWeather> cities;
static int  selectedCity   = 0;
static int  activeTab      = 0; // 0=current 1=hourly 2=weekly
static bool addCityOpen    = false;
static bool appRunning     = true;
static float animTime      = 0.0f;
static int   scrollOffset  = 0;

// Modal
static char cityInputBuf[64] = "";
static int  cityInputLen = 0;
static bool cityInputFocus = false;

// ── Particle system for weather effects ───────────────────
struct Particle {
    float x, y, vx, vy, alpha, size;
};
static std::vector<Particle> particles;

// ── Pseudo-random seeded by city+day ──────────────────────
static float PseudoRand(int seed, int idx) {
    unsigned int s = (unsigned int)(seed * 2654435761u + idx * 1013904223u);
    s ^= s >> 16; s *= 0x45d9f3b; s ^= s >> 16;
    return (float)(s & 0xFFFF) / 65535.0f;
}

// Generate realistic weather for a city based on its name hash + current day
static CityWeather MakeCityWeather(const std::string& name, int utcMin) {
    CityWeather cw;
    cw.name = name;
    cw.utcOffsetMin = utcMin;

    // Seed from city name
    int seed = 0;
    for (char c : name) seed = seed * 31 + c;
    time_t now = time(nullptr);
    int dayOfYear = (int)(now / 86400);
    int combined  = seed + dayOfYear;

    float r0 = PseudoRand(combined, 0);
    float r1 = PseudoRand(combined, 1);
    float r2 = PseudoRand(combined, 2);
    float r3 = PseudoRand(combined, 3);
    float r4 = PseudoRand(combined, 4);

    // Temperature varies by UTC offset (rough latitude proxy)
    float baseTempC = 20.0f - fabsf((float)utcMin / 60.0f - 6.0f) * 1.2f;
    baseTempC += (r0 - 0.5f) * 18.0f;
    cw.tempC = roundf(baseTempC * 10.0f) / 10.0f;
    cw.feelsLike = cw.tempC + (r1 - 0.5f) * 4.0f;
    cw.humidity   = 30 + (int)(r2 * 60);
    cw.windKph    = 5.0f + r3 * 45.0f;
    cw.uvIndex    = 1 + (int)(r4 * 10);
    cw.visibilityKm = 5.0f + PseudoRand(combined,5) * 25.0f;

    // Condition
    float cr = PseudoRand(combined, 6);
    if      (cr < 0.25f) { cw.condCode = 0; cw.condStr = "Sunny"; }
    else if (cr < 0.45f) { cw.condCode = 5; cw.condStr = "Partly Cloudy"; }
    else if (cr < 0.60f) { cw.condCode = 1; cw.condStr = "Cloudy"; }
    else if (cr < 0.75f) { cw.condCode = 2; cw.condStr = "Rainy"; }
    else if (cr < 0.88f) { cw.condCode = 3; cw.condStr = "Stormy"; }
    else                 { cw.condCode = 4; cw.condStr = "Snowy"; }

    // Snow only for cold cities
    if (cw.condCode == 4 && cw.tempC > 8.0f) { cw.condCode = 2; cw.condStr = "Rainy"; }
    return cw;
}

static std::vector<DayForecast> MakeForecast(int citySeed) {
    static const char* dnames[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};
    time_t now = time(nullptr);
    struct tm* lt = localtime(&now);
    int today = lt->tm_wday;
    int dayOfYear = (int)(now / 86400);

    std::vector<DayForecast> fc;
    for (int i = 0; i < 7; i++) {
        DayForecast d;
        d.dayName = dnames[(today + i) % 7];
        float hi = 15.0f + PseudoRand(citySeed + dayOfYear + i, 0) * 22.0f;
        float lo = hi - 4.0f - PseudoRand(citySeed + dayOfYear + i, 1) * 8.0f;
        d.hiC = roundf(hi * 10.0f) / 10.0f;
        d.loC = roundf(lo * 10.0f) / 10.0f;
        d.precipPct = (int)(PseudoRand(citySeed + dayOfYear + i, 2) * 100.0f);
        float cr = PseudoRand(citySeed + dayOfYear + i, 3);
        d.condCode = cr < 0.3f ? 0 : cr < 0.5f ? 5 : cr < 0.65f ? 1 : cr < 0.8f ? 2 : cr < 0.92f ? 3 : 4;
        fc.push_back(d);
    }
    return fc;
}

static std::vector<HourForecast> MakeHourly(int citySeed, float baseTemp) {
    std::vector<HourForecast> hf;
    for (int h = 0; h < 24; h++) {
        HourForecast hfe;
        hfe.hour = h;
        float diurnal = sinf((h - 6) * 3.14159f / 12.0f) * 5.0f;
        hfe.tempC = roundf((baseTemp + diurnal + (PseudoRand(citySeed, h) - 0.5f) * 2.0f) * 10.0f) / 10.0f;
        hfe.precipPct = (int)(PseudoRand(citySeed + 1000, h) * 80.0f);
        float cr = PseudoRand(citySeed + 2000, h);
        hfe.condCode = cr < 0.3f ? 0 : cr < 0.55f ? 5 : cr < 0.7f ? 1 : cr < 0.85f ? 2 : 3;
        hf.push_back(hfe);
    }
    return hf;
}

// ── Weather icon drawing ──────────────────────────────────
static void DrawWeatherIcon(int condCode, float cx, float cy, float sz, float anim) {
    float r = sz * 0.38f;
    switch (condCode) {
    case 0: { // Sunny
        Color sunC = {255, 210, 0, 255};
        DrawCircle((int)cx, (int)cy, (int)r, sunC);
        for (int i = 0; i < 8; i++) {
            float ang = i * 3.14159f / 4.0f + anim * 0.3f;
            float x1 = cx + cosf(ang) * (r + 3);
            float y1 = cy + sinf(ang) * (r + 3);
            float x2 = cx + cosf(ang) * (r + 10 + sz * 0.06f);
            float y2 = cy + sinf(ang) * (r + 10 + sz * 0.06f);
            DrawLineEx({x1, y1}, {x2, y2}, 2.0f, sunC);
        }
        break;
    }
    case 1: { // Cloudy
        Color cloudC = {160, 160, 200, 255};
        DrawCircle((int)(cx-r*0.3f), (int)(cy+r*0.1f), (int)(r*0.7f), cloudC);
        DrawCircle((int)(cx+r*0.3f), (int)(cy+r*0.1f), (int)(r*0.6f), cloudC);
        DrawCircle((int)cx, (int)(cy-r*0.1f), (int)(r*0.75f), cloudC);
        break;
    }
    case 2: { // Rainy
        Color cloudC = {100, 120, 160, 255};
        Color rainC  = {80, 160, 255, 200};
        DrawCircle((int)(cx-r*0.3f), (int)(cy-r*0.2f), (int)(r*0.65f), cloudC);
        DrawCircle((int)(cx+r*0.3f), (int)(cy-r*0.2f), (int)(r*0.55f), cloudC);
        DrawCircle((int)cx, (int)(cy-r*0.3f), (int)(r*0.7f), cloudC);
        for (int i = 0; i < 4; i++) {
            float bx = cx - r*0.6f + i * r*0.4f;
            float by = cy + r*0.4f + fmodf(anim * 60.0f + i * 15.0f, 30.0f);
            DrawLineEx({bx, by}, {bx - 3, by + 10}, 2.0f, rainC);
        }
        break;
    }
    case 3: { // Stormy
        Color stormC = {60, 60, 90, 255};
        Color boltC  = {255, 210, 0, 255};
        DrawCircle((int)(cx-r*0.3f), (int)(cy-r*0.2f), (int)(r*0.65f), stormC);
        DrawCircle((int)(cx+r*0.3f), (int)(cy-r*0.2f), (int)(r*0.55f), stormC);
        DrawCircle((int)cx, (int)(cy-r*0.3f), (int)(r*0.7f), stormC);
        // Lightning bolt
        Vector2 boltPts[] = {
            {cx+r*0.1f, cy+r*0.1f},
            {cx-r*0.15f, cy+r*0.5f},
            {cx+r*0.05f, cy+r*0.5f},
            {cx-r*0.2f,  cy+r*0.95f}
        };
        for (int i = 0; i < 3; i++)
            DrawLineEx(boltPts[i], boltPts[i+1], 2.5f, boltC);
        break;
    }
    case 4: { // Snowy
        Color cloudC = {200, 210, 230, 255};
        Color snowC  = {220, 235, 255, 220};
        DrawCircle((int)(cx-r*0.3f), (int)(cy-r*0.2f), (int)(r*0.65f), cloudC);
        DrawCircle((int)(cx+r*0.3f), (int)(cy-r*0.2f), (int)(r*0.55f), cloudC);
        DrawCircle((int)cx, (int)(cy-r*0.3f), (int)(r*0.7f), cloudC);
        for (int i = 0; i < 5; i++) {
            float bx = cx - r*0.8f + i * r*0.4f;
            float by = cy + r*0.4f + fmodf(anim * 30.0f + i * 12.0f, 25.0f);
            DrawCircle((int)bx, (int)by, 3, snowC);
        }
        break;
    }
    case 5: { // Partly cloudy
        Color sunC   = {255, 210, 0, 255};
        Color cloudC = {160, 170, 200, 220};
        DrawCircle((int)(cx-r*0.15f), (int)(cy-r*0.1f), (int)(r*0.7f), sunC);
        DrawCircle((int)(cx+r*0.2f),  (int)(cy+r*0.15f), (int)(r*0.6f), cloudC);
        DrawCircle((int)(cx+r*0.6f),  (int)(cy+r*0.15f), (int)(r*0.5f), cloudC);
        DrawCircle((int)(cx+r*0.42f), (int)(cy+r*0.0f),  (int)(r*0.65f), cloudC);
        break;
    }
    }
}

// ── Particles ─────────────────────────────────────────────
static void SpawnParticles(int condCode, int sw, int editorH) {
    if (condCode == 2 || condCode == 3) { // rain
        if ((int)particles.size() < 80) {
            Particle p;
            p.x = (float)(rand() % sw);
            p.y = (float)(rand() % 60);
            p.vx = -1.5f; p.vy = 8.0f + (rand() % 6);
            p.alpha = 0.6f + (rand() % 40) * 0.01f;
            p.size  = 1.5f;
            particles.push_back(p);
        }
    } else if (condCode == 4) { // snow
        if ((int)particles.size() < 60) {
            Particle p;
            p.x = (float)(rand() % sw);
            p.y = (float)(rand() % 60);
            p.vx = (rand() % 3 - 1) * 0.5f;
            p.vy = 1.5f + (rand() % 3) * 0.5f;
            p.alpha = 0.7f;
            p.size  = 3.0f;
            particles.push_back(p);
        }
    } else {
        particles.clear();
    }
}

static void UpdateDrawParticles(float dt, int editorH) {
    for (auto& p : particles) {
        p.x += p.vx;
        p.y += p.vy;
        DrawCircle((int)p.x, (int)p.y, p.size,
            {200, 220, 255, (unsigned char)(int)(p.alpha * 200)});
    }
    particles.erase(std::remove_if(particles.begin(), particles.end(),
        [editorH](const Particle& p){ return p.y > editorH || p.x < 0; }),
        particles.end());
}

// ── Helper: draw text centered ────────────────────────────
static void DrawTxtC(const char* t, int cx, int y, int sz, Color c) {
    int w = MeasureText(t, sz);
    DrawText(t, cx - w/2, y, sz, c);
}

// ── Add City Modal ────────────────────────────────────────
static const struct { const char* name; int utcMin; } CITY_DB[] = {
    {"Lahore",300},{"Karachi",300},{"Islamabad",300},
    {"London",0},{"Paris",60},{"Berlin",60},{"Rome",60},
    {"Dubai",240},{"Delhi",330},{"Tokyo",540},
    {"New York",-300},{"Los Angeles",-480},
    {"Sydney",600},{"Cairo",120},{"Singapore",480},
    {"Toronto",-300},{"Chicago",-360},{"Moscow",180},
    {"Bangkok",420},{"Jakarta",420},
};
static const int CITY_DB_COUNT = 20;

static void DrawAddCityModal(int sw, int sh) {
    if (!addCityOpen) return;
    DrawRectangle(0, 0, sw, sh, {0,0,0,190});
    int pw=580, ph=400, px=(sw-pw)/2, py=(sh-ph)/2;
    DrawRectangle(px, py, pw, ph, BG_PANEL);
    DrawGlowRect({(float)px,(float)py,(float)pw,(float)ph}, NEON_CYAN, 5);
    DrawText("Add City", px+16, py+14, FONT_LARGE, NEON_CYAN);
    DrawLine(px, py+40, px+pw, py+40, BORDER_DIM);

    // Search box
    Rectangle sbox={(float)(px+16),(float)(py+52),(float)(pw-32),34};
    DrawRectangleRec(sbox, BG_DEEP);
    DrawRectangleLinesEx(sbox, 1.5f, cityInputFocus ? NEON_CYAN : BORDER_DIM);
    DrawText(cityInputBuf, (int)sbox.x+10, (int)sbox.y+9, FONT_SMALL, TEXT_PRIMARY);
    if (cityInputFocus && (int)(GetTime()*2)%2==0) {
        int cw = MeasureText(cityInputBuf, FONT_SMALL);
        DrawText("|", (int)sbox.x+12+cw, (int)sbox.y+9, FONT_SMALL, NEON_CYAN);
    }
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON) && CheckCollisionPointRec(GetMousePosition(), sbox))
        cityInputFocus = true;

    // Text input
    if (cityInputFocus) {
        int k = GetCharPressed();
        while (k > 0) {
            bool ok = (k>='a'&&k<='z')||(k>='A'&&k<='Z')||k==' ';
            if (ok && cityInputLen < 62) { cityInputBuf[cityInputLen++]=(char)k; cityInputBuf[cityInputLen]='\0'; }
            k = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && cityInputLen > 0) cityInputBuf[--cityInputLen] = '\0';
    }

    // City list filtered
    int ry = py + 98;
    std::string q(cityInputBuf);
    for (auto& c : q) c = tolower(c);
    for (int i = 0; i < CITY_DB_COUNT; i++) {
        std::string n(CITY_DB[i].name); std::string nl = n;
        for (auto& c : nl) c = tolower(c);
        if (!q.empty() && nl.find(q) == std::string::npos) continue;
        Rectangle row={(float)(px+12),(float)ry,(float)(pw-24),28};
        bool hov = CheckCollisionPointRec(GetMousePosition(), row);
        if (hov) DrawRectangleRec(row, BG_HOVER);
        DrawText(CITY_DB[i].name, (int)row.x+10, (int)row.y+6, FONT_SMALL, TEXT_PRIMARY);
        char utcStr[16]; snprintf(utcStr,16,"UTC%+d", CITY_DB[i].utcMin/60);
        DrawText(utcStr, (int)(row.x+row.width-80), (int)row.y+6, FONT_TINY, TEXT_MUTED);
        if (DrawButton({row.x+row.width-56, row.y+4, 48, 20}, "Add", hov?NEON_CYAN:BG_HOVER, hov?BG_DEEP:TEXT_PRIMARY, FONT_TINY)) {
            cities.push_back(MakeCityWeather(CITY_DB[i].name, CITY_DB[i].utcMin));
            addCityOpen = false; cityInputFocus = false;
            memset(cityInputBuf, 0, 64); cityInputLen = 0;
            break;
        }
        ry += 34;
        if (ry > py+ph-44) break;
    }

    if (DrawButton({(float)(px+pw-96),(float)(py+ph-40),86,28}, "Cancel", BG_HOVER, NEON_PINK, FONT_SMALL))
        { addCityOpen=false; cityInputFocus=false; }
    if (IsKeyPressed(KEY_ESCAPE)) { addCityOpen=false; cityInputFocus=false; }
}

// ── Current weather panel ─────────────────────────────────
static void DrawCurrentPanel(Rectangle c, const CityWeather& cw) {
    int cx = (int)(c.x + c.width/2);

    // Background gradient effect (faint colored rect)
    Color bgAccent;
    switch (cw.condCode) {
        case 0: bgAccent = {40,30,0,80}; break;
        case 1: bgAccent = {20,20,40,80}; break;
        case 2: bgAccent = {0,20,50,80}; break;
        case 3: bgAccent = {10,10,30,100}; break;
        case 4: bgAccent = {30,40,60,80}; break;
        default: bgAccent = {20,25,40,80}; break;
    }
    DrawRectangleRec(c, bgAccent);

    // City name
    DrawTxtC(cw.name.c_str(), cx, (int)c.y+18, FONT_LARGE, TEXT_PRIMARY);

    // Big weather icon
    DrawWeatherIcon(cw.condCode, (float)cx, c.y+130, 100, animTime);

    // Condition string
    DrawTxtC(cw.condStr.c_str(), cx, (int)c.y+190, FONT_NORMAL, TEXT_MUTED);

    // Big temperature
    char tmpStr[16]; snprintf(tmpStr,16,"%.0f C", cw.tempC);
    int tw = MeasureText(tmpStr, 56);
    DrawText(tmpStr, cx-tw/2, (int)c.y+216, 56, NEON_GOLD);

    // Feels like
    char flStr[32]; snprintf(flStr,32,"Feels like %.0f C", cw.feelsLike);
    DrawTxtC(flStr, cx, (int)c.y+280, FONT_SMALL, TEXT_MUTED);

    // Stats row
    int statsY = (int)c.y + 316;
    int statW  = (int)c.width / 4;
    struct { const char* label; char val[24]; } stats[4];
    snprintf(stats[0].val,24,"%d%%",cw.humidity); stats[0].label="Humidity";
    snprintf(stats[1].val,24,"%.0f km/h",cw.windKph); stats[1].label="Wind";
    snprintf(stats[2].val,24,"UV %d",cw.uvIndex); stats[2].label="UV Index";
    snprintf(stats[3].val,24,"%.0f km",cw.visibilityKm); stats[3].label="Visibility";

    for (int i = 0; i < 4; i++) {
        int sx = (int)(c.x + i * statW + statW/2);
        Rectangle card={(float)(c.x+i*statW+8),(float)statsY,(float)(statW-16),62};
        DrawRectangleRounded(card, 0.15f, 6, BG_HOVER);
        DrawRectangleLinesEx(card, 1.0f, BORDER_DIM);
        DrawTxtC(stats[i].val, sx, statsY+8,  FONT_SMALL, NEON_CYAN);
        DrawTxtC(stats[i].label, sx, statsY+32, FONT_TINY, TEXT_MUTED);
    }
}

// ── Hourly panel ──────────────────────────────────────────
static void DrawHourlyPanel(Rectangle c, const CityWeather& cw) {
    int citySeed = 0;
    for (char ch : cw.name) citySeed = citySeed * 31 + ch;
    auto hourly = MakeHourly(citySeed, cw.tempC);

    DrawText("Hourly Forecast", (int)c.x+16, (int)c.y+12, FONT_NORMAL, TEXT_MUTED);
    DrawLine((int)c.x, (int)c.y+36, (int)(c.x+c.width), (int)c.y+36, BORDER_DIM);

    time_t now = time(nullptr);
    struct tm* lt = localtime(&now);
    int curHour = lt->tm_hour;

    float colW = c.width / 6.0f;
    int start = (scrollOffset / (int)colW) % 24;
    for (int i = 0; i < 7; i++) {
        int hi = (curHour + i) % 24;
        auto& hfe = hourly[hi];
        float hx = c.x + i * colW;
        Rectangle col={hx, c.y+42, colW-4, c.height-50};
        bool hov = CheckCollisionPointRec(GetMousePosition(), {hx, c.y, colW, c.height});
        DrawRectangleRounded(col, 0.1f, 6, hov ? BG_HOVER : BG_PANEL);
        DrawRectangleLinesEx(col, 1.0f, hov ? NEON_CYAN : BORDER_DIM);

        char hStr[8]; snprintf(hStr,8,"%02d:00",hi);
        DrawTxtC(hStr, (int)(hx+colW/2), (int)c.y+50, FONT_TINY, i==0 ? NEON_CYAN : TEXT_MUTED);

        DrawWeatherIcon(hfe.condCode, hx+colW/2, c.y+130, 44, animTime);

        char tStr[10]; snprintf(tStr,10,"%.0f",hfe.tempC);
        DrawTxtC(tStr, (int)(hx+colW/2), (int)c.y+190, FONT_NORMAL, NEON_GOLD);

        char pStr[8]; snprintf(pStr,8,"%.0f%%",hfe.precipPct);
        DrawTxtC(pStr, (int)(hx+colW/2), (int)c.y+216, FONT_TINY, {80,160,255,200});
    }
}

// ── Weekly panel ──────────────────────────────────────────
static void DrawWeeklyPanel(Rectangle c, const CityWeather& cw) {
    int citySeed = 0;
    for (char ch : cw.name) citySeed = citySeed * 31 + ch;
    auto fc = MakeForecast(citySeed);

    DrawText("7-Day Forecast", (int)c.x+16, (int)c.y+12, FONT_NORMAL, TEXT_MUTED);
    DrawLine((int)c.x, (int)c.y+36, (int)(c.x+c.width), (int)c.y+36, BORDER_DIM);

    float rowH = (c.height - 44) / 7.0f;
    for (int i = 0; i < 7; i++) {
        float ry = c.y + 44 + i * rowH;
        Rectangle row={c.x+8, ry, c.width-16, rowH-4};
        bool hov = CheckCollisionPointRec(GetMousePosition(), row);
        DrawRectangleRounded(row, 0.1f, 6, hov?BG_HOVER:BG_PANEL);
        DrawRectangleLinesEx(row, 1.0f, hov?NEON_CYAN:BORDER_DIM);

        // Day name
        DrawText(fc[i].dayName.c_str(), (int)c.x+22, (int)ry+12, FONT_SMALL,
            i==0 ? NEON_CYAN : TEXT_PRIMARY);
        // Icon
        DrawWeatherIcon(fc[i].condCode, c.x+140, ry+rowH/2, 30, animTime);
        // Hi/Lo
        char hlStr[20]; snprintf(hlStr,20,"%.0f / %.0f", fc[i].hiC, fc[i].loC);
        DrawText(hlStr, (int)(c.x+c.width-240), (int)ry+12, FONT_SMALL, TEXT_PRIMARY);
        // Precip
        char prStr[16]; snprintf(prStr,16,"Rain %d%%", fc[i].precipPct);
        DrawText(prStr, (int)(c.x+c.width-100), (int)ry+12, FONT_TINY, {80,160,255,220});
    }
}

// ── City selector sidebar ─────────────────────────────────
static void DrawCitySidebar(int sw, int sh, int sideW) {
    DrawRectangle(0, 0, sideW, sh, {12,12,26,240});
    DrawLine(sideW, 0, sideW, sh, BORDER_DIM);
    DrawText("Cities", 14, 14, FONT_SMALL, TEXT_MUTED);

    if (DrawButton({(float)(sideW-76),8,(float)68,24}, "+ Add", BG_HOVER, NEON_CYAN, FONT_TINY)) {
        addCityOpen = true; cityInputFocus = true;
        memset(cityInputBuf,0,64); cityInputLen=0;
    }

    DrawLine(0, 40, sideW, 40, BORDER_DIM);

    for (int i = 0; i < (int)cities.size(); i++) {
        int cy = 50 + i * 72;
        Rectangle row={4,(float)cy,(float)(sideW-8),68};
        bool hov = CheckCollisionPointRec(GetMousePosition(), row);
        bool sel = (i == selectedCity);

        Color bg = sel ? BG_HOVER : (hov ? Color{58,60,86,255} : BG_PANEL);
        DrawRectangleRounded(row, 0.1f, 6, bg);
        if (sel) DrawRectangleLinesEx(row, 1.0f, NEON_CYAN);

        DrawWeatherIcon(cities[i].condCode, 28, cy+34, 28, animTime);
        DrawText(cities[i].name.c_str(), 52, cy+8, FONT_TINY, sel?NEON_CYAN:TEXT_PRIMARY);
        char tmpStr[16]; snprintf(tmpStr,16,"%.0f C", cities[i].tempC);
        DrawText(tmpStr, 52, cy+28, FONT_SMALL, NEON_GOLD);
        DrawText(cities[i].condStr.c_str(), 52, cy+48, FONT_TINY, TEXT_DIM);

        // Delete btn
        if (cities.size() > 1) {
            Rectangle delBtn={(float)(sideW-22),(float)(cy+4),18,18};
            if (DrawButton(delBtn,"x",BG_DEEP,NEON_PINK,FONT_TINY)) {
                cities.erase(cities.begin()+i);
                if (selectedCity >= (int)cities.size()) selectedCity = (int)cities.size()-1;
                break;
            }
        }

        if (hov && IsMouseButtonPressed(MOUSE_LEFT_BUTTON) &&
            !CheckCollisionPointRec(GetMousePosition(), {(float)(sideW-22),(float)(cy+4),18,18}))
            selectedCity = i;
    }
}

// ── Tab bar ───────────────────────────────────────────────
static void DrawTabBar(int sideW, int sw) {
    const char* tabs[] = {"Current","Hourly","7-Day"};
    int tabW = (sw - sideW) / 3;
    int y = 0;
    DrawRectangle(sideW, y, sw-sideW, 38, BG_TITLEBAR);
    DrawLine(sideW, 38, sw, 38, BORDER_DIM);
    for (int i = 0; i < 3; i++) {
        Rectangle r={(float)(sideW+i*tabW),(float)y,(float)tabW,38};
        bool act = (i == activeTab);
        if (act) { DrawRectangleRec(r, BG_HOVER); DrawLine((int)r.x,(int)(r.y+36),(int)(r.x+r.width),(int)(r.y+36),NEON_CYAN); }
        int tw = MeasureText(tabs[i], FONT_SMALL);
        DrawText(tabs[i], (int)(r.x+(r.width-tw)/2), y+12, FONT_SMALL, act?NEON_CYAN:TEXT_MUTED);
        if (CheckCollisionPointRec(GetMousePosition(), r) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON))
            activeTab = i;
    }
}

// ── Refresh button ────────────────────────────────────────
static double lastRefresh = 0;
static void DrawRefreshBtn(int sw) {
    double age = GetTime() - lastRefresh;
    bool can = age > 10.0;
    char label[24]; snprintf(label,24, can ? "Refresh" : "%.0fs", 10.0 - age);
    if (DrawButton({(float)(sw-104),4,96,28}, label, BG_HOVER, can?NEON_GREEN:TEXT_DIM, FONT_TINY)) {
        if (can) {
            for (auto& c : cities) c = MakeCityWeather(c.name, c.utcOffsetMin);
            lastRefresh = GetTime();
        }
    }
}

// ============================================================
//  MAIN
// ============================================================
int main() {
    if (!RequestResources(APP_NAME, RAM_MB, HDD_MB, PRIORITY_NORMAL, 1)) {
        InitWindow(440,120,"Weather - Denied"); SetTargetFPS(30);
        double t=GetTime();
        while (!WindowShouldClose()&&GetTime()-t<3.5) {
            BeginDrawing(); ClearBackground(BG_DEEP);
            DrawText("Insufficient resources.",18,40,FONT_NORMAL,NEON_PINK);
            EndDrawing();
        }
        CloseWindow(); return 1;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(WIN_W, WIN_H, "NexOS Weather");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    SetWindowFocused();

    srand((unsigned)time(nullptr));

    // Default cities
    cities.push_back(MakeCityWeather("Lahore",     300));
    cities.push_back(MakeCityWeather("London",     0));
    cities.push_back(MakeCityWeather("New York",  -300));
    cities.push_back(MakeCityWeather("Tokyo",      540));
    lastRefresh = GetTime();

    while (!WindowShouldClose() && appRunning) {
        int sw = GetScreenWidth(), sh = GetScreenHeight();
        animTime = GetTime();
        float dt = GetFrameTime();

        int sideW = 160;
        int tabH  = 38;
        Rectangle contentArea = {
            (float)sideW, (float)tabH,
            (float)(sw - sideW), (float)(sh - tabH)
        };

        // Spawn particles for current city condition
        if (!cities.empty())
            SpawnParticles(cities[selectedCity].condCode, sw, sh);

        // Mouse wheel scroll for hourly
        if (activeTab == 1) {
            float wh = GetMouseWheelMove();
            scrollOffset -= (int)(wh * 30);
            if (scrollOffset < 0) scrollOffset = 0;
        }

        BeginDrawing();
        ClearBackground(BG_DEEP);
        DrawCyberpunkGrid(sw, sh);

        if (!cities.empty()) {
            auto& cw = cities[selectedCity];
            if (activeTab == 0)      DrawCurrentPanel(contentArea, cw);
            else if (activeTab == 1) DrawHourlyPanel(contentArea, cw);
            else                     DrawWeeklyPanel(contentArea, cw);
        }

        // Particle effects on top of content
        UpdateDrawParticles(dt, sh);

        DrawCitySidebar(sw, sh, sideW);
        DrawTabBar(sideW, sw);
        DrawRefreshBtn(sw);

        if (addCityOpen) DrawAddCityModal(sw, sh);

        EndDrawing();
    }

    appRunning = false;
    ReleaseResources(APP_NAME, RAM_MB, HDD_MB);
    CloseWindow();
    return 0;
}