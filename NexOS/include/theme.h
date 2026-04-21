#pragma once
#include "raylib.h"

// ============================================================
//  NexOS — Cyberpunk Theme
//  All colors, fonts sizes, and UI drawing helpers live here.
//  Include this in every file that draws anything.
// ============================================================

// --- Background colors --------------------------------------
static const Color BG_DEEP      = { 10,  10,  20, 255 }; // main desktop
static const Color BG_PANEL     = { 18,  18,  36, 255 }; // app windows
static const Color BG_TITLEBAR  = { 14,  14,  30, 255 }; // window title bars
static const Color BG_TASKBAR   = { 12,  12,  26, 255 }; // bottom taskbar
static const Color BG_ICON      = { 22,  22,  44, 255 }; // desktop icon bg
static const Color BG_HOVER     = { 30,  30,  60, 255 }; // hovered icon/btn

// --- Neon accent colors -------------------------------------
static const Color NEON_CYAN    = {  0, 255, 200, 255 }; // primary accent
static const Color NEON_CYAN_DIM= {  0, 255, 200,  60 }; // glow / transparent
static const Color NEON_PINK    = {255,  45, 120, 255 }; // close btn / danger
static const Color NEON_PINK_DIM= {255,  45, 120,  50 };
static const Color NEON_PURPLE  = {140,  60, 220, 255 }; // kernel mode / select
static const Color NEON_GOLD    = {255, 210,   0, 255 }; // RAM bar / warnings
static const Color NEON_GREEN   = { 57, 255,  20, 255 }; // success / running
static const Color NEON_ORANGE  = {255, 140,   0, 255 }; // minimize btn

// --- Text colors --------------------------------------------
static const Color TEXT_PRIMARY = {220, 220, 255, 255 }; // main readable text
static const Color TEXT_MUTED   = {110, 110, 160, 255 }; // secondary / labels
static const Color TEXT_DIM     = { 60,  60, 100, 255 }; // very faint text
static const Color TEXT_CYAN    = {  0, 230, 180, 255 }; // cyan labels

// --- Border / line colors -----------------------------------
static const Color BORDER_CYAN  = {  0, 255, 200,  90 }; // window border glow
static const Color BORDER_DIM   = { 40,  40,  80, 255 }; // subtle separator
static const Color GRID_LINE    = {  0, 255, 200,  12 }; // bg grid (very faint)

// --- Font sizes ---------------------------------------------
#define FONT_TITLE   28
#define FONT_LARGE   20
#define FONT_NORMAL  16
#define FONT_SMALL   13
#define FONT_TINY    11

// --- Window chrome sizes ------------------------------------
#define TITLEBAR_H   30
#define TASKBAR_H    42
#define ICON_SIZE    72
#define ICON_GAP     24
#define BORDER_W      1

// ============================================================
//  Helper: draw a neon-glowing rectangle border
//  Draws a large transparent rect first (glow), then the line.
// ============================================================
static inline void DrawGlowRect(Rectangle r, Color c, int glow)
{
    // outer glow (transparent)
    Color gc = { c.r, c.g, c.b, 28 };
    DrawRectangle(
        (int)(r.x - glow), (int)(r.y - glow),
        (int)(r.width  + glow * 2),
        (int)(r.height + glow * 2), gc
    );
    // sharp border
    DrawRectangleLinesEx(r, 1.2f, c);
}

// ============================================================
//  Helper: draw a filled rounded button, returns true if clicked
// ============================================================
static inline bool DrawButton(Rectangle r, const char* label,
                               Color bg, Color fg, int fontSize)
{
    bool hovered = CheckCollisionPointRec(GetMousePosition(), r);
    Color fill   = hovered ? BG_HOVER : bg;
    DrawRectangleRec(r, fill);
    DrawRectangleLinesEx(r, 1.0f, hovered ? NEON_CYAN : BORDER_DIM);
    int tw = MeasureText(label, fontSize);
    DrawText(label,
             (int)(r.x + (r.width  - tw) / 2),
             (int)(r.y + (r.height - fontSize) / 2),
             fontSize, fg);
    return hovered && IsMouseButtonPressed(MOUSE_LEFT_BUTTON);
}

// ============================================================
//  Helper: draw a progress bar (value 0.0–1.0)
// ============================================================
static inline void DrawProgressBar(Rectangle r, float value,
                                    Color barColor, Color bgColor)
{
    DrawRectangleRec(r, bgColor);
    Rectangle fill = { r.x, r.y, r.width * value, r.height };
    DrawRectangleRec(fill, barColor);
    DrawRectangleLinesEx(r, 1.0f, BORDER_DIM);
}

// ============================================================
//  Helper: draw cyberpunk grid background
// ============================================================
static inline void DrawCyberpunkGrid(int screenW, int screenH)
{
    int spacing = 40;
    for (int x = 0; x < screenW; x += spacing)
        DrawLine(x, 0, x, screenH, GRID_LINE);
    for (int y = 0; y < screenH; y += spacing)
        DrawLine(0, y, screenW, y, GRID_LINE);
}
