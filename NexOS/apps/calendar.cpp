#include "raylib.h"
#include "../include/theme.h"
#include "../include/resources.h"
#include "../include/ipc.h"

#include <unistd.h>
#include <sys/stat.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

#define APP_NAME    "Calendar"
#define RAM_MB      20
#define HDD_MB      1
#define WIN_W       1024
#define WIN_H       680
#define STATE_PATH  "hdd/calendar_state.txt"

static const char* MONTH_NAMES[12] = {
    "January", "February", "March", "April", "May", "June",
    "July", "August", "September", "October", "November", "December"
};
static const char* WEEK_NAMES[7] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

struct CalendarEvent {
    int year;
    int month;
    int day;
    std::string note;
};

static std::vector<CalendarEvent> events;
static int currentYear = 0;
static int currentMonth = 0;
static int currentDay = 1;
static std::string noteText;
static bool noteFocused = false;
static bool appRunning = true;
static bool stateDirty = false;

static constexpr float EVENT_PANEL_WIDTH = 300.0f;
static constexpr float EVENT_PANEL_RIGHT_MARGIN = 20.0f;
static constexpr float GRID_LEFT_MARGIN = 16.0f;
static constexpr float GRID_GAP_TO_PANEL = 12.0f;

static std::vector<std::string> Split(const std::string& s, char d) {
    std::vector<std::string> result;
    std::stringstream ss(s);
    std::string item;
    while (std::getline(ss, item, d)) result.push_back(item);
    return result;
}

static bool IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int DaysInMonth(int year, int month) {
    if (month == 2) return IsLeapYear(year) ? 29 : 28;
    if (month == 4 || month == 6 || month == 9 || month == 11) return 30;
    return 31;
}

static int FirstWeekdayOfMonth(int year, int month) {
    int y = year;
    int m = month;
    if (m < 3) {
        m += 12;
        y -= 1;
    }
    int K = y % 100;
    int J = y / 100;
    int h = (1 + (13 * (m + 1)) / 5 + K + K / 4 + J / 4 + 5 * J) % 7;
    // Zeller's congruence returns 0=Saturday, 1=Sunday, ..., 6=Friday
    return (h + 6) % 7;
}

static bool IsToday(int year, int month, int day) {
    time_t now = time(nullptr);
    tm lt = *localtime(&now);
    return lt.tm_year + 1900 == year && lt.tm_mon + 1 == month && lt.tm_mday == day;
}

static CalendarEvent* FindEvent(int year, int month, int day) {
    for (auto& e : events) {
        if (e.year == year && e.month == month && e.day == day) return &e;
    }
    return nullptr;
}

static void SaveState() {
    mkdir("hdd", 0755);
    FILE* f = fopen(STATE_PATH, "w");
    if (!f) return;
    for (auto& e : events) {
        std::string escaped = e.note;
        fprintf(f, "%04d|%02d|%02d|%s\n", e.year, e.month, e.day, escaped.c_str());
    }
    fclose(f);
    stateDirty = false;
}

static void LoadState() {
    FILE* f = fopen(STATE_PATH, "r");
    if (!f) return;
    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (s.empty()) continue;
        auto parts = Split(s, '|');
        if (parts.size() < 4) continue;
        CalendarEvent e;
        e.year = atoi(parts[0].c_str());
        e.month = atoi(parts[1].c_str());
        e.day = atoi(parts[2].c_str());
        e.note = parts[3];
        for (size_t i = 4; i < parts.size(); i++) {
            e.note += "|" + parts[i];
        }
        if (e.year >= 1900 && e.month >= 1 && e.month <= 12 && e.day >= 1 && e.day <= DaysInMonth(e.year, e.month)) {
            events.push_back(e);
        }
    }
    fclose(f);
}

static void EnsureCurrentDate() {
    time_t now = time(nullptr);
    tm lt = *localtime(&now);
    currentYear = lt.tm_year + 1900;
    currentMonth = lt.tm_mon + 1;
    currentDay = lt.tm_mday;
    noteText.clear();
    auto* ev = FindEvent(currentYear, currentMonth, currentDay);
    if (ev) noteText = ev->note;
}

static void SelectDate(int year, int month, int day) {
    currentYear = year;
    currentMonth = month;
    currentDay = day;
    if (currentDay > DaysInMonth(currentYear, currentMonth)) currentDay = DaysInMonth(currentYear, currentMonth);
    noteText.clear();
    auto* ev = FindEvent(currentYear, currentMonth, currentDay);
    if (ev) noteText = ev->note;
    noteFocused = false;
}

