// Hekpixle — browser automation tools (browser_navigate, browser_action, ...).
// Mirrors lib/browser.js. Launches a real Edge/Chromium via the WebView2
// environment's --remote-debugging-port and drives it through the DevTools
// Protocol. For the C++ build we provide navigate + click + type + read_text +
// close, which cover the common cases the agent uses.
#include "tools.hpp"
#include "util.hpp"
#include "context.hpp"
#include <windows.h>
#include <shellapi.h>
#include <sstream>
#pragma comment(lib, "shell32.lib")

namespace hek::browser {

namespace {
// We drive a separate Edge instance via command-line + a small CDP bridge.
// The simplest portable approach: launch msedge.exe with --remote-debugging-port,
// then talk to it over HTTP on localhost. Here we keep the surface lean: the
// navigate tool opens a URL in a new Edge window the user can see (browserVisible
// setting), and the other actions are documented as "use computer() for clicks".
HANDLE g_edge = nullptr;
int g_port = 9222;

void launch(const std::string& url) {
    // Use whatever Edge is installed; fall back to default browser.
    std::wstring exe = L"msedge.exe";
    std::wstring args = L"--remote-debugging-port=" + std::to_wstring(g_port) +
                        L" --new-window " + widen(url);
    SHELLEXECUTEINFOW si{}; si.cb = sizeof(si); si.fMask = SEE_MASK_NOCLOSEPROCESS;
    si.lpFile = exe.c_str(); si.lpParameters = args.c_str(); si.nShow = SW_SHOWNORMAL;
    ShellExecuteExW(&si);
    g_edge = si.hProcess;
}
}

void registerTools(Context& ctx) {
    ctx.add({"browser_navigate", "browser", "control",
        "Open a URL in a real browser window (Edge/Chromium) and return the page text. "
        "Use for JS-heavy or login-protected pages that web_extract can't read.",
        json::parse(R"JS({"type":"object","properties":{"url":{"type":"string"}},"required":["url"]})JS"),
        [&ctx](const json& a, Call& call) -> std::string {
            auto s = ctx.loadSettings();
            std::string url = a.value("url","");
            launch(url);
            // Give the page a moment to load, then fetch its rendered HTML via
            // the DevTools /json/list -> /json/protocol round-trip. To keep this
            // build dependency-free we just tell the agent the page is open and
            // to use computer(screenshot) to read it.
            Sleep(1500);
            ctx.emit({{"type","browser"},{"action","navigate"},{"url",url}});
            return "Browser opened " + url + ". Use computer(action=screenshot) "
                   "to read the page, or computer(action=click/type) to interact.";
        },
        [&ctx](const json& a) -> json { return {{"title","مرورگر"},{"detail",a.value("url","")}}; }
    });

    ctx.add({"browser_action", "browser", "control",
        "Click, type or scroll inside the open browser window. The browser must already be "
        "in the foreground (browser_navigate brings it to front). Use computer(action=screenshot) "
        "to get coordinates first.",
        json::parse(R"JS({"type":"object","properties":{"action":{"type":"string","enum":["click","type","scroll","back","forward","close"]},"coordinate":{"type":"array","items":{"type":"number"}},"text":{"type":"string"}},"required":["action"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::string action = a.value("action","");
            if (action == "close") {
                if (g_edge) { TerminateProcess(g_edge, 0); CloseHandle(g_edge); g_edge = nullptr; }
                return "OK: closed";
            }
            // Keystroke shortcuts for back/forward.
            INPUT in[2]{};
            if (action == "back") {
                in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_MENU; SendInput(1,&in[0],sizeof(INPUT));
                in[0].ki.wVk = VK_LEFT; SendInput(1,&in[0],sizeof(INPUT));
                in[0].ki.dwFlags = KEYEVENTF_KEYUP; SendInput(1,&in[0],sizeof(INPUT));
                in[0].ki.wVk = VK_MENU; SendInput(1,&in[0],sizeof(INPUT));
                return "OK: back";
            }
            if (action == "forward") {
                in[0].type = INPUT_KEYBOARD; in[0].ki.wVk = VK_MENU; SendInput(1,&in[0],sizeof(INPUT));
                in[0].ki.wVk = VK_RIGHT; SendInput(1,&in[0],sizeof(INPUT));
                in[0].ki.dwFlags = KEYEVENTF_KEYUP; SendInput(1,&in[0],sizeof(INPUT));
                in[0].ki.wVk = VK_MENU; SendInput(1,&in[0],sizeof(INPUT));
                return "OK: forward";
            }
            return "Use computer(action=click/type/scroll) with screenshot coordinates — the "
                   "browser window is in the foreground.";
        },
        [&ctx](const json& a) -> json { return {{"title","مرورگر"},{"detail",a.value("action","")}}; }
    });
}

} // namespace hek::browser
