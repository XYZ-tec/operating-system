// clock.cpp — NexOS Clock App
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
#include <sys/stat.h>
#include <algorithm>
#include <vector>
#include <string>
#include <sstream>
#include <thread>
#include <atomic>
#include <mutex>

#define APP_NAME    "Clock"
#define RAM_MB      20
#define HDD_MB      1
#define WIN_W       960
#define WIN_H       680

static const char* STATE_PATH = "hdd/clock_state.txt";
static const int DAYS = 7;
static const char* DAY_NAMES[DAYS] = {"S", "M", "T", "W", "T", "F", "S"};

struct WorldClock {
    std::string name;
    int utcOffsetMinutes;
};

struct Alarm {
    int hour = 7;
    int minute = 0;
    bool enabled = true;
    int repeatMask = 0;
    int snoozeMinutes = 10;
    std::string label = "Alarm";
    bool ringing = false;
    int lastFireKey = -1;
    double snoozeUntil = 0.0;
    double nextBeepAt = 0.0;
};

struct TimerState {
    bool running = false;
    bool finished = false;
    double durationSeconds = 300.0;
    double remainingSeconds = 300.0;
    double startAt = 0.0;
};

struct StopwatchState {
    bool running = false;
    double elapsedSeconds = 0.0;
    double startAt = 0.0;
    std::vector<double> laps;
};

static std::vector<WorldClock> worldClocks;
static std::vector<Alarm> alarms;
static TimerState timerState;
static StopwatchState stopwatchState;
static int currentTab = 0;
static bool addCityModal = false;
static bool addAlarmModal = false;
static int editAlarmIndex = -1;
static std::string citySearch;
static std::string alarmLabelDraft = "Alarm";
static int modalHour = 7;
static int modalMinute = 0;
static int modalSnooze = 10;
static int modalRepeatMask = 0;
static int selectedCityIndex = -1;
static int activeRingIndex = -1;
static double lastPersistAt = 0.0;
static bool stateDirty = false;
static Sound alarmTone = {0};
static bool audioReady = false;
static bool toneLoaded = false;
static bool searchFocused = false;
static bool labelFocused = false;

// ============================================================
//  Background Thread State — for continuous alarm/timer updates
// ============================================================
static std::atomic<bool> bgThreadRunning(true);
static std::mutex bgMutex;

// ============================================================
//  Notification System — alert user when alarm/timer triggers
// ============================================================
static std::atomic<int> activeNotificationIndex(-1);
static std::atomic<bool> showNotificationModal(false);
static std::atomic<bool> needsAudio(false);

static const struct CityInfo { const char* name; int utcOffsetMinutes; } CITY_DB[] = {
    {"Local Time", 0},
    {"London", 0},
    {"Lisbon", 0},
    {"Reykjavik", 0},
    {"Paris", 60},
    {"Berlin", 60},
    {"Rome", 60},
    {"Cairo", 120},
    {"Dubai", 240},
    {"Karachi", 300},
    {"Lahore", 300},
    {"Delhi", 330},
    {"Bangkok", 420},
    {"Singapore", 480},
    {"Tokyo", 540},
    {"Sydney", 600},
    {"Honolulu", -600},
    {"Los Angeles", -480},
    {"Denver", -420},
    {"Chicago", -360},
    {"New York", -300},
    {"Toronto", -300},
    {"Rio de Janeiro", -180},
    {"Buenos Aires", -180}
};

static int CurrentUtcOffsetMinutes() {
    time_t now = time(nullptr);
    tm localTm = *localtime(&now);
    tm gmtTm = *gmtime(&now);
    return (int)difftime(mktime(&localTm), mktime(&gmtTm)) / 60;
}

static int DayOfWeekIndex(time_t t) {
    tm localTm = *localtime(&t);
    return localTm.tm_wday;
}

static int DateKey(time_t t) {
    tm localTm = *localtime(&t);
    return (localTm.tm_year + 1900) * 1000 + localTm.tm_yday;
}

static std::string FormatTimeLocal(time_t t, const char* fmt = "%H:%M:%S") {
    char buffer[64];
    tm localTm = *localtime(&t);
    strftime(buffer, sizeof(buffer), fmt, &localTm);
    return std::string(buffer);
}

static std::string FormatDuration(double totalSeconds) {
    int seconds = (int)std::max(0.0, std::round(totalSeconds));
    int hours = seconds / 3600;
    seconds %= 3600;
    int minutes = seconds / 60;
    int secs = seconds % 60;
    char buffer[64];
    if (hours > 0) snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", hours, minutes, secs);
    else snprintf(buffer, sizeof(buffer), "%02d:%02d", minutes, secs);
    return std::string(buffer);
}

static std::string FormatShortDuration(int totalMinutes) {
    int hours = totalMinutes / 60;
    int minutes = totalMinutes % 60;
    char buffer[32];
    if (hours > 0 && minutes > 0) snprintf(buffer, sizeof(buffer), "%dh %dm", hours, minutes);
    else if (hours > 0) snprintf(buffer, sizeof(buffer), "%dh", hours);
    else snprintf(buffer, sizeof(buffer), "%dm", minutes);
    return std::string(buffer);
}

static std::string RepeatSummary(int mask) {
    if (mask == 0) return "Once";
    std::string text;
    for (int i = 0; i < DAYS; ++i) {
        if (mask & (1 << i)) {
            if (!text.empty()) text += " ";
            text += DAY_NAMES[i];
        }
    }
    return text;
}

static std::string ToLower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

static bool ContainsInsensitive(const std::string& text, const std::string& query) {
    if (query.empty()) return true;
    return ToLower(text).find(ToLower(query)) != std::string::npos;
}