static void SaveNoteForSelectedDay() {
    if (currentDay < 1 || currentDay > DaysInMonth(currentYear, currentMonth)) return;
    auto* ev = FindEvent(currentYear, currentMonth, currentDay);
    if (noteText.empty()) {
        if (ev) {
            events.erase(std::remove_if(events.begin(), events.end(), [&](const CalendarEvent& e) {
                return e.year == currentYear && e.month == currentMonth && e.day == currentDay;
            }), events.end());
            stateDirty = true;
        }
    } else {
        if (ev) {
            ev->note = noteText;
        } else {
            events.push_back({currentYear, currentMonth, currentDay, noteText});
        }
        stateDirty = true;
    }
}

static void DrawCalendarHeader(Rectangle c) {
    std::string title = std::string(MONTH_NAMES[currentMonth - 1]) + " " + std::to_string(currentYear);
    int tw = MeasureText(title.c_str(), FONT_TITLE);
    DrawText(title.c_str(), (int)(c.x + (c.width - tw) / 2), (int)(c.y + 18), FONT_TITLE, NEON_CYAN);

    if (DrawButton({c.x + 20, c.y + 16, 46, 30}, "<<", BG_HOVER, TEXT_PRIMARY, FONT_NORMAL)) {
        currentYear -= 1;
        if (currentYear < 1900) currentYear = 1900;
        SelectDate(currentYear, currentMonth, currentDay);
    }
    if (DrawButton({c.x + 74, c.y + 16, 46, 30}, "<", BG_HOVER, TEXT_PRIMARY, FONT_NORMAL)) {
        currentMonth -= 1;
        if (currentMonth < 1) { currentMonth = 12; currentYear -= 1; }
        SelectDate(currentYear, currentMonth, currentDay);
    }
    if (DrawButton({c.x + c.width - 120, c.y + 16, 46, 30}, ">", BG_HOVER, TEXT_PRIMARY, FONT_NORMAL)) {
        currentMonth += 1;
        if (currentMonth > 12) { currentMonth = 1; currentYear += 1; }
        SelectDate(currentYear, currentMonth, currentDay);
    }
    if (DrawButton({c.x + c.width - 68, c.y + 16, 46, 30}, ">>", BG_HOVER, TEXT_PRIMARY, FONT_NORMAL)) {
        currentYear += 1;
        SelectDate(currentYear, currentMonth, currentDay);
    }
}

static void DrawCalendarGrid(Rectangle c) {
    int days = DaysInMonth(currentYear, currentMonth);
    int firstWeekday = FirstWeekdayOfMonth(currentYear, currentMonth);
    float gridX = c.x + GRID_LEFT_MARGIN;
    float gridY = c.y + 84;
    float panelX = c.x + c.width - EVENT_PANEL_WIDTH - EVENT_PANEL_RIGHT_MARGIN;
    float gridRight = panelX - GRID_GAP_TO_PANEL;
    float gridW = gridRight - gridX;
    if (gridW < 280.0f) gridW = 280.0f;
    float cellW = gridW / 7.0f;
    float cellH = 70.0f;

    for (int i = 0; i < 7; i++) {
        Rectangle head = {gridX + i * cellW, gridY, cellW, 28};
        DrawRectangleRec(head, BG_PANEL);
        DrawRectangleLinesEx(head, 1.0f, BORDER_DIM);
        int tw = MeasureText(WEEK_NAMES[i], FONT_SMALL);
        DrawText(WEEK_NAMES[i], (int)(head.x + (head.width - tw) / 2), (int)(head.y + 6), FONT_SMALL, TEXT_MUTED);
    }

    int rows = (firstWeekday + days + 6) / 7;
    for (int r = 0; r < rows; r++) {
        for (int col = 0; col < 7; col++) {
            int index = r * 7 + col;
            int day = index - firstWeekday + 1;
            Rectangle cell = {gridX + col * cellW, gridY + 30 + r * cellH, cellW - 6, cellH - 8};
            DrawRectangleRounded(cell, 0.12f, 8, BG_PANEL);
            DrawRectangleLinesEx(cell, 1.0f, BORDER_DIM);
            if (day >= 1 && day <= days) {
                bool isSelected = (day == currentDay);
                bool isToday = IsToday(currentYear, currentMonth, day);
                if (isSelected) {
                    DrawRectangleRounded(cell, 0.12f, 8, Color{0, 70, 130, 255});
                    DrawRectangleLinesEx(cell, 2.0f, NEON_CYAN);
                } else if (isToday) {
                    DrawRectangleRounded(cell, 0.12f, 8, Color{18, 36, 80, 255});
                    DrawRectangleLinesEx(cell, 1.5f, NEON_GOLD);
                }
                char label[3]; snprintf(label, sizeof(label), "%02d", day);
                DrawText(label, (int)(cell.x + 8), (int)(cell.y + 8), FONT_NORMAL, TEXT_PRIMARY);
                auto* ev = FindEvent(currentYear, currentMonth, day);
                if (ev) {
                    DrawCircle((int)(cell.x + cell.width - 16), (int)(cell.y + cell.height - 16), 6, NEON_CYAN);
                }
                if (CheckCollisionPointRec(GetMousePosition(), cell) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                    SelectDate(currentYear, currentMonth, day);
                }
            }
        }
    }
}

