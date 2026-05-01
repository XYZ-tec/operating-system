#include "raylib.h"
#include "../include/theme.h"
#include "../include/resources.h"
#include "../include/ipc.h"

#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <stack>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <functional>

#define APP_NAME    "Calculator"
#define RAM_MB      30
#define HDD_MB      5
#define WIN_W       600
#define WIN_H       500
#define PI 3.141592653589793

static std::string expression = "";
static std::string result = "0";
static bool appRunning = true;
static bool isAdvanced = false;

// Simple expression evaluator with functions
static double evaluateExpression(const std::string& expr) {
    std::string s = expr;
    size_t pos = 0;

    auto skipSpace = [&]() { while (pos < s.size() && isspace(s[pos])) ++pos; };

    auto parseNumber = [&]() -> double {
        skipSpace();
        size_t start = pos;
        while (pos < s.size() && (isdigit(s[pos]) || s[pos] == '.')) ++pos;
        return std::stod(s.substr(start, pos - start));
    };

    auto parseFunc = [&]() -> std::string {
        skipSpace();
        size_t start = pos;
        while (pos < s.size() && isalpha(s[pos])) ++pos;
        return s.substr(start, pos - start);
    };

    std::function<double()> parseExpr;
    std::function<double()> parseTerm;
    std::function<double()> parsePower;
    std::function<double()> parseFactor;

    parseFactor = [&]() -> double {
        skipSpace();
        if (s[pos] == '(') {
            ++pos;
            double res = parseExpr();
            skipSpace();
            if (pos < s.size() && s[pos] == ')') ++pos;
            return res;
        } else if (isalpha(s[pos])) {
            std::string func = parseFunc();
            skipSpace();
            if (pos < s.size() && s[pos] == '(') {
                ++pos;
                double val = parseExpr();
                skipSpace();
                if (pos < s.size() && s[pos] == ')') ++pos;
                if (func == "log") return log(val);
                if (func == "exp") return exp(val);
                if (func == "sin") return sin(val * PI / 180.0);
                if (func == "cos") return cos(val * PI / 180.0);
                if (func == "tan") return tan(val * PI / 180.0);
                return val;
            }
            return 0;
        } else {
            return parseNumber();
        }
    };

    parsePower = [&]() -> double {
        double res = parseFactor();
        skipSpace();
        if (pos < s.size() && s[pos] == '^') {
            ++pos;
            double exp = parsePower();
            res = pow(res, exp);
        }
        return res;
    };

    parseTerm = [&]() -> double {
        double res = parsePower();
        while (pos < s.size()) {
            skipSpace();
            if (pos < s.size() && (s[pos] == '*' || s[pos] == '/')) {
                char op = s[pos++];
                double val = parsePower();
                if (op == '*') res *= val;
                else if (val != 0) res /= val;
            } else break;
        }
        return res;
    };

    parseExpr = [&]() -> double {
        double res = parseTerm();
        while (pos < s.size()) {
            skipSpace();
            if (pos < s.size() && (s[pos] == '+' || s[pos] == '-')) {
                char op = s[pos++];
                double val = parseTerm();
                if (op == '+') res += val;
                else res -= val;
            } else break;
        }
        return res;
    };

    return parseExpr();
}

static void appendToExpression(const std::string& s) {
    expression += s;
}

static void clearExpression() {
    expression = "";
    result = "0";
}

static void calculate() {
    try {
        double res = evaluateExpression(expression);
        result = std::to_string(res);
        // Remove trailing zeros
        size_t dot = result.find('.');
        if (dot != std::string::npos) {
            result.erase(result.find_last_not_of('0') + 1, std::string::npos);
            if (result.back() == '.') result.pop_back();
        }
    } catch (...) {
        result = "Error";
    }
}

