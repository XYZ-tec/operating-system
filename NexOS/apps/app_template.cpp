// ============================================================
//  NexOS App Template — copy this to start any new app
//
//  HOW TO USE:
//  1. Copy this file: cp apps/app_template.cpp apps/notepad.cpp
//  2. Change APP_NAME, RAM_MB, HDD_MB below
//  3. Replace DrawAppContent() with your actual app logic
//  4. Compile: make apps/notepad
// ============================================================

#include "raylib.h"
#include "../include/./theme.h"
#include "../include/resources.h"
#include "../include/ipc.h"
#include <unistd.h>
#include <cstdio>
#include <cstring>

// ============================================================
//  CHANGE THESE for each app
// ============================================================
#define APP_NAME    "MyApp"
#define RAM_MB      50
#define HDD_MB      10
#define WIN_W       700
#define WIN_H       500

// ============================================================
//  Draw your app content here.
//  'content' is the area inside the window (below title bar).
//  This function is called every frame.
// ============================================================
static void DrawAppContent(Rectangle content)
{
    // Example: just show a placeholder
    DrawText("App content goes here.", 
             (int)content.x + 20, 
             (int)content.y + 20, 
             FONT_NORMAL, TEXT_PRIMARY);
}

// ============================================================
//  Window chrome: title bar + close/minimize buttons
//  Returns: 0=normal  1=minimize  2=close
// ============================================================
static int DrawWindowChrome(Rectangle& win, const char* title,
                             bool& dragging, Vector2& dragOffset)
{
    Rectangle titleBar = { win.x, win.y, win.width, TITLEBAR_H };

    // Title bar background
    DrawRectangleRec(titleBar, BG_TITLEBAR);
    DrawGlowRect(win, NEON_CYAN, 3);

    // Title text
    DrawText(title, (int)win.x + 12, (int)win.y + 8,
             FONT_SMALL, NEON_CYAN);

    // Buttons: minimize [-], maximize [O], close [X]
    float bx = win.x + win.width - 90;
    float by = win.y + 5;
    float bw = 22, bh = 20;

    int result = 0;
    Rectangle minBtn  = { bx,       by, bw, bh };
    Rectangle maxBtn  = { bx + 28,  by, bw, bh };
    Rectangle closeBtn= { bx + 56,  by, bw, bh };

    if (DrawButton(minBtn,   "-", BG_DEEP, NEON_ORANGE, FONT_NORMAL)) result = 1;
    if (DrawButton(maxBtn,   "O", BG_DEEP, NEON_CYAN,   FONT_TINY))   result = 0;
    if (DrawButton(closeBtn, "X", BG_DEEP, NEON_PINK,   FONT_SMALL))  result = 2;

    // Drag logic
    Vector2 mouse = GetMousePosition();
    if (CheckCollisionPointRec(mouse, titleBar)
        && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
        dragging    = true;
        dragOffset  = { mouse.x - win.x, mouse.y - win.y };
    }
    if (IsMouseButtonReleased(MOUSE_LEFT_BUTTON)) dragging = false;
    if (dragging) {
        win.x = mouse.x - dragOffset.x;
        win.y = mouse.y - dragOffset.y;
    }

    return result;
}

// ============================================================
//  MAIN
// ============================================================
int main()
{
    // 1. Request resources from OS
    if (!RequestResources(APP_NAME, RAM_MB, HDD_MB,
                          PRIORITY_NORMAL, 1)) {
        // OS denied — show brief error and exit
        InitWindow(400, 120, APP_NAME " - Error");
        SetTargetFPS(30);
        double start = GetTime();
        while (!WindowShouldClose() && GetTime() - start < 3.0) {
            BeginDrawing();
            ClearBackground(BG_DEEP);
            DrawText("Insufficient resources. Cannot open.",
                     20, 40, FONT_NORMAL, NEON_PINK);
            EndDrawing();
        }
        CloseWindow();
        return 1;
    }

    // 2. Open raylib window
    InitWindow(WIN_W, WIN_H, APP_NAME);
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);

    Rectangle win       = { 40, 40, WIN_W - 80, WIN_H - 80 };
    bool      dragging  = false;
    Vector2   dragOff   = { 0, 0 };
    bool      appRunning= true;

    // 3. Main loop
    while (!WindowShouldClose() && appRunning) {
        BeginDrawing();
        ClearBackground(BG_DEEP);
        DrawCyberpunkGrid(WIN_W, WIN_H);

        // Draw window chrome + detect close/minimize
        int action = DrawWindowChrome(win, APP_NAME, dragging, dragOff);
        if (action == 2) appRunning = false; // close

        // Draw app content inside window (below title bar)
        Rectangle content = {
            win.x, win.y + TITLEBAR_H,
            win.width, win.height - TITLEBAR_H
        };
        DrawRectangleRec(content, BG_PANEL);
        DrawAppContent(content);

        EndDrawing();
    }

    // 4. Release resources back to OS and exit
    ReleaseResources(APP_NAME, RAM_MB, HDD_MB);
    CloseWindow();
    return 0;
}