static void DrawEventPanel(Rectangle c) {
    Rectangle panel = {
        c.x + c.width - EVENT_PANEL_WIDTH - EVENT_PANEL_RIGHT_MARGIN,
        c.y + 84,
        EVENT_PANEL_WIDTH,
        c.height - 108
    };
    DrawRectangleRounded(panel, 0.08f, 8, BG_PANEL);
    DrawRectangleLinesEx(panel, 1.0f, BORDER_DIM);

    char dateLabel[64];
    snprintf(dateLabel, sizeof(dateLabel), "%s %d, %d", MONTH_NAMES[currentMonth - 1], currentDay, currentYear);
    DrawText(dateLabel, (int)(panel.x + 16), (int)(panel.y + 16), FONT_LARGE, NEON_CYAN);

    bool hasEvent = FindEvent(currentYear, currentMonth, currentDay) != nullptr;
    DrawText(hasEvent ? "Note saved" : "No note yet", (int)(panel.x + 16), (int)(panel.y + 52), FONT_SMALL, hasEvent ? NEON_GREEN : TEXT_MUTED);

    Rectangle noteBox = {panel.x + 16, panel.y + 86, panel.width - 32, panel.height - 160};
    DrawRectangleRec(noteBox, BG_DEEP);
    DrawRectangleLinesEx(noteBox, 1.0f, noteFocused ? NEON_CYAN : BORDER_DIM);
    DrawText(noteText.c_str(), (int)(noteBox.x + 8), (int)(noteBox.y + 8), FONT_SMALL, TEXT_PRIMARY);
    if (noteFocused && (int)(GetTime() * 2) % 2 == 0) {
        int cursorX = (int)(noteBox.x + 8 + MeasureText(noteText.c_str(), FONT_SMALL));
        DrawText("|", cursorX, (int)(noteBox.y + 8), FONT_SMALL, NEON_CYAN);
    }
    if (IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        if (CheckCollisionPointRec(GetMousePosition(), noteBox)) noteFocused = true;
        else noteFocused = false;
    }

    static auto TextInputField = [&](std::string& s, int maxLen, bool focused) {
        if (!focused) return;
        int key = GetCharPressed();
        while (key > 0) {
            if (key >= 32 && key < 127 && (int)s.size() < maxLen) s.push_back((char)key);
            key = GetCharPressed();
        }
        if (IsKeyPressed(KEY_BACKSPACE) && !s.empty()) s.pop_back();
    };
    TextInputField(noteText, 256, noteFocused);

    if (DrawButton({panel.x + 16, panel.y + panel.height - 56, 108, 30}, "Save Note", BG_HOVER, NEON_CYAN, FONT_SMALL)) {
        SaveNoteForSelectedDay();
    }
    if (hasEvent && DrawButton({panel.x + 136, panel.y + panel.height - 56, 108, 30}, "Delete Note", BG_HOVER, NEON_PINK, FONT_SMALL)) {
        noteText.clear();
        SaveNoteForSelectedDay();
    }
}

int main() {
    if (!RequestResources(APP_NAME, RAM_MB, HDD_MB, PRIORITY_NORMAL, 1)) {
        InitWindow(420, 120, APP_NAME " - Denied");
        SetTargetFPS(30);
        double start = GetTime();
        while (!WindowShouldClose() && GetTime() - start < 3.5) {
            BeginDrawing();
            ClearBackground(BG_DEEP);
            DrawText("Insufficient resources.", 20, 40, FONT_NORMAL, NEON_PINK);
            EndDrawing();
        }
        CloseWindow();
        return 1;
    }

    SetConfigFlags(FLAG_WINDOW_RESIZABLE);
    InitWindow(WIN_W, WIN_H, "NexOS Calendar");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    SetWindowFocused();

    LoadState();
    EnsureCurrentDate();

    while (!WindowShouldClose() && appRunning) {
        int sw = GetScreenWidth();
        int sh = GetScreenHeight();
        Rectangle content = {20, 40, (float)(sw - 40), (float)(sh - 60)};

        BeginDrawing();
        ClearBackground(BG_DEEP);
        DrawCyberpunkGrid(sw, sh);
        DrawRectangleRounded(content, 0.05f, 8, BG_PANEL);
        DrawRectangleLinesEx(content, 1.0f, BORDER_DIM);
        DrawCalendarHeader(content);
        DrawCalendarGrid(content);
        DrawEventPanel(content);

        EndDrawing();
    }

    if (stateDirty) SaveState();
    ReleaseResources(APP_NAME, RAM_MB, HDD_MB);
    CloseWindow();
    return 0;
}