static void HandleTextInput(std::string& text, int maxLen, bool focused, bool allowSpaces = true) {
    if (!focused) return;
    int key = GetCharPressed();
    while (key > 0) {
        bool ok = (key >= 'a' && key <= 'z') || (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') || key == '-' || key == '_' || key == ',' || key == '.' || key == '\'';
        if (allowSpaces && key == ' ') ok = true;
        if (ok && (int)text.size() < maxLen) text.push_back((char)key);
        key = GetCharPressed();
    }
    if (IsKeyPressed(KEY_BACKSPACE) && !text.empty()) text.pop_back();
}

static bool DrawToggle(Rectangle r, bool value) {
    bool hovered = CheckCollisionPointRec(GetMousePosition(), r);
    Color fill = value ? NEON_GREEN : BG_HOVER;
    DrawRectangleRounded(r, 0.45f, 10, fill);
    DrawRectangleLinesEx(r, 1.0f, hovered ? NEON_CYAN : BORDER_DIM);
    float knobX = value ? (r.x + r.width - r.height + 2) : (r.x + 2);
    DrawCircle((int)(knobX + (r.height - 4) / 2), (int)(r.y + r.height / 2), (r.height - 6) / 2, RAYWHITE);
    return hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
}

static void InitAlarmTone() {
    if (toneLoaded) return;
    if (!IsAudioDeviceReady()) InitAudioDevice();
    audioReady = IsAudioDeviceReady();
    if (!audioReady) return;

    const int sampleRate = 44100;
    const float seconds = 0.35f;
    const float freq = 880.0f;
    int sampleCount = (int)(sampleRate * seconds);
    short* samples = (short*)MemAlloc(sampleCount * sizeof(short));
    if (!samples) return;

    for (int i = 0; i < sampleCount; ++i) {
        float t = (float)i / (float)sampleRate;
        float envelope = 0.8f * (1.0f - t / seconds);
        if (envelope < 0.15f) envelope = 0.15f;
        samples[i] = (short)(sinf(2.0f * PI * freq * t) * 30000.0f * envelope);
    }

    Wave wave = {0};
    wave.frameCount = sampleCount;
    wave.sampleRate = sampleRate;
    wave.sampleSize = 16;
    wave.channels = 1;
    wave.data = samples;
    alarmTone = LoadSoundFromWave(wave);
    UnloadWave(wave);

    if (alarmTone.stream.buffer != NULL) {
        SetSoundVolume(alarmTone, 0.65f);
        toneLoaded = true;
    }
}

static void PlayAlarmTone() {
    if (!toneLoaded) InitAlarmTone();
    if (toneLoaded) PlaySound(alarmTone);
}

static void SaveState() {
    mkdir("hdd", 0755);
    FILE* f = fopen(STATE_PATH, "w");
    if (!f) return;
    fprintf(f, "WORLD %zu\n", worldClocks.size());
    for (const auto& w : worldClocks) fprintf(f, "%s|%d\n", w.name.c_str(), w.utcOffsetMinutes);
    fprintf(f, "ALARMS %zu\n", alarms.size());
    for (const auto& a : alarms) {
        fprintf(f, "%d|%d|%d|%d|%d|%s\n", a.hour, a.minute, a.enabled ? 1 : 0, a.repeatMask, a.snoozeMinutes, a.label.c_str());
    }
    fprintf(f, "TIMER %d|%.3f|%.3f|%d\n", timerState.running ? 1 : 0, timerState.durationSeconds, timerState.remainingSeconds, timerState.finished ? 1 : 0);
    fprintf(f, "STOPWATCH %d|%.3f|%zu\n", stopwatchState.running ? 1 : 0, stopwatchState.elapsedSeconds, stopwatchState.laps.size());
    for (double lap : stopwatchState.laps) fprintf(f, "LAP|%.3f\n", lap);
    fclose(f);
    stateDirty = false;
    lastPersistAt = GetTime();
}

static std::vector<std::string> Split(const std::string& s, char delim) {
    std::vector<std::string> parts;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, delim)) parts.push_back(item);
    return parts;
}

static void LoadState() {
    FILE* f = fopen(STATE_PATH, "r");
    if (!f) return;
    worldClocks.clear();
    alarms.clear();
    stopwatchState.laps.clear();
    char line[512];
    enum class Section { None, World, Alarms, Timer, Stopwatch } section = Section::None;
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.rfind("WORLD ", 0) == 0) { section = Section::World; continue; }
        if (s.rfind("ALARMS ", 0) == 0) { section = Section::Alarms; continue; }
        if (s.rfind("TIMER ", 0) == 0) { section = Section::Timer; continue; }
        if (s.rfind("STOPWATCH ", 0) == 0) { section = Section::Stopwatch; continue; }
        if (s.rfind("LAP|", 0) == 0) {
            auto parts = Split(s, '|');
            if (parts.size() >= 2) stopwatchState.laps.push_back(atof(parts[1].c_str()));
            continue;
        }
        if (s.empty()) continue;
        auto parts = Split(s, '|');
        if (section == Section::World && parts.size() >= 2) {
            worldClocks.push_back({parts[0], atoi(parts[1].c_str())});
        } else if (section == Section::Alarms && parts.size() >= 6) {
            Alarm a;
            a.hour = atoi(parts[0].c_str());
            a.minute = atoi(parts[1].c_str());
            a.enabled = atoi(parts[2].c_str()) != 0;
            a.repeatMask = atoi(parts[3].c_str());
            a.snoozeMinutes = atoi(parts[4].c_str());
            a.label = parts[5];
            alarms.push_back(a);
        } else if (section == Section::Timer && parts.size() >= 4) {
            timerState.running = atoi(parts[0].c_str()) != 0;
            timerState.durationSeconds = atof(parts[1].c_str());
            timerState.remainingSeconds = atof(parts[2].c_str());
            timerState.finished = atoi(parts[3].c_str()) != 0;
        } else if (section == Section::Stopwatch && parts.size() >= 3) {
            stopwatchState.running = atoi(parts[0].c_str()) != 0;
            stopwatchState.elapsedSeconds = atof(parts[1].c_str());
        }
    }
    fclose(f);
}

static void EnsureDefaults() {
    if (worldClocks.empty()) {
        worldClocks.push_back({"Local Time", CurrentUtcOffsetMinutes()});
        worldClocks.push_back({"London", 0});
        worldClocks.push_back({"Lahore", 300});
    }
    if (alarms.empty()) {
        Alarm a;
        a.hour = 7;
        a.minute = 30;
        a.enabled = true;
        a.repeatMask = 0;
        a.snoozeMinutes = 10;
        a.label = "Wake up";
        alarms.push_back(a);
    }
}

static std::string CityOffsetText(int minutes) {
    int hours = minutes / 60;
    int mins = std::abs(minutes % 60);
    char buf[32];
    snprintf(buf, sizeof(buf), "UTC%+d:%02d", hours, mins);
    return std::string(buf);
}

static void DrawSectionTitle(const char* text, Rectangle area) {
    int w = MeasureText(text, FONT_LARGE);
    DrawText(text, (int)(area.x + (area.width - w) / 2), (int)area.y + 16, FONT_LARGE, TEXT_PRIMARY);
}

static void DrawTopTabs(int sw) {
    const char* labels[] = {"World", "Alarms", "Stopwatch", "Timer"};
    int tabW = 130;
    int gap = 12;
    int total = tabW * 4 + gap * 3;
    int x = (sw - total) / 2;
    for (int i = 0; i < 4; ++i) {
        Rectangle r = {(float)(x + i * (tabW + gap)), 12, (float)tabW, 34};
        bool active = currentTab == i;
        Color bg = active ? BG_HOVER : BG_PANEL;
        DrawRectangleRounded(r, 0.35f, 8, bg);
        DrawRectangleLinesEx(r, 1.0f, active ? NEON_CYAN : BORDER_DIM);
        int tw = MeasureText(labels[i], FONT_SMALL);
        DrawText(labels[i], (int)(r.x + (r.width - tw) / 2), (int)(r.y + 8), FONT_SMALL, active ? NEON_CYAN : TEXT_PRIMARY);
        if (CheckCollisionPointRec(GetMousePosition(), r) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) currentTab = i;
    }
}

