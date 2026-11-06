// Hekpixle — C++ desktop agent entry point.
//
// Architecture (mirrors the original Electron app, graphics/design unchanged):
//
//   main.cpp        app entry: parse --profile=, build Context, run window loop
//   window.cpp      Win32 window + WebView2 host; loads renderer/index.html;
//                   injects the IPC shim; bridges WebMessage <-> ipcDispatch
//   ipc.cpp         dispatches window.hek.* calls to the backend (exact preload.js)
//   ipc_shim.cpp    the JS injected before each page load (recreates window.hek)
//   agent.cpp       the LLM tool-call loop (runAgent / compressChat)
//   context.cpp     tool registry + settings + userData + permission prompts
//   util.cpp        WinHTTP fetch, LLM chat, paths, text helpers
//   lib/*.cpp       one file per tool module (web, files, computer, ...)
//
// The renderer/ folder is byte-for-byte the original Electron renderer, so the
// UI, fonts, KaTeX, highlight.js, 3D hero and styles are identical.

#include "context.hpp"
#include "window.hpp"
#include "util.hpp"
#include <windows.h>
#include <shellapi.h>          // CommandLineToArgvW
#include <shellscalingapi.h>   // SetProcessDpiAwarenessContext
#include <clocale>
#include <string>
#include <vector>

static std::string parseProfile(int argc, char* argv[]) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if (a.rfind("--profile=", 0) == 0) {
            std::string v = a.substr(10);
            std::string clean;
            for (char c : v) {
                if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '_' || c == '-')
                    clean += c;
            }
            return clean;
        }
    }
    return "";
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int showCmd) {
    // UTF-8 console/files everywhere; DPI aware so the 3D hero isn't blurry.
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    setlocale(LC_ALL, ".UTF8");

    // Reconstruct argc/argv from the wide command line (WinMain gives wide).
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);   // includes argv[0] = exe path, so parseProfile's i=1 is right
    std::vector<std::string> argvUtf8;
    for (int i = 0; wargv && i < argc; ++i) argvUtf8.push_back(hek::narrow(wargv[i]));
    if (wargv) LocalFree(wargv);
    std::vector<char*> argvPtrs;
    for (auto& s : argvUtf8) argvPtrs.push_back(s.data());

    hek::Context ctx;
    std::string profile = parseProfile((int)argvPtrs.size(), argvPtrs.data());
    if (!profile.empty()) ctx.setProfile(profile);

    ctx.registerBuiltins();

    hek::WindowOptions opts;
    opts.title  = profile.empty() ? "Hekpixle"
                                    : (std::string("Hekpixle — ") + profile);
    opts.width  = 1280;
    opts.height = 820;

    return hek::runWindow(ctx, opts);
}
