#include "raylib.h"
#include "../include/theme.h"
#include "../include/resources.h"
#include "../include/ipc.h"

#include <array>
#include <vector>
#include <deque>
#include <algorithm>
#include <random>
#include <ctime>
#include <string>
#include <cstdio>

#define APP_NAME "Tetris"
#define RAM_MB   45
#define HDD_MB   5
#define WIN_W    960
#define WIN_H    720

static constexpr int BOARD_W = 10;
static constexpr int BOARD_H = 20;
enum PieceType { I = 0, O, T, S, Z, J, L };

struct Piece {
    int type = I;
    int rot = 0;
    int x = 3;
    int y = 0;
};

static int board[BOARD_H][BOARD_W] = {};
static Piece current;
static Piece ghost;
static int holdType = -1;
static bool holdUsed = false;
static std::deque<int> nextQueue;
static std::vector<int> bag;
static std::mt19937 rng((unsigned int)time(nullptr));

static int score = 0;
static int linesClearedTotal = 0;
static int level = 1;
static bool paused = false;
static bool gameOver = false;
static bool appRunning = true;
static double fallTimer = 0.0;
static double inputRepeatTimer = 0.0;

static const Color PIECE_COLORS[7] = {
    {0, 230, 255, 255},   // I
    {255, 220, 0, 255},   // O
    {190, 90, 255, 255},  // T
    {70, 255, 120, 255},  // S
    {255, 70, 110, 255},  // Z
    {80, 130, 255, 255},  // J
    {255, 150, 60, 255}   // L
};

// [piece][rotation][row][col]
static const int SHAPES[7][4][4][4] = {
    { // I
        {{0,0,0,0},{1,1,1,1},{0,0,0,0},{0,0,0,0}},
        {{0,0,1,0},{0,0,1,0},{0,0,1,0},{0,0,1,0}},
        {{0,0,0,0},{0,0,0,0},{1,1,1,1},{0,0,0,0}},
        {{0,1,0,0},{0,1,0,0},{0,1,0,0},{0,1,0,0}}
    },
    { // O
        {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,1,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}}
    },
    { // T
        {{0,1,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,0,0},{0,1,1,0},{0,1,0,0},{0,0,0,0}},
        {{0,0,0,0},{1,1,1,0},{0,1,0,0},{0,0,0,0}},
        {{0,1,0,0},{1,1,0,0},{0,1,0,0},{0,0,0,0}}
    },
    { // S
        {{0,1,1,0},{1,1,0,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,0,0},{0,1,1,0},{0,0,1,0},{0,0,0,0}},
        {{0,0,0,0},{0,1,1,0},{1,1,0,0},{0,0,0,0}},
        {{1,0,0,0},{1,1,0,0},{0,1,0,0},{0,0,0,0}}
    },
    { // Z
        {{1,1,0,0},{0,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,0,1,0},{0,1,1,0},{0,1,0,0},{0,0,0,0}},
        {{0,0,0,0},{1,1,0,0},{0,1,1,0},{0,0,0,0}},
        {{0,1,0,0},{1,1,0,0},{1,0,0,0},{0,0,0,0}}
    },
    { // J
        {{1,0,0,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,1,0},{0,1,0,0},{0,1,0,0},{0,0,0,0}},
        {{0,0,0,0},{1,1,1,0},{0,0,1,0},{0,0,0,0}},
        {{0,1,0,0},{0,1,0,0},{1,1,0,0},{0,0,0,0}}
    },
    { // L
        {{0,0,1,0},{1,1,1,0},{0,0,0,0},{0,0,0,0}},
        {{0,1,0,0},{0,1,0,0},{0,1,1,0},{0,0,0,0}},
        {{0,0,0,0},{1,1,1,0},{1,0,0,0},{0,0,0,0}},
        {{1,1,0,0},{0,1,0,0},{0,1,0,0},{0,0,0,0}}
    }
};

static bool IsCellOccupied(int x, int y) {
    if (x < 0 || x >= BOARD_W || y >= BOARD_H) return true;
    if (y < 0) return false;
    return board[y][x] != 0;
}

static bool Collides(const Piece& p) {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!SHAPES[p.type][p.rot][r][c]) continue;
            int bx = p.x + c;
            int by = p.y + r;
            if (IsCellOccupied(bx, by)) return true;
        }
    }
    return false;
}