static void DrawWorldTab(Rectangle content, int sw) {
    time_t now = time(nullptr);
    DrawSectionTitle("World Clock", {content.x, content.y, content.width, 0});
    std::string local = FormatTimeLocal(now, "%H:%M:%S");
    int tW = MeasureText(local.c_str(), FONT_LARGE);
    DrawText(local.c_str(), (int)(content.x + (content.width - tW) / 2), (int)content.y + 54, FONT_LARGE, NEON_CYAN);

    std::string date = FormatTimeLocal(now, "%A, %B %d");
    int dW = MeasureText(date.c_str(), FONT_SMALL);
    DrawText(date.c_str(), (int)(content.x + (content.width - dW) / 2), (int)content.y + 92, FONT_SMALL, TEXT_MUTED);

    Rectangle listArea = {content.x + 32, content.y + 138, content.width - 64, content.height - 200};
    DrawRectangleRounded(listArea, 0.08f, 8, BG_PANEL);
    DrawRectangleLinesEx(listArea, 1.0f, BORDER_DIM);
    DrawText("World Clocks", (int)listArea.x + 16, (int)listArea.y + 12, FONT_SMALL, TEXT_MUTED);
    Rectangle addBtn = {listArea.x + listArea.width - 110, listArea.y + 8, 94, 28};
    if (DrawButton(addBtn, "Add City", BG_HOVER, NEON_CYAN, FONT_TINY)) {
        addCityModal = true;
        citySearch.clear();
        selectedCityIndex = -1;
        searchFocused = true;
        labelFocused = false;
    }

    int rowY = (int)listArea.y + 48;
    for (size_t i = 0; i < worldClocks.size(); ++i) {
        Rectangle row = {listArea.x + 12, (float)rowY, listArea.width - 24, 54};
        DrawRectangleRounded(row, 0.16f, 8, BG_HOVER);
        DrawRectangleLinesEx(row, 1.0f, BORDER_DIM);
        DrawText(worldClocks[i].name.c_str(), (int)row.x + 14, (int)row.y + 10, FONT_NORMAL, TEXT_PRIMARY);
        std::string offset = CityOffsetText(worldClocks[i].utcOffsetMinutes);
        DrawText(offset.c_str(), (int)row.x + 14, (int)row.y + 30, FONT_TINY, TEXT_MUTED);
        time_t cityTime = now + (worldClocks[i].utcOffsetMinutes - CurrentUtcOffsetMinutes()) * 60;
        std::string timeStr = FormatTimeLocal(cityTime, "%H:%M");
        int timeW = MeasureText(timeStr.c_str(), FONT_LARGE);
        DrawText(timeStr.c_str(), (int)(row.x + row.width - timeW - 44), (int)row.y + 14, FONT_LARGE, NEON_GOLD);
        Rectangle del = {row.x + row.width - 28, row.y + 13, 18, 18};
        if (DrawButton(del, "x", BG_DEEP, NEON_PINK, FONT_SMALL) && worldClocks.size() > 1) {
            worldClocks.erase(worldClocks.begin() + i);
            stateDirty = true;
            break;
        }
        rowY += 64;
    }

    DrawText("Mouse only: use Add City, then click a city from the list.", (int)content.x + 32, (int)(content.y + content.height - 30), FONT_TINY, TEXT_DIM);
}

static void DrawStopwatchTab(Rectangle content) {
    DrawSectionTitle("Stopwatch", content);
    // Only add current session if running; otherwise just use saved elapsed time
    double display = stopwatchState.elapsedSeconds;
    if (stopwatchState.running) {
        display += (GetTime() - stopwatchState.startAt);
    }
    std::string s = FormatDuration(display);
    int w = MeasureText(s.c_str(), FONT_TITLE);
    DrawText(s.c_str(), (int)(content.x + (content.width - w) / 2), (int)content.y + 78, FONT_TITLE, NEON_CYAN);

    Rectangle controls = {content.x + (content.width - 360) / 2, content.y + 148, 360, 52};
    Rectangle startStop = {controls.x, controls.y, 110, 34};
    Rectangle lapBtn = {controls.x + 125, controls.y, 90, 34};
    Rectangle resetBtn = {controls.x + 230, controls.y, 130, 34};
    if (DrawButton(startStop, stopwatchState.running ? "Pause" : "Start", BG_HOVER, stopwatchState.running ? NEON_ORANGE : NEON_GREEN, FONT_SMALL)) {
        if (stopwatchState.running) {
            stopwatchState.elapsedSeconds += GetTime() - stopwatchState.startAt;
            stopwatchState.running = false;
        } else {
            stopwatchState.startAt = GetTime();
            stopwatchState.running = true;
        }
        stateDirty = true;
    }
    if (DrawButton(lapBtn, "Lap", BG_HOVER, NEON_CYAN, FONT_SMALL)) {
        if (display > 0.01) stopwatchState.laps.push_back(display);
        stateDirty = true;
    }
    if (DrawButton(resetBtn, "Reset", BG_HOVER, NEON_PINK, FONT_SMALL)) {
        stopwatchState.running = false;
        stopwatchState.elapsedSeconds = 0.0;
        stopwatchState.laps.clear();
        stateDirty = true;
    }

    Rectangle lapArea = {content.x + 40, content.y + 220, content.width - 80, content.height - 260};
    DrawRectangleRounded(lapArea, 0.12f, 8, BG_PANEL);
    DrawRectangleLinesEx(lapArea, 1.0f, BORDER_DIM);
    DrawText("Laps", (int)lapArea.x + 14, (int)lapArea.y + 12, FONT_SMALL, TEXT_MUTED);
    int y = (int)lapArea.y + 42;
    for (int i = (int)stopwatchState.laps.size() - 1; i >= 0 && y < (int)(lapArea.y + lapArea.height - 20); --i) {
        char label[64];
        snprintf(label, sizeof(label), "Lap %d", (int)stopwatchState.laps.size() - i);
        DrawText(label, (int)lapArea.x + 14, y, FONT_SMALL, TEXT_MUTED);
        std::string lapTime = FormatDuration(stopwatchState.laps[i]);
        int lapW = MeasureText(lapTime.c_str(), FONT_NORMAL);
        DrawText(lapTime.c_str(), (int)(lapArea.x + lapArea.width - lapW - 16), y - 2, FONT_NORMAL, TEXT_PRIMARY);
        y += 28;
    }
}

static void ApplyTimerDuration(int seconds) {
    timerState.durationSeconds = std::max(1, seconds);
    timerState.remainingSeconds = timerState.durationSeconds;
    timerState.running = false;
    timerState.finished = false;
    stateDirty = true;
}

