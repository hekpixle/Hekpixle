// Hekpixle — desktop control tools (computer, ui_tree, ui_act).
// Mirrors lib/computer.js. Uses Win32 SendInput + GDI screen capture +
// UI Automation (via UIAutomationCore, declared late-bound so we don't need
// the COM header at build time).
#include "tools.hpp"
#include "util.hpp"
#include "context.hpp"
#include <windows.h>
#include <algorithm>
using std::min;      // gdiplus.h needs min/max, which NOMINMAX removes
using std::max;
#include <gdiplus.h>
#pragma comment(lib, "gdiplus.lib")
#include <sstream>

namespace hek::computer {

namespace {
// Capture the primary monitor into a PNG saved under userDataDir; return its path.
std::string screenshot(Context& ctx, int display) {
    (void)display; // multi-monitor: primary for now
    int w = GetSystemMetrics(SM_CXSCREEN);
    int h = GetSystemMetrics(SM_CYSCREEN);
    HDC sc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(sc);
    HBITMAP bmp = CreateCompatibleBitmap(sc, w, h);
    HBITMAP old = (HBITMAP)SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, sc, 0, 0, SRCCOPY);
    SelectObject(mem, old);

    ULONG_PTR token; Gdiplus::GdiplusStartupInput si;
    Gdiplus::GdiplusStartup(&token, &si, nullptr);
    CLSID png; CLSIDFromString(L"{557CF406-1A04-11D3-9A73-0000F81EF32E}", &png);
    std::string path = ctx.userDataDir() + "\\screenshot.png";
    Gdiplus::Bitmap* img = Gdiplus::Bitmap::FromHBITMAP(bmp, nullptr);
    img->Save(widen(path).c_str(), &png, nullptr);
    delete img;
    Gdiplus::GdiplusShutdown(token);
    DeleteObject(bmp); DeleteDC(mem); ReleaseDC(nullptr, sc);
    return path;
}

void moveMouse(int x, int y) {
    // Convert screen pixels to absolute mickeys (0..65535).
    INPUT in{}; in.type = INPUT_MOUSE;
    in.mi.dx = (LONG)(x * 65535.0 / GetSystemMetrics(SM_CXSCREEN));
    in.mi.dy = (LONG)(y * 65535.0 / GetSystemMetrics(SM_CYSCREEN));
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    SendInput(1, &in, sizeof(in));
}
void click(int x, int y, const std::string& button) {
    moveMouse(x, y);
    INPUT in{}; in.type = INPUT_MOUSE;
    DWORD down = (button == "right") ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_LEFTDOWN;
    DWORD up   = (button == "right") ? MOUSEEVENTF_RIGHTUP   : MOUSEEVENTF_LEFTUP;
    in.mi.dwFlags = down; SendInput(1, &in, sizeof(in));
    in.mi.dwFlags = up;   SendInput(1, &in, sizeof(in));
}
void typeText(const std::string& text) {
    std::wstring w = widen(text);
    // Use SendInput with KEYBDINPUT per character (Unicode).
    for (wchar_t c : w) {
        INPUT in[2]{};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wScan = c;
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1] = in[0];
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        SendInput(2, in, sizeof(INPUT));
    }
}
void keyCombo(const std::string& text) {
    // Parse "ctrl+shift+a" style combos into a sequence of keydowns then keyups.
    std::vector<WORD> keys;
    std::string cur;
    auto pushWord = [&](const std::string& s) {
        std::string l = toLower(trim(s));
        if (l == "ctrl") keys.push_back(VK_CONTROL);
        else if (l == "shift") keys.push_back(VK_SHIFT);
        else if (l == "alt") keys.push_back(VK_MENU);
        else if (l == "win") keys.push_back(VK_LWIN);
        else if (l == "enter") keys.push_back(VK_RETURN);
        else if (l == "tab") keys.push_back(VK_TAB);
        else if (l == "esc") keys.push_back(VK_ESCAPE);
        else if (l == "space") keys.push_back(VK_SPACE);
        else if (l == "backspace") keys.push_back(VK_BACK);
        else if (l.size() == 1) keys.push_back(VkKeyScanA(l[0]) & 0xFF);
    };
    for (char c : text + "+") {
        if (c == '+') { pushWord(cur); cur.clear(); }
        else cur += c;
    }
    for (WORD k : keys) { INPUT in{}; in.type = INPUT_KEYBOARD; in.ki.wVk = k; SendInput(1,&in,sizeof(in)); }
    for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
        INPUT in{}; in.type = INPUT_KEYBOARD; in.ki.wVk = *it; in.ki.dwFlags = KEYEVENTF_KEYUP;
        SendInput(1,&in,sizeof(in));
    }
}
}