static void RefillBag() {
    bag = {I, O, T, S, Z, J, L};
    std::shuffle(bag.begin(), bag.end(), rng);
}

static int PopNextType() {
    if (bag.empty()) RefillBag();
    int t = bag.back();
    bag.pop_back();
    return t;
}

static void EnsureQueueFilled() {
    while ((int)nextQueue.size() < 5) nextQueue.push_back(PopNextType());
}

static double FallInterval() {
    // Faster drop with higher levels.
    double base = 0.72 - (level - 1) * 0.055;
    if (base < 0.08) base = 0.08;
    return base;
}

static void RecomputeGhost() {
    ghost = current;
    while (!Collides(ghost)) ghost.y++;
    ghost.y--;
}

static bool SpawnPiece(int type) {
    current = {type, 0, 3, -1};
    holdUsed = false;
    if (Collides(current)) return false;
    RecomputeGhost();
    return true;
}

static bool SpawnFromQueue() {
    EnsureQueueFilled();
    int t = nextQueue.front();
    nextQueue.pop_front();
    EnsureQueueFilled();
    return SpawnPiece(t);
}

static void LockPiece() {
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!SHAPES[current.type][current.rot][r][c]) continue;
            int bx = current.x + c;
            int by = current.y + r;
            if (bx >= 0 && bx < BOARD_W && by >= 0 && by < BOARD_H) {
                board[by][bx] = current.type + 1;
            }
        }
    }
}

static int ClearLines() {
    int cleared = 0;
    for (int y = BOARD_H - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < BOARD_W; x++) {
            if (board[y][x] == 0) {
                full = false;
                break;
            }
        }
        if (!full) continue;

        for (int row = y; row > 0; row--) {
            for (int x = 0; x < BOARD_W; x++) board[row][x] = board[row - 1][x];
        }
        for (int x = 0; x < BOARD_W; x++) board[0][x] = 0;
        cleared++;
        y++; // re-check same row after collapse
    }
    return cleared;
}

static void ApplyLineScore(int cleared) {
    if (cleared <= 0) return;
    static const int baseScore[5] = {0, 100, 300, 500, 800};
    score += baseScore[cleared] * level;
    linesClearedTotal += cleared;
    level = (linesClearedTotal / 10) + 1;
}

static void MoveHorizontal(int dx) {
    Piece test = current;
    test.x += dx;
    if (!Collides(test)) {
        current = test;
        RecomputeGhost();
    }
}

static void RotatePiece(int dir) {
    Piece test = current;
    test.rot = (test.rot + dir + 4) % 4;
    if (!Collides(test)) {
        current = test;
        RecomputeGhost();
        return;
    }

    // Basic wall kicks.
    const int kicks[] = {-1, 1, -2, 2};
    for (int dx : kicks) {
        test = current;
        test.rot = (test.rot + dir + 4) % 4;
        test.x += dx;
        if (!Collides(test)) {
            current = test;
            RecomputeGhost();
            return;
        }
    }
}

static bool SoftDropOne() {
    Piece test = current;
    test.y += 1;
    if (!Collides(test)) {
        current = test;
        score += 1;
        RecomputeGhost();
        return true;
    }
    return false;
}

static void HardDrop() {
    int dropped = 0;
    while (SoftDropOne()) dropped++;
    score += dropped; // extra reward on top of soft drop points already applied
    LockPiece();
    int lines = ClearLines();
    ApplyLineScore(lines);
    if (!SpawnFromQueue()) gameOver = true;
}

static void HoldPiece() {
    if (holdUsed || gameOver) return;
    holdUsed = true;
    if (holdType < 0) {
        holdType = current.type;
        if (!SpawnFromQueue()) gameOver = true;
    } else {
        int swapType = holdType;
        holdType = current.type;
        if (!SpawnPiece(swapType)) gameOver = true;
    }
}

static void ResetGame() {
    for (int y = 0; y < BOARD_H; y++) {
        for (int x = 0; x < BOARD_W; x++) board[y][x] = 0;
    }
    holdType = -1;
    holdUsed = false;
    nextQueue.clear();
    bag.clear();
    score = 0;
    linesClearedTotal = 0;
    level = 1;
    paused = false;
    gameOver = false;
    fallTimer = 0.0;
    inputRepeatTimer = 0.0;
    EnsureQueueFilled();
    SpawnFromQueue();
}