static void DrawTimerTab(Rectangle content) {
    DrawSectionTitle("Timer", content);
    Rectangle quickArea = {content.x + 28, content.y + 54, content.width - 56, 92};
    DrawRectangleRounded(quickArea, 0.12f, 8, BG_PANEL);
    DrawRectangleLinesEx(quickArea, 1.0f, BORDER_DIM);
    DrawText("Quick Start", (int)quickArea.x + 16, (int)quickArea.y + 12, FONT_SMALL, TEXT_MUTED);
    const int presetVals[] = {60, 120, 180, 300, 900, 1800, 2700, 3600};
    const char* presetLabels[] = {"1m", "2m", "3m", "5m", "15m", "30m", "45m", "1h"};
    for (int i = 0; i < 8; ++i) {
        int col = i % 4;
        int row = i / 4;
        Rectangle b = {quickArea.x + 16 + col * 92, quickArea.y + 38 + row * 30, 72, 24};
        if (DrawButton(b, presetLabels[i], BG_HOVER, TEXT_PRIMARY, FONT_SMALL)) ApplyTimerDuration(presetVals[i]);
    }

    Rectangle mainArea = {content.x + 28, content.y + 166, content.width - 56, content.height - 210};
    DrawRectangleRounded(mainArea, 0.12f, 8, BG_PANEL);
    DrawRectangleLinesEx(mainArea, 1.0f, BORDER_DIM);
    DrawText("Set Timer", (int)mainArea.x + 16, (int)mainArea.y + 12, FONT_SMALL, TEXT_MUTED);

    double currentRemaining = timerState.remainingSeconds;
    if (timerState.running) currentRemaining = std::max(0.0, timerState.durationSeconds - (GetTime() - timerState.startAt));
    if (!timerState.running && timerState.finished) currentRemaining = 0.0;
    if (timerState.running && currentRemaining <= 0.0) {
        timerState.running = false;
        timerState.finished = true;
        timerState.remainingSeconds = 0.0;
        PlayAlarmTone();
    }

    std::string timerText = FormatDuration(currentRemaining);
    int timerW = MeasureText(timerText.c_str(), FONT_TITLE);
    DrawText(timerText.c_str(), (int)(mainArea.x + (mainArea.width - timerW) / 2), (int)mainArea.y + 72, FONT_TITLE, NEON_ORANGE);

    int hours = (int)timerState.durationSeconds / 3600;
    int minutes = ((int)timerState.durationSeconds % 3600) / 60;
    int seconds = (int)timerState.durationSeconds % 60;
    float pickX = mainArea.x + (mainArea.width - 390) / 2;
    Rectangle hourBox = {pickX, mainArea.y + 132, 106, 34};
    Rectangle minuteBox = {pickX + 142, mainArea.y + 132, 106, 34};
    Rectangle secondBox = {pickX + 284, mainArea.y + 132, 106, 34};

    auto drawStepper = [&](Rectangle r, const char* label, int value) {
        DrawRectangleRounded(r, 0.12f, 8, BG_HOVER);
        DrawRectangleLinesEx(r, 1.0f, BORDER_DIM);
        Rectangle up = {r.x + r.width - 28, r.y + 2, 24, 14};
        Rectangle dn = {r.x + r.width - 28, r.y + 18, 24, 14};
        DrawText(label, (int)r.x + 10, (int)r.y + 9, FONT_SMALL, TEXT_MUTED);
        char buf[16]; snprintf(buf, sizeof(buf), "%02d", value);
        DrawText(buf, (int)r.x + 46, (int)r.y + 7, FONT_LARGE, TEXT_PRIMARY);
        if (DrawButton(up, "+", BG_PANEL, NEON_CYAN, FONT_TINY)) return 1;
        if (DrawButton(dn, "-", BG_PANEL, NEON_CYAN, FONT_TINY)) return -1;
        return 0;
    };

    int hDelta = drawStepper(hourBox, "Hours", hours);
    int mDelta = drawStepper(minuteBox, "Minutes", minutes);
    int sDelta = drawStepper(secondBox, "Seconds", seconds);
    if (hDelta || mDelta || sDelta) {
        int total = std::max(0, hours * 3600 + minutes * 60 + seconds + hDelta * 3600 + mDelta * 60 + sDelta);
        ApplyTimerDuration(total);
    }

    Rectangle startBtn = {mainArea.x + 80, mainArea.y + 192, 110, 36};
    Rectangle pauseBtn = {mainArea.x + 200, mainArea.y + 192, 110, 36};
    Rectangle resetBtn = {mainArea.x + 320, mainArea.y + 192, 110, 36};
    if (DrawButton(startBtn, timerState.running ? "Running" : "Start", timerState.running ? BG_HOVER : BG_HOVER, timerState.running ? NEON_GREEN : NEON_CYAN, FONT_SMALL)) {
        if (!timerState.running && timerState.remainingSeconds <= 0.0) timerState.remainingSeconds = timerState.durationSeconds;
        if (!timerState.running) {
            timerState.startAt = GetTime();
            timerState.running = true;
            timerState.finished = false;
        }
        stateDirty = true;
    }
    if (DrawButton(pauseBtn, "Pause", BG_HOVER, NEON_ORANGE, FONT_SMALL)) {
        if (timerState.running) {
            timerState.remainingSeconds = std::max(0.0, timerState.durationSeconds - (GetTime() - timerState.startAt));
            timerState.running = false;
            stateDirty = true;
        }
    }
    if (DrawButton(resetBtn, "Reset", BG_HOVER, NEON_PINK, FONT_SMALL)) {
        timerState.running = false;
        timerState.finished = false;
        timerState.remainingSeconds = timerState.durationSeconds;
        stateDirty = true;
    }

    if (timerState.finished) {
        Rectangle done = {mainArea.x + 80, mainArea.y + 246, mainArea.width - 160, 44};
        DrawRectangleRounded(done, 0.2f, 8, Color{40, 18, 18, 255});
        DrawRectangleLinesEx(done, 1.0f, NEON_PINK);
        DrawText("Timer complete", (int)done.x + 16, (int)done.y + 13, FONT_SMALL, NEON_PINK);
        if (DrawButton({done.x + done.width - 100, done.y + 8, 84, 28}, "Dismiss", BG_HOVER, NEON_PINK, FONT_TINY)) {
            timerState.finished = false;
        }
    }
}

static void DrawDayChips(Rectangle r, int& mask, int yOffset = 0) {
    int chipW = 28;
    int gap = 8;
    for (int i = 0; i < DAYS; ++i) {
        Rectangle chip = {r.x + i * (chipW + gap), r.y + yOffset, (float)chipW, 24};
        bool active = (mask & (1 << i)) != 0;
        bool clicked = DrawButton(chip, DAY_NAMES[i], active ? NEON_CYAN : BG_HOVER, active ? BG_DEEP : TEXT_PRIMARY, FONT_SMALL);
        if (clicked) {
            mask ^= (1 << i);
            stateDirty = true;
        }
    }
}

static void BeginAlarmEditor(int index) {
    editAlarmIndex = index;
    searchFocused = false;
    labelFocused = true;
    if (index >= 0 && index < (int)alarms.size()) {
        modalHour = alarms[index].hour;
        modalMinute = alarms[index].minute;
        modalSnooze = alarms[index].snoozeMinutes;
        modalRepeatMask = alarms[index].repeatMask;
        alarmLabelDraft = alarms[index].label;
    } else {
        modalHour = 7;
        modalMinute = 0;
        modalSnooze = 10;
        modalRepeatMask = 0;
        alarmLabelDraft = "Alarm";
    }
    addAlarmModal = true;
}