static void DrawButtonGrid(Rectangle area, int rows, int cols, const std::vector<std::string>& labels, std::function<void(int)> onClick) {
    float btnW = area.width / cols;
    float btnH = area.height / rows;
    for (int r = 0; r < rows; ++r) {
        for (int c = 0; c < cols; ++c) {
            int idx = r * cols + c;
            if (idx >= (int)labels.size()) break;
            Rectangle btn = {area.x + c * btnW, area.y + r * btnH, btnW - 4, btnH - 4};
            Color bg = CheckCollisionPointRec(GetMousePosition(), btn) ? BG_HOVER : BG_PANEL;
            DrawRectangleRounded(btn, 0.2f, 6, bg);
            DrawRectangleLinesEx(btn, 1.0f, BORDER_DIM);
            int tw = MeasureText(labels[idx].c_str(), FONT_NORMAL);
            DrawText(labels[idx].c_str(), (int)(btn.x + (btn.width - tw) / 2), (int)(btn.y + (btn.height - FONT_NORMAL) / 2), FONT_NORMAL, TEXT_PRIMARY);
            if (CheckCollisionPointRec(GetMousePosition(), btn) && IsMouseButtonPressed(MOUSE_LEFT_BUTTON)) {
                onClick(idx);
            }
        }
    }
}

int main() {
    if (!RequestResources(APP_NAME, RAM_MB, HDD_MB, PRIORITY_NORMAL, 1)) {
        InitWindow(420, 120, APP_NAME " — Denied");
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
    InitWindow(WIN_W, WIN_H, "NexOS Calculator");
    SetTargetFPS(60);
    SetExitKey(KEY_NULL);
    SetWindowFocused();

    while (!WindowShouldClose() && appRunning) {
        int sw = GetScreenWidth();
        int sh = GetScreenHeight();
        Rectangle content = {20, 40, (float)(sw - 40), (float)(sh - 60)};

        BeginDrawing();
        ClearBackground(BG_DEEP);
        DrawCyberpunkGrid(sw, sh);
        DrawRectangleRounded(content, 0.05f, 8, BG_PANEL);
        DrawRectangleLinesEx(content, 1.0f, BORDER_DIM);

        // Display
        Rectangle display = {content.x + 20, content.y + 20, content.width - 40, 80};
        DrawRectangleRounded(display, 0.1f, 8, BG_DEEP);
        DrawRectangleLinesEx(display, 1.0f, BORDER_DIM);
        DrawText(expression.c_str(), (int)(display.x + 10), (int)(display.y + 10), FONT_NORMAL, TEXT_PRIMARY);
        DrawText(result.c_str(), (int)(display.x + 10), (int)(display.y + 40), FONT_LARGE, NEON_CYAN);

        // Mode toggle
        if (DrawButton({content.x + 20, content.y + 110, 100, 30}, isAdvanced ? "Basic" : "Advanced", BG_HOVER, NEON_CYAN, FONT_SMALL)) {
            isAdvanced = !isAdvanced;
        }

        // Buttons
        Rectangle btnArea = {content.x + 20, content.y + 150, content.width - 40, content.height - 180};
        std::vector<std::string> basicLabels = {
            "7", "8", "9", "/",
            "4", "5", "6", "*",
            "1", "2", "3", "-",
            "0", ".", "=", "+",
            "C", "(", ")", "⌫"
        };
        std::vector<std::string> advancedLabels = {
            "7", "8", "9", "/", "log",
            "4", "5", "6", "*", "sin",
            "1", "2", "3", "-", "cos",
            "0", ".", "=", "+", "tan",
            "C", "(", ")", "⌫", "^"
        };
        auto labels = isAdvanced ? advancedLabels : basicLabels;
        int rows = isAdvanced ? 5 : 5;
        int cols = isAdvanced ? 5 : 4;

        DrawButtonGrid(btnArea, rows, cols, labels, [&](int idx) {
            auto& lbl = labels[idx];
            if (lbl == "C") clearExpression();
            else if (lbl == "=") calculate();
            else if (lbl == "⌫") { if (!expression.empty()) expression.pop_back(); }
            else if (isAdvanced && lbl == "log") appendToExpression("log(");
            else if (isAdvanced && lbl == "sin") appendToExpression("sin(");
            else if (isAdvanced && lbl == "cos") appendToExpression("cos(");
            else if (isAdvanced && lbl == "tan") appendToExpression("tan(");
            else appendToExpression(lbl);
        });

        EndDrawing();
    }

    ReleaseResources(APP_NAME, RAM_MB, HDD_MB);
    CloseWindow();
    return 0;
}