static void DrawPieceMini(int type, Rectangle box) {
    DrawRectangleRec(box, BG_PANEL);
    DrawRectangleLinesEx(box, 1.0f, BORDER_DIM);
    if (type < 0) return;

    int minR = 4, maxR = -1, minC = 4, maxC = -1;
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (SHAPES[type][0][r][c]) {
                minR = std::min(minR, r);
                maxR = std::max(maxR, r);
                minC = std::min(minC, c);
                maxC = std::max(maxC, c);
            }
        }
    }

    if (maxR < 0) return;
    int w = maxC - minC + 1;
    int h = maxR - minR + 1;
    float size = std::min((box.width - 16) / w, (box.height - 16) / h);
    float startX = box.x + (box.width - w * size) * 0.5f;
    float startY = box.y + (box.height - h * size) * 0.5f;

    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!SHAPES[type][0][r][c]) continue;
            int dr = r - minR;
            int dc = c - minC;
            Rectangle cell = {startX + dc * size, startY + dr * size, size - 2, size - 2};
            DrawRectangleRec(cell, PIECE_COLORS[type]);
            DrawRectangleLinesEx(cell, 1.0f, Color{10, 16, 30, 220});
        }
    }
}

static void DrawBoard(Rectangle area, int cellSize) {
    DrawRectangleRounded(area, 0.03f, 8, BG_PANEL);
    DrawRectangleLinesEx(area, 1.0f, BORDER_DIM);

    Rectangle boardRect = {
        area.x + (area.width - BOARD_W * cellSize) * 0.5f,
        area.y + (area.height - BOARD_H * cellSize) * 0.5f,
        (float)(BOARD_W * cellSize),
        (float)(BOARD_H * cellSize)
    };
    DrawRectangleRec(boardRect, Color{6, 8, 20, 255});
    DrawRectangleLinesEx(boardRect, 1.0f, BORDER_DIM);

    // Grid lines
    for (int x = 1; x < BOARD_W; x++) {
        DrawLine((int)(boardRect.x + x * cellSize), (int)boardRect.y,
                 (int)(boardRect.x + x * cellSize), (int)(boardRect.y + boardRect.height), Color{30, 38, 70, 120});
    }
    for (int y = 1; y < BOARD_H; y++) {
        DrawLine((int)boardRect.x, (int)(boardRect.y + y * cellSize),
                 (int)(boardRect.x + boardRect.width), (int)(boardRect.y + y * cellSize), Color{30, 38, 70, 120});
    }

    // Locked cells
    for (int y = 0; y < BOARD_H; y++) {
        for (int x = 0; x < BOARD_W; x++) {
            int v = board[y][x];
            if (v == 0) continue;
            Rectangle block = {
                boardRect.x + x * cellSize + 1,
                boardRect.y + y * cellSize + 1,
                (float)cellSize - 2,
                (float)cellSize - 2
            };
            DrawRectangleRec(block, PIECE_COLORS[v - 1]);
            DrawRectangleLinesEx(block, 1.0f, Color{10, 16, 30, 220});
        }
    }

    // Ghost piece
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!SHAPES[ghost.type][ghost.rot][r][c]) continue;
            int bx = ghost.x + c;
            int by = ghost.y + r;
            if (by < 0) continue;
            Rectangle cell = {
                boardRect.x + bx * cellSize + 1,
                boardRect.y + by * cellSize + 1,
                (float)cellSize - 2,
                (float)cellSize - 2
            };
            Color gc = PIECE_COLORS[ghost.type];
            gc.a = 50;
            DrawRectangleRec(cell, gc);
            DrawRectangleLinesEx(cell, 1.0f, Color{160, 210, 255, 130});
        }
    }

    // Current piece
    for (int r = 0; r < 4; r++) {
        for (int c = 0; c < 4; c++) {
            if (!SHAPES[current.type][current.rot][r][c]) continue;
            int bx = current.x + c;
            int by = current.y + r;
            if (by < 0) continue;
            Rectangle cell = {
                boardRect.x + bx * cellSize + 1,
                boardRect.y + by * cellSize + 1,
                (float)cellSize - 2,
                (float)cellSize - 2
            };
            DrawRectangleRec(cell, PIECE_COLORS[current.type]);
            DrawRectangleLinesEx(cell, 1.0f, Color{10, 16, 30, 220});
        }
    }

    if (paused || gameOver) {
        DrawRectangleRec(boardRect, Color{2, 4, 12, 170});
        const char* msg = gameOver ? "GAME OVER" : "PAUSED";
        int tw = MeasureText(msg, FONT_TITLE);
        DrawText(msg, (int)(boardRect.x + (boardRect.width - tw) * 0.5f),
                 (int)(boardRect.y + boardRect.height * 0.45f), FONT_TITLE, gameOver ? NEON_PINK : NEON_CYAN);
        if (gameOver) {
            DrawText("Press R to restart", (int)(boardRect.x + 90), (int)(boardRect.y + boardRect.height * 0.55f),
                     FONT_NORMAL, TEXT_PRIMARY);
        } else {
            DrawText("Press P to resume", (int)(boardRect.x + 94), (int)(boardRect.y + boardRect.height * 0.55f),
                     FONT_NORMAL, TEXT_PRIMARY);
        }
    }
}