static void DrawAddCityModal(int sw, int sh) {
    DrawRectangle(0, 0, sw, sh, Color{0, 0, 0, 190});
    Rectangle box = {(float)(sw / 2 - 360), (float)(sh / 2 - 220), 720, 440};
    DrawRectangleRounded(box, 0.06f, 8, BG_PANEL);
    DrawRectangleLinesEx(box, 1.0f, NEON_CYAN);
    DrawText("Add a City", (int)box.x + 20, (int)box.y + 18, FONT_LARGE, NEON_CYAN);

    Rectangle search = {box.x + 20, box.y + 58, box.width - 40, 34};
    DrawRectangleRounded(search, 0.16f, 8, BG_DEEP);
    DrawRectangleLinesEx(search, 1.2f, searchFocused ? NEON_CYAN : BORDER_DIM);
    DrawText(citySearch.c_str(), (int)search.x + 10, (int)search.y + 8, FONT_SMALL, TEXT_PRIMARY);
    if ((int)(GetTime() * 2) % 2 == 0) {
        int w = MeasureText(citySearch.c_str(), FONT_SMALL);
        DrawText("|", (int)search.x + 10 + w, (int)search.y + 8, FONT_SMALL, NEON_CYAN);
    }

    if (CheckCollisionPointRec(GetMousePosition(), search) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        searchFocused = true;
        labelFocused = false;
    }
    HandleTextInput(citySearch, 28, searchFocused, true);
    if (searchFocused && IsKeyPressed(KEY_ENTER) && !citySearch.empty()) {
        for (const auto& c : CITY_DB) {
            if (ContainsInsensitive(c.name, citySearch)) {
                worldClocks.push_back({c.name, c.utcOffsetMinutes});
                stateDirty = true;
                addCityModal = false;
                searchFocused = false;
                break;
            }
        }
    }

    Rectangle results = {box.x + 360, box.y + 58, box.width - 380, 300};
    DrawRectangleRounded(results, 0.08f, 8, BG_HOVER);
    DrawRectangleLinesEx(results, 1.0f, BORDER_DIM);
    DrawText("Cities", (int)results.x + 12, (int)results.y + 10, FONT_SMALL, TEXT_MUTED);

    std::vector<CityInfo> matches;
    for (const auto& c : CITY_DB) {
        if (ContainsInsensitive(c.name, citySearch)) matches.push_back(c);
    }
    int y = (int)results.y + 38;
    for (size_t i = 0; i < matches.size() && i < 6; ++i) {
        Rectangle row = {results.x + 10, (float)y, results.width - 20, 30};
        bool hovered = CheckCollisionPointRec(GetMousePosition(), row);
        if (hovered) DrawRectangleRec(row, BG_PANEL);
        DrawText(matches[i].name, (int)row.x + 10, (int)row.y + 7, FONT_SMALL, TEXT_PRIMARY);
        DrawText(CityOffsetText(matches[i].utcOffsetMinutes).c_str(), (int)row.x + row.width - 92, (int)row.y + 7, FONT_TINY, TEXT_MUTED);
        Rectangle add = {row.x + row.width - 44, row.y + 4, 34, 22};
        if (DrawButton(add, "Add", hovered ? NEON_CYAN : BG_HOVER, hovered ? BG_DEEP : TEXT_PRIMARY, FONT_TINY)) {
            worldClocks.push_back({matches[i].name, matches[i].utcOffsetMinutes});
            stateDirty = true;
            addCityModal = false;
            searchFocused = false;
        }
        if (hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) selectedCityIndex = (int)i;
        y += 36;
    }

    Rectangle cancel = {box.x + box.width - 120, box.y + box.height - 44, 100, 30};
    if (DrawButton(cancel, "Cancel", BG_HOVER, NEON_PINK, FONT_SMALL)) { addCityModal = false; searchFocused = false; }
}