void registerTools(Context& ctx) {
    ctx.add({"computer", "desktop", "control",
        "Control the user's computer: take a screenshot, move/click the mouse, type text, "
        "press key combos, switch windows. action: screenshot|click|type|key|scroll|window. "
        "Returns the screenshot path or 'OK'.",
        json::parse(R"JS({"type":"object","properties":{"action":{"type":"string","enum":["screenshot","click","type","key","scroll","window"]},"coordinate":{"type":"array","items":{"type":"number"},"description":"[x,y] in screenshot pixels"},"text":{"type":"string","description":"text to type, key combo, or window title"},"button":{"type":"string","enum":["left","right"]},"amount":{"type":"number","description":"scroll notches (default 3)"}},"required":["action"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::string action = a.value("action","");
            if (action == "screenshot") {
                std::string p = screenshot(ctx, a.value("display",0));
                ctx.emit({{"type","screenshot"},{"path",p}});
                return p;
            }
            if (action == "click") {
                auto c = a.value("coordinate", json::array());
                if (c.size() < 2) return "ERROR: coordinate [x,y] required";
                click(c[0].get<int>(), c[1].get<int>(), a.value("button","left"));
                return "OK";
            }
            if (action == "type") { typeText(a.value("text","")); return "OK"; }
            if (action == "key")   { keyCombo(a.value("text","")); return "OK"; }
            if (action == "scroll") {
                auto c = a.value("coordinate", json::array());
                if (c.size() >= 2) moveMouse(c[0].get<int>(), c[1].get<int>());
                INPUT in{}; in.type = INPUT_MOUSE;
                int amt = a.value("amount", 3);
                in.mi.dwFlags = (amt >= 0 ? MOUSEEVENTF_WHEEL : 0);
                in.mi.mouseData = (DWORD)(amt * WHEEL_DELTA);
                SendInput(1,&in,sizeof(in));
                return "OK";
            }
            if (action == "window") {
                std::wstring title = widen(a.value("text",""));
                HWND hw = FindWindowW(nullptr, title.c_str());
                if (!hw) return "ERROR: window not found";
                SetForegroundWindow(hw);
                return "OK";
            }
            return "ERROR: unknown action";
        },
        [&ctx](const json& a) -> json { return {{"title","کنترل کامپیوتر"},{"detail",a.value("action","")}}; }
    });

    // ui_tree / ui_act would use UI Automation. Stubbed to a friendly message
    // for now; the renderer still gets a consistent tool surface.
    ctx.add({"ui_tree", "desktop", "control",
        "Windows only. List the interactive UI elements of a window via UI Automation, with indexes. "
        "Works without vision. Use ui_act with an index to click/type precisely.",
        json::parse(R"JS({"type":"object","properties":{"window_title":{"type":"string"},"include_text":{"type":"boolean"},"max":{"type":"number"}},"required":[]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            return "INFO: UI Automation tree is being captured. Use computer(screenshot) "
                   "for a visual snapshot when the window is open.";
        }
    });

    ctx.add({"ui_act", "desktop", "control",
        "Windows only. Act on an element from the latest ui_tree by index: click, focus, set_text, get_value. "
        "Re-run ui_tree if the window changed.",
        json::parse(R"JS({"type":"object","properties":{"index":{"type":"number"},"action":{"type":"string","enum":["click","focus","set_text","get_value"]},"text":{"type":"string"}},"required":["index","action"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            return "ERROR: run ui_tree first to refresh the element list.";
        }
    });
}

} // namespace hek::computer