static void DrawSidebar(Rectangle panel, const char* title) {
    DrawRectangleRounded(panel, 0.08f, 8, BG_PANEL);
    DrawRectangleLinesEx(panel, 1.0f, BORDER_DIM);
    DrawText(title, (int)panel.x + 12, (int)panel.y + 10, FONT_NORMAL, NEON_CYAN);
}

static void DrawUI(Rectangle content) {
    const float outerPad = 16.0f;
    const float gap = 16.0f;
    const float statsWidth = 260.0f;

    Rectangle right = {
        content.x + content.width - outerPad - statsWidth,
        content.y + outerPad,
        statsWidth,
        content.height - outerPad * 2
    };
    Rectangle boardArea = {
        content.x + outerPad,
        content.y + outerPad,
        right.x - gap - (content.x + outerPad),
        content.height - outerPad * 2
    };

    float maxCellByWidth = (boardArea.width - 24.0f) / BOARD_W;
    float maxCellByHeight = (boardArea.height - 24.0f) / BOARD_H;
    int cellSize = (int)std::floor(std::min(maxCellByWidth, maxCellByHeight));
    if (cellSize < 12) cellSize = 12;

    DrawBoard(boardArea, cellSize);
    DrawSidebar(right, "Stats");

    int tx = (int)right.x + 12;
    float panelBottom = right.y + right.height;
    float y = right.y + 46.0f;

    DrawText(TextFormat("Score: %d", score), tx, (int)y, FONT_NORMAL, TEXT_PRIMARY);
    DrawText(TextFormat("Level: %d", level), tx, (int)(y + 28), FONT_NORMAL, TEXT_PRIMARY);
    DrawText(TextFormat("Lines: %d", linesClearedTotal), tx, (int)(y + 56), FONT_NORMAL, TEXT_PRIMARY);
    if (DrawButton({right.x + right.width - 106, right.y + 10, 94, 28}, "Reset", BG_HOVER, NEON_PINK, FONT_SMALL)) {
        ResetGame();
    }
    y += 98.0f;

    float holdBoxH = std::clamp(right.height * 0.14f, 58.0f, 92.0f);
    DrawText("Hold", tx, (int)y, FONT_NORMAL, NEON_GOLD);
    y += 28.0f;
    DrawPieceMini(holdType, {right.x + 12, y, right.width - 24, holdBoxH});
    y += holdBoxH + 14.0f;

    // Controls block adapts first, then "next" uses the remaining space.
    bool compact = right.height < 620.0f;
    int controlsLines = compact ? 4 : 5;
    int controlsBlockH = compact ? 94 : 118;
    float controlsY = panelBottom - controlsBlockH - 12.0f;

    DrawText("Controls", tx, (int)controlsY, FONT_NORMAL, NEON_CYAN);
    DrawText("Arrows  Move / Soft drop", tx, (int)(controlsY + 22), FONT_SMALL, TEXT_MUTED);
    DrawText("Up/X    Rotate CW", tx, (int)(controlsY + 40), FONT_SMALL, TEXT_MUTED);
    DrawText("Z       Rotate CCW", tx, (int)(controlsY + 58), FONT_SMALL, TEXT_MUTED);
    if (controlsLines >= 5) {
        DrawText("Space   Hard drop", tx, (int)(controlsY + 76), FONT_SMALL, TEXT_MUTED);
        DrawText("C Hold   P Pause", tx, (int)(controlsY + 94), FONT_SMALL, TEXT_MUTED);
    } else {
        DrawText("Space Hard drop  C Hold  P Pause", tx, (int)(controlsY + 76), FONT_SMALL, TEXT_MUTED);
    }

    float nextTop = y;
    float nextBottom = controlsY - 12.0f;
    float nextAreaH = nextBottom - nextTop;
    if (nextAreaH > 70.0f) {
        DrawText("Next", tx, (int)nextTop, FONT_NORMAL, NEON_GREEN);
        nextTop += 28.0f;
    }

    float minBoxH = compact ? 56.0f : 72.0f;
    float stepGap = 8.0f;
    int maxCountByHeight = (int)((nextBottom - nextTop + stepGap) / (minBoxH + stepGap));
    int nextCount = std::clamp(maxCountByHeight, 1, 3);
    float boxH = (nextBottom - nextTop - (nextCount - 1) * stepGap) / nextCount;
    if (boxH < 40.0f) boxH = 40.0f;

    for (int i = 0; i < nextCount; i++) {
        int t = (i < (int)nextQueue.size()) ? nextQueue[i] : -1;
        DrawPieceMini(t, {right.x + 12, nextTop + i * (boxH + stepGap), right.width - 24, boxH});
    }
}