static void DrawAlarmEditorModal(int sw, int sh) {
    DrawRectangle(0, 0, sw, sh, Color{0, 0, 0, 190});
    Rectangle box = {(float)(sw / 2 - 400), (float)(sh / 2 - 280), 800, 560};
    DrawRectangleRounded(box, 0.06f, 8, BG_PANEL);
    DrawRectangleLinesEx(box, 1.0f, NEON_CYAN);
    DrawText(editAlarmIndex >= 0 ? "Edit Alarm" : "New Alarm", (int)box.x + 20, (int)box.y + 18, FONT_LARGE, NEON_CYAN);

    // ========== TIME PICKER (FULL WIDTH, BETTER SPACING) ==========
    Rectangle timeCard = {box.x + 20, box.y + 62, box.width - 40, 100};
    DrawRectangleRounded(timeCard, 0.12f, 8, BG_HOVER);
    DrawRectangleLinesEx(timeCard, 1.0f, BORDER_DIM);
    DrawText("Time (24h)", (int)timeCard.x + 12, (int)timeCard.y + 8, FONT_SMALL, TEXT_MUTED);
    
    // Hour and minute controls with better spacing
    float timeControlY = timeCard.y + 32;
    float timeControlX = timeCard.x + 40;
    float controlSpacing = 140;
    
    // Hour control
    Rectangle hourUp = {timeControlX, timeControlY, 48, 20};
    Rectangle hourDisplay = {timeControlX + 4, timeControlY + 22, 40, 38};
    Rectangle hourDown = {timeControlX, timeControlY + 62, 48, 20};
    
    // Minute control
    Rectangle minUp = {timeControlX + controlSpacing, timeControlY, 48, 20};
    Rectangle minDisplay = {timeControlX + controlSpacing + 4, timeControlY + 22, 40, 38};
    Rectangle minDown = {timeControlX + controlSpacing, timeControlY + 62, 48, 20};
    
    // Draw hour controls
    if (DrawButton(hourUp, "+", BG_PANEL, NEON_CYAN, FONT_SMALL)) modalHour = (modalHour + 1) % 24;
    DrawRectangleRounded(hourDisplay, 0.08f, 8, BG_DEEP);
    DrawRectangleLinesEx(hourDisplay, 1.0f, BORDER_DIM);
    char hourBuf[8];
    snprintf(hourBuf, sizeof(hourBuf), "%02d", modalHour);
    int hourW = MeasureText(hourBuf, FONT_TITLE);
    DrawText(hourBuf, (int)(hourDisplay.x + (hourDisplay.width - hourW) / 2), (int)hourDisplay.y + 4, FONT_TITLE, NEON_GOLD);
    if (DrawButton(hourDown, "-", BG_PANEL, NEON_CYAN, FONT_SMALL)) modalHour = (modalHour + 23) % 24;
    
    // Draw colon separator
    int colonX = (int)(timeControlX + controlSpacing - 18);
    DrawText(":", colonX, (int)(timeControlY + 35), FONT_TITLE, TEXT_PRIMARY);
    
    // Draw minute controls
    if (DrawButton(minUp, "+", BG_PANEL, NEON_CYAN, FONT_SMALL)) modalMinute = (modalMinute + 1) % 60;
    DrawRectangleRounded(minDisplay, 0.08f, 8, BG_DEEP);
    DrawRectangleLinesEx(minDisplay, 1.0f, BORDER_DIM);
    char minBuf[8];
    snprintf(minBuf, sizeof(minBuf), "%02d", modalMinute);
    int minW = MeasureText(minBuf, FONT_TITLE);
    DrawText(minBuf, (int)(minDisplay.x + (minDisplay.width - minW) / 2), (int)minDisplay.y + 4, FONT_TITLE, NEON_GOLD);
    if (DrawButton(minDown, "-", BG_PANEL, NEON_CYAN, FONT_SMALL)) modalMinute = (modalMinute + 59) % 60;
    
    // ========== LABEL (FULL WIDTH) ==========
    Rectangle labelCard = {box.x + 20, box.y + 178, box.width - 40, 94};
    DrawRectangleRounded(labelCard, 0.12f, 8, BG_HOVER);
    DrawRectangleLinesEx(labelCard, 1.0f, BORDER_DIM);
    DrawText("Label", (int)labelCard.x + 12, (int)labelCard.y + 8, FONT_SMALL, TEXT_MUTED);
    Rectangle labelBox = {labelCard.x + 12, labelCard.y + 30, labelCard.width - 24, 32};
    DrawRectangleRounded(labelBox, 0.14f, 8, BG_DEEP);
    DrawRectangleLinesEx(labelBox, 1.2f, labelFocused ? NEON_CYAN : BORDER_DIM);
    DrawText(alarmLabelDraft.c_str(), (int)labelBox.x + 10, (int)labelBox.y + 7, FONT_SMALL, TEXT_PRIMARY);
    if ((int)(GetTime() * 2) % 2 == 0 && labelFocused) {
        int cursorW = MeasureText(alarmLabelDraft.c_str(), FONT_SMALL);
        DrawText("|", (int)labelBox.x + 10 + cursorW, (int)labelBox.y + 7, FONT_SMALL, NEON_CYAN);
    }
    if (CheckCollisionPointRec(GetMousePosition(), labelBox) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        labelFocused = true;
        searchFocused = false;
    }
    HandleTextInput(alarmLabelDraft, 28, labelFocused, true);

    // ========== REPEAT (LEFT COLUMN) ==========
    Rectangle repeatCard = {box.x + 20, box.y + 290, (box.width - 40) / 2 - 6, 110};
    DrawRectangleRounded(repeatCard, 0.12f, 8, BG_HOVER);
    DrawRectangleLinesEx(repeatCard, 1.0f, BORDER_DIM);
    DrawText("Repeat", (int)repeatCard.x + 12, (int)repeatCard.y + 10, FONT_SMALL, TEXT_MUTED);
    Rectangle chips = {repeatCard.x + 8, repeatCard.y + 32, repeatCard.width - 16, 26};
    DrawDayChips(chips, modalRepeatMask);
    DrawText("No days = once", (int)repeatCard.x + 12, (int)repeatCard.y + 68, FONT_TINY, TEXT_DIM);

    // ========== SNOOZE DURATION (RIGHT COLUMN) ==========
    Rectangle snoozeCard = {box.x + (box.width - 40) / 2 + 6, box.y + 290, (box.width - 40) / 2 - 6, 110};
    DrawRectangleRounded(snoozeCard, 0.12f, 8, BG_HOVER);
    DrawRectangleLinesEx(snoozeCard, 1.0f, BORDER_DIM);
    DrawText("Snooze", (int)snoozeCard.x + 12, (int)snoozeCard.y + 10, FONT_SMALL, TEXT_MUTED);
    
    Rectangle snoozeMinus = {snoozeCard.x + 16, snoozeCard.y + 36, 36, 26};
    Rectangle snoozeDisplay = {snoozeCard.x + 58, snoozeCard.y + 36, 68, 26};
    Rectangle snoozePlus = {snoozeCard.x + snoozeCard.width - 52, snoozeCard.y + 36, 36, 26};
    
    if (DrawButton(snoozeMinus, "-", BG_PANEL, NEON_CYAN, FONT_SMALL)) modalSnooze = std::max(1, modalSnooze - 1);
    DrawRectangleRounded(snoozeDisplay, 0.08f, 8, BG_DEEP);
    DrawRectangleLinesEx(snoozeDisplay, 1.0f, BORDER_DIM);
    std::string snoozeTxt = FormatShortDuration(modalSnooze);
    int snoozeW = MeasureText(snoozeTxt.c_str(), FONT_NORMAL);
    DrawText(snoozeTxt.c_str(), (int)(snoozeDisplay.x + (snoozeDisplay.width - snoozeW) / 2), (int)snoozeDisplay.y + 4, FONT_NORMAL, TEXT_PRIMARY);
    if (DrawButton(snoozePlus, "+", BG_PANEL, NEON_CYAN, FONT_SMALL)) modalSnooze = std::min(60, modalSnooze + 1);
    
    DrawText("Duration", (int)snoozeCard.x + 12, (int)(snoozeCard.y + 68), FONT_TINY, TEXT_DIM);

    // ========== BUTTONS ==========
    Rectangle save = {box.x + box.width - 230, box.y + box.height - 42, 100, 32};
    Rectangle cancel = {box.x + box.width - 116, box.y + box.height - 42, 96, 32};
    
    if (DrawButton(save, "Save", BG_HOVER, NEON_CYAN, FONT_SMALL)) {
        Alarm a;
        a.hour = modalHour;
        a.minute = modalMinute;
        a.enabled = true;
        a.repeatMask = modalRepeatMask;
        a.snoozeMinutes = modalSnooze;
        a.label = alarmLabelDraft.empty() ? "Alarm" : alarmLabelDraft;
        if (editAlarmIndex >= 0 && editAlarmIndex < (int)alarms.size()) alarms[editAlarmIndex] = a;
        else alarms.push_back(a);
        addAlarmModal = false;
        editAlarmIndex = -1;
        stateDirty = true;
        labelFocused = false;
    }
    if (DrawButton(cancel, "Cancel", BG_HOVER, NEON_PINK, FONT_SMALL)) {
        addAlarmModal = false;
        editAlarmIndex = -1;
        labelFocused = false;
    }

    if (labelFocused && IsKeyPressed(KEY_ENTER)) {
        Alarm a;
        a.hour = modalHour;
        a.minute = modalMinute;
        a.enabled = true;
        a.repeatMask = modalRepeatMask;
        a.snoozeMinutes = modalSnooze;
        a.label = alarmLabelDraft.empty() ? "Alarm" : alarmLabelDraft;
        if (editAlarmIndex >= 0 && editAlarmIndex < (int)alarms.size()) alarms[editAlarmIndex] = a;
        else alarms.push_back(a);
        addAlarmModal = false;
        editAlarmIndex = -1;
        labelFocused = false;
        stateDirty = true;
    }
}

static void TriggerMatchingAlarms() {
    time_t now = time(nullptr);
    int key = DateKey(now) * 1440 + (localtime(&now)->tm_hour * 60 + localtime(&now)->tm_min);
    int weekday = DayOfWeekIndex(now);
    for (size_t i = 0; i < alarms.size(); ++i) {
        Alarm& a = alarms[i];
        if (!a.enabled) continue;
        if (a.snoozeUntil > 0.0 && GetTime() < a.snoozeUntil) continue;
        if (a.snoozeUntil > 0.0 && GetTime() >= a.snoozeUntil) a.snoozeUntil = 0.0;
        tm lt = *localtime(&now);
        bool timeMatch = lt.tm_hour == a.hour && lt.tm_min == a.minute && lt.tm_sec == 0;
        bool repeatMatch = (a.repeatMask == 0) || ((a.repeatMask & (1 << weekday)) != 0);
        if (timeMatch && repeatMatch && a.lastFireKey != key) {
            a.lastFireKey = key;
            a.ringing = true;
            a.nextBeepAt = 0.0;
            activeRingIndex = (int)i;
        }
    }
}

static void DrawAlarmRingingBanner(Rectangle area) {
    if (activeRingIndex < 0 || activeRingIndex >= (int)alarms.size()) return;
    Alarm& a = alarms[activeRingIndex];
    Rectangle banner = {area.x + 20, area.y + 16, area.width - 40, 80};
    DrawRectangleRounded(banner, 0.14f, 8, Color{50, 18, 18, 255});
    DrawRectangleLinesEx(banner, 1.0f, NEON_PINK);
    DrawText("Alarm ringing", (int)banner.x + 14, (int)banner.y + 10, FONT_SMALL, NEON_PINK);
    char msg[128];
    snprintf(msg, sizeof(msg), "%02d:%02d  %s", a.hour, a.minute, a.label.c_str());
    DrawText(msg, (int)banner.x + 14, (int)banner.y + 34, FONT_LARGE, TEXT_PRIMARY);
    Rectangle snooze = {banner.x + banner.width - 214, banner.y + 24, 92, 30};
    Rectangle stop = {banner.x + banner.width - 112, banner.y + 24, 88, 30};
    if (DrawButton(snooze, "Snooze", BG_HOVER, NEON_GOLD, FONT_SMALL)) {
        a.ringing = false;
        a.snoozeUntil = GetTime() + a.snoozeMinutes * 60.0;
        if (activeRingIndex >= 0 && activeRingIndex < (int)alarms.size() && &alarms[activeRingIndex] == &a) {
            activeRingIndex = -1;
        }
        stateDirty = true;
    }
    if (DrawButton(stop, "Stop", BG_HOVER, NEON_PINK, FONT_SMALL)) {
        a.ringing = false;
        if (a.repeatMask == 0) a.enabled = false;
        if (activeRingIndex >= 0 && activeRingIndex < (int)alarms.size() && &alarms[activeRingIndex] == &a) {
            activeRingIndex = -1;
        }
        stateDirty = true;
    }
}

static void DrawAlarmsTab(Rectangle content) {
    DrawSectionTitle("Alarms", content);

    Rectangle list = {content.x + 30, content.y + 110, content.width - 60, content.height - 150};
    DrawRectangleRounded(list, 0.12f, 8, BG_PANEL);
    DrawRectangleLinesEx(list, 1.0f, BORDER_DIM);
    Rectangle add = {list.x + list.width - 124, list.y + 10, 110, 28};
    if (DrawButton(add, "New Alarm", BG_HOVER, NEON_CYAN, FONT_TINY)) BeginAlarmEditor(-1);

    int rowY = (int)list.y + 50;
    for (size_t i = 0; i < alarms.size(); ++i) {
        Alarm& a = alarms[i];
        Rectangle row = {list.x + 12, (float)rowY, list.width - 24, 64};
        DrawRectangleRounded(row, 0.12f, 8, a.ringing ? Color{50, 20, 20, 255} : BG_HOVER);
        DrawRectangleLinesEx(row, 1.0f, a.ringing ? NEON_PINK : BORDER_DIM);
        std::string timeText = FormatDuration(a.hour * 3600 + a.minute * 60);
        if (timeText.size() == 5) {
            // keep HH:MM format for alarms
            char buf[8];
            snprintf(buf, sizeof(buf), "%02d:%02d", a.hour, a.minute);
            timeText = buf;
        }
        DrawText(timeText.c_str(), (int)row.x + 14, (int)row.y + 10, FONT_TITLE, TEXT_PRIMARY);
        DrawText(a.label.c_str(), (int)row.x + 14, (int)row.y + 42, FONT_SMALL, TEXT_MUTED);
        DrawText(RepeatSummary(a.repeatMask).c_str(), (int)row.x + 200, (int)row.y + 42, FONT_TINY, TEXT_DIM);
        std::string snoozeText = "Snooze " + FormatShortDuration(a.snoozeMinutes);
        DrawText(snoozeText.c_str(), (int)row.x + 320, (int)row.y + 42, FONT_TINY, TEXT_DIM);

        Rectangle edit = {row.x + row.width - 150, row.y + 16, 40, 26};
        Rectangle del = {row.x + row.width - 102, row.y + 16, 40, 26};
        Rectangle toggle = {row.x + row.width - 52, row.y + 18, 36, 20};
        if (DrawButton(edit, "Edit", BG_HOVER, NEON_CYAN, FONT_TINY)) BeginAlarmEditor((int)i);
        if (DrawButton(del, "Del", BG_HOVER, NEON_PINK, FONT_TINY) && i < alarms.size()) {
            alarms.erase(alarms.begin() + i);
            stateDirty = true;
            break;
        }
        if (DrawToggle(toggle, a.enabled)) {
            a.enabled = !a.enabled;
            stateDirty = true;
        }
        rowY += 76;
    }
}