static void HandleInput(double dt) {
    if (IsKeyPressed(KEY_P)) paused = !paused;

    if (gameOver) {
        if (IsKeyPressed(KEY_R)) ResetGame();
        return;
    }

    if (paused) return;

    if (IsKeyPressed(KEY_LEFT) || (IsKeyDown(KEY_LEFT) && inputRepeatTimer <= 0.0)) {
        MoveHorizontal(-1);
        inputRepeatTimer = IsKeyPressed(KEY_LEFT) ? 0.13 : 0.045;
    } else if (IsKeyPressed(KEY_RIGHT) || (IsKeyDown(KEY_RIGHT) && inputRepeatTimer <= 0.0)) {
        MoveHorizontal(1);
        inputRepeatTimer = IsKeyPressed(KEY_RIGHT) ? 0.13 : 0.045;
    }

    if (IsKeyPressed(KEY_UP) || IsKeyPressed(KEY_X)) RotatePiece(1);
    if (IsKeyPressed(KEY_Z)) RotatePiece(-1);
    if (IsKeyPressed(KEY_C)) HoldPiece();
    if (IsKeyPressed(KEY_SPACE)) {
        HardDrop();
        fallTimer = 0.0;
        return;
    }

    bool accelerated = IsKeyDown(KEY_DOWN);
    double interval = accelerated ? std::max(0.02, FallInterval() * 0.08) : FallInterval();
    fallTimer += dt;
    if (fallTimer >= interval) {
        fallTimer = 0.0;
        if (!SoftDropOne()) {
            LockPiece();
            int lines = ClearLines();
            ApplyLineScore(lines);
            if (!SpawnFromQueue()) gameOver = true;
        }
    }

    if (inputRepeatTimer > 0.0) inputRepeatTimer -= dt;
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
    InitWindow(WIN_W, WIN_H, "NexOS Tetris");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    SetWindowFocused();

    ResetGame();

    while (!WindowShouldClose() && appRunning) {
        double dt = GetFrameTime();
        HandleInput(dt);

        int sw = GetScreenWidth();
        int sh = GetScreenHeight();
        Rectangle content = {20, 40, (float)(sw - 40), (float)(sh - 60)};

        BeginDrawing();
        ClearBackground(BG_DEEP);
        DrawCyberpunkGrid(sw, sh);
        DrawRectangleRounded(content, 0.05f, 8, BG_PANEL);
        DrawRectangleLinesEx(content, 1.0f, BORDER_DIM);
        DrawUI(content);
        EndDrawing();
    }

    ReleaseResources(APP_NAME, RAM_MB, HDD_MB);
    CloseWindow();
    return 0;
}