// ============================================================
//  Draw Notification Modal — for active alarms and timers
// ============================================================
static void DrawNotificationModal(int sw, int sh) {
    if (!showNotificationModal) return;
    
    int idx = activeNotificationIndex;
    if (idx == -1) {
        showNotificationModal = false;
        return;
    }
    
    // Dark overlay
    DrawRectangle(0, 0, sw, sh, Color{0, 0, 0, 220});
    
    // Notification box
    Rectangle notif = {(float)(sw / 2 - 280), (float)(sh / 2 - 140), 560, 280};
    DrawRectangleRounded(notif, 0.12f, 8, Color{50, 20, 20, 255});
    DrawRectangleLinesEx(notif, 2.0f, NEON_PINK);
    
    if (idx == -2) {
        // ========== TIMER NOTIFICATION ==========
        DrawText("TIMER FINISHED!", (int)notif.x + 20, (int)notif.y + 20, FONT_LARGE, NEON_PINK);
        std::string text = "Your timer has ended.";
        int tw = MeasureText(text.c_str(), FONT_NORMAL);
        DrawText(text.c_str(), (int)(notif.x + (notif.width - tw) / 2), (int)notif.y + 80, FONT_NORMAL, TEXT_PRIMARY);
    } else if (idx >= 0 && idx < (int)alarms.size()) {
        // ========== ALARM NOTIFICATION ==========
        Alarm& a = alarms[idx];
        DrawText("ALARM!", (int)notif.x + 20, (int)notif.y + 20, FONT_LARGE, NEON_PINK);
        
        char alarmTime[32];
        snprintf(alarmTime, sizeof(alarmTime), "%02d:%02d", a.hour, a.minute);
        DrawText(alarmTime, (int)notif.x + 20, (int)notif.y + 70, FONT_TITLE, NEON_GOLD);
        
        DrawText(a.label.c_str(), (int)notif.x + 20, (int)notif.y + 120, FONT_NORMAL, TEXT_PRIMARY);
    } else {
        showNotificationModal = false;
        activeNotificationIndex = -1;
        return;
    }
    
    // ========== BUTTONS ==========
    Rectangle snoozeBtn = {notif.x + 60, notif.y + notif.height - 60, 130, 44};
    Rectangle stopBtn = {notif.x + notif.width - 190, notif.y + notif.height - 60, 130, 44};
    
    if (DrawButton(snoozeBtn, "Snooze", BG_HOVER, NEON_GOLD, FONT_NORMAL)) {
        // Snooze action
        if (idx == -2) {
            // Timer - just dismiss
            showNotificationModal = false;
            activeNotificationIndex = -1;
            needsAudio = false;
        } else if (idx >= 0 && idx < (int)alarms.size()) {
            alarms[idx].ringing = false;
            alarms[idx].snoozeUntil = GetTime() + alarms[idx].snoozeMinutes * 60.0;
            if (activeRingIndex == idx) activeRingIndex = -1;
            showNotificationModal = false;
            activeNotificationIndex = -1;
            needsAudio = false;
        }
        stateDirty = true;
    }
    
    if (DrawButton(stopBtn, "Stop", BG_HOVER, NEON_PINK, FONT_NORMAL)) {
        // Stop action
        if (idx == -2) {
            // Timer - dismiss
            timerState.finished = false;
            showNotificationModal = false;
            activeNotificationIndex = -1;
            needsAudio = false;
        } else if (idx >= 0 && idx < (int)alarms.size()) {
            alarms[idx].ringing = false;
            if (alarms[idx].repeatMask == 0) {
                alarms[idx].enabled = false;
            }
            if (activeRingIndex == idx) activeRingIndex = -1;
            showNotificationModal = false;
            activeNotificationIndex = -1;
            needsAudio = false;
        }
        stateDirty = true;
    }
}

// ============================================================
//  Background Thread Function — Continuous alarm/timer updates
// ============================================================
static void BackgroundUpdateThread() {
    // Initialize audio in background thread (raylib audio is thread-safe)
    if (!IsAudioDeviceReady()) {
        InitAudioDevice();
    }
    
    double lastSaveTime = GetTime();
    double lastBeepTime = 0.0;
    
    while (bgThreadRunning) {
        {
            std::lock_guard<std::mutex> lock(bgMutex);
            
            // Update timer state
            if (timerState.running) {
                timerState.remainingSeconds = std::max(0.0, timerState.durationSeconds - (GetTime() - timerState.startAt));
                if (timerState.remainingSeconds <= 0.0) {
                    timerState.running = false;
                    timerState.finished = true;
                    timerState.remainingSeconds = 0.0;
                    
                    // Signal notification for timer
                    if (!showNotificationModal) {
                        activeNotificationIndex = -2;  // -2 = timer
                        showNotificationModal = true;
                        needsAudio = true;
                    }
                    stateDirty = true;
                }
            }
            
            // DON'T update stopwatch here - only the UI thread should do calculations
            
            // Trigger alarms
            TriggerMatchingAlarms();
            
            // Handle alarm ringing with notifications
            bool anyRing = false;
            int ringIdx = -1;
            for (size_t i = 0; i < alarms.size(); ++i) {
                if (alarms[i].ringing) {
                    anyRing = true;
                    if (ringIdx < 0) ringIdx = (int)i;
                }
            }
            
            // Signal notification for active alarm
            if (anyRing && !showNotificationModal) {
                activeNotificationIndex = ringIdx;
                showNotificationModal = true;
                needsAudio = true;
            }
            if (!anyRing && activeRingIndex != -1) {
                activeRingIndex = -1;
            }
            
            // Play beeping sound continuously while notification should be active
            double now = GetTime();
            if (needsAudio && now >= lastBeepTime) {
                PlayAlarmTone();
                lastBeepTime = now + 0.8;  // Beep every 0.8 seconds
            }
            
            // Persist state periodically
            if (GetTime() - lastSaveTime > 8.0 && stateDirty) {
                SaveState();
                lastSaveTime = GetTime();
            }
        }
        
        // Sleep briefly to avoid busy-waiting
        usleep(100000); // 100ms
    }
}

int main() {
    if (!RequestResources(APP_NAME, RAM_MB, HDD_MB, PRIORITY_HIGH, 0)) {
        InitWindow(440, 120, "Clock — Denied");
        SetTargetFPS(30);
        double start = GetTime();
        while (!WindowShouldClose() && GetTime() - start < 3.0) {
            BeginDrawing();
            ClearBackground(BG_DEEP);
            DrawText("Insufficient resources.", 20, 40, FONT_NORMAL, NEON_PINK);
            EndDrawing();
        }
        CloseWindow();
        return 1;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE | FLAG_WINDOW_ALWAYS_RUN);
    InitWindow(WIN_W, WIN_H, "NexOS Clock");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    SetWindowFocused();
    InitAlarmTone();
    LoadState();
    EnsureDefaults();

    // Start background thread for continuous alarm/timer updates
    bgThreadRunning = true;
    std::thread bgThread(BackgroundUpdateThread);
    
    while (!WindowShouldClose()) {
        int sw = GetScreenWidth();
        int sh = GetScreenHeight();
        
        // ========== WINDOW RESTORATION LOGIC ==========
        bool isMinimized = IsWindowMinimized();
        
        // If notification is showing, restore window and force focus
        if (showNotificationModal && isMinimized) {
            RestoreWindow();
            ClearWindowState(FLAG_WINDOW_MINIMIZED);
            SetWindowFocused();
            usleep(50000);  // Small delay to let system process
        }
        
        Rectangle content = {20, 54, (float)sw - 40, (float)sh - 74};

        BeginDrawing();
        ClearBackground(BG_DEEP);
        DrawCyberpunkGrid(sw, sh);
        DrawTopTabs(sw);

        DrawRectangleRounded(content, 0.04f, 8, BG_PANEL);
        DrawRectangleLinesEx(content, 1.0f, BORDER_DIM);
        DrawAlarmRingingBanner(content);

        if (currentTab == 0) DrawWorldTab(content, sw);
        else if (currentTab == 1) DrawAlarmsTab(content);
        else if (currentTab == 2) DrawStopwatchTab(content);
        else DrawTimerTab(content);

        if (addCityModal) DrawAddCityModal(sw, sh);
        if (addAlarmModal) DrawAlarmEditorModal(sw, sh);
        
        // ========== NOTIFICATION MODAL (HIGHEST PRIORITY) ==========
        if (showNotificationModal) {
            DrawNotificationModal(sw, sh);
        }

        EndDrawing();
    }

    // Stop background thread
    bgThreadRunning = false;
    bgThread.join();

    SaveState();
    if (toneLoaded) UnloadSound(alarmTone);
    ReleaseResources(APP_NAME, RAM_MB, HDD_MB);
    CloseWindow();
    return 0;
}
