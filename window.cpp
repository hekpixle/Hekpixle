// Hekpixle — Win32 window + WebView2 host.
//
// Creates an HWND, embeds a WebView2 (Edge/Chromium), loads renderer/index.html,
// injects the IPC shim before each navigation, and bridges
//   WebView2 -> WebMessageReceived -> ipcDispatch -> PostWebMessageAsJson
// plus  ctx.emit() -> PostWebMessageAsJson for streaming events.
//
// The renderer never knows it's not Electron: window.hek.* works identically.
#include "window.hpp"
#include "ipc.hpp"
#include "context.hpp"
#include "util.hpp"

#include <windows.h>
#include <shlwapi.h>
#include <objbase.h>
#include <shlobj.h>
#include <wrl.h>
#include <WebView2.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shell32.lib")

#include <atomic>
#include <string>
#include <thread>

using namespace Microsoft::WRL;

namespace hek {

// Global, single-window. WebView2 callbacks are COM-style; we keep the
// controller / view in static storage so lambdas can reach them.
namespace {
struct AppState {
    Context* ctx = nullptr;
    HWND hwnd = nullptr;
    ComPtr<ICoreWebView2Controller> controller;
    ComPtr<ICoreWebView2> webview;
    std::atomic<bool> ready{false};
};
AppState g;

std::string indexPath() {
    char exe[MAX_PATH];
    GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string dir = exe;
    size_t slash = dir.find_last_of("\\/");
    if (slash != std::string::npos) dir = dir.substr(0, slash);
    std::string url;
    for (char c : dir + "\\renderer\\index.html") {
        if (c == '\\') url += '/';
        else if (c == ' ') url += "%20";
        else url += c;
    }
    return "file:///" + url;
}

// Forward
LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);

void resizeWebView() {
    if (!g.controller) return;
    RECT r;
    GetClientRect(g.hwnd, &r);
    g.controller->put_Bounds(r);
}

// WebView2 objects may only be touched on the UI thread, but the agent loop and tool
// calls run on worker threads. So every message to the page is queued to the UI thread
// with PostMessage and delivered in WndProc (WM_HEK_TO_VIEW).
constexpr UINT WM_HEK_TO_VIEW = WM_APP + 1;

void postToView(const json& msg) {
    if (!g.hwnd) return;
    // dump(-1, ' ', false, replace) never throws on invalid UTF-8
    auto* s = new std::string(msg.dump(-1, ' ', false, json::error_handler_t::replace));
    if (!PostMessageW(g.hwnd, WM_HEK_TO_VIEW, 0, (LPARAM)s)) delete s;
}

// Runs on the UI thread (called from WndProc).
void deliverToView(std::string* s) {
    if (g.webview) g.webview->PostWebMessageAsJson(widen(*s).c_str());
    delete s;
}

void onEmitter(const json& ev) {
    // Streaming events arrive here from the agent loop and tool modules.
    // Wrap as {"event":"agent:event","data":ev} — the shim dispatches it.
    json wrap = {{"event", "agent:event"}, {"data", ev}};
    postToView(wrap);
}

// Runs ipcDispatch on a worker thread so long calls (agent runs, permission prompts that
// wait for the user's answer) never freeze the window.
void dispatchAsync(std::string text) {
    std::thread([text]() {
        int id = 0;
        try {
            json msg = json::parse(text);
            id = msg.value("id", 0);
            postToView(ipcDispatch(*g.ctx, msg));
        } catch (const std::exception& e) {
            // Tell the renderer the call failed so its promise rejects.
            postToView(json{{"id", id}, {"ok", false}, {"error", std::string("bad message: ") + e.what()}});
        }
    }).detach();
}

// Handle a {id, method, args} message from the renderer (UI thread).
void onWebMessage(ICoreWebView2WebMessageReceivedEventArgs* args) {
    LPWSTR wide = nullptr;
    if (SUCCEEDED(args->TryGetWebMessageAsString(&wide)) && wide) {
        std::string s = narrow(std::wstring(wide));
        CoTaskMemFree(wide);
        dispatchAsync(std::move(s));
        return;
    }
    // The page posted an object instead of a string: the JSON text is the message itself.
    LPWSTR j = nullptr;
    if (SUCCEEDED(args->get_WebMessageAsJson(&j)) && j) {
        std::string s = narrow(std::wstring(j));
        CoTaskMemFree(j);
        dispatchAsync(std::move(s));
    }
}

void onWebviewCreated(HRESULT hr, ICoreWebView2Controller* ctrl) {
    if (FAILED(hr) || !ctrl) {
        MessageBoxA(g.hwnd,
            "WebView2 failed to initialize.\n\n"
            "Install the Microsoft Edge WebView2 Runtime:\n"
            "https://developer.microsoft.com/microsoft-edge/webview2/",
            "Hekpixle", MB_ICONERROR);
        PostQuitMessage(1);
        return;
    }
    g.controller = ctrl;
    g.controller->get_CoreWebView2(&g.webview);

    // Messages from the page.
    EventRegistrationToken tok{};
    g.webview->add_WebMessageReceived(
        Callback<ICoreWebView2WebMessageReceivedEventHandler>(
            [](ICoreWebView2*, ICoreWebView2WebMessageReceivedEventArgs* a) -> HRESULT {
                onWebMessage(a);
                return S_OK;
            }).Get(), &tok);

    // Inject the IPC shim into every document, BEFORE its scripts run (so window.hek exists
    // when app.js starts). Must be registered once, before Navigate.
    std::wstring js = widen(std::string(kIpcShimJs));
    g.webview->AddScriptToExecuteOnDocumentCreated(js.c_str(), nullptr);

#ifndef NDEBUG
    g.webview->put_IsDevToolsEnabled(TRUE);
#else
    g.webview->put_IsDevToolsEnabled(FALSE);
#endif

    // Background colour matches the Hekpixle bone theme so there's no white flash.
    COREWEBVIEW2_COLOR bg{};
    bg.R = 0xEC; bg.G = 0xE7; bg.B = 0xDC; bg.A = 255;
    ICoreWebView2Controller2* c2 = nullptr;
    if (SUCCEEDED(g.controller->QueryInterface(IID_PPV_ARGS(&c2))) && c2) {
        c2->put_DefaultBackgroundColor(bg);
        c2->Release();
    }

    resizeWebView();
    g.ready = true;

    // Route agent events to the renderer.
    g.ctx->setEmitter(onEmitter);

    // Load the renderer.
    std::wstring url = widen(indexPath());
    g.webview->Navigate(url.c_str());
}

int messageLoop() {
    MSG msg;
    while (GetMessage(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}
LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_HEK_TO_VIEW:
            deliverToView((std::string*)lp);
            return 0;
        case WM_SIZE:
            if (g.ready) resizeWebView();
            return 0;
        case WM_DESTROY:
            PostQuitMessage(0);
            return 0;
        case WM_GETMINMAXINFO: {
            // Keep a sane minimum window size.
            auto* mmi = (MINMAXINFO*)lp;
            mmi->ptMinTrackSize.x = 720;
            mmi->ptMinTrackSize.y = 480;
            return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

} // namespace

int runWindow(Context& ctx, const WindowOptions& opts) {
    g.ctx = &ctx;

    // COM apartment: WebView2 requires a single-threaded apartment.
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    bool uninitCom = SUCCEEDED(hrCom);

    std::wstring wtitle = widen(opts.title);
    std::wstring klass  = L"HekpixleWindow";

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = GetModuleHandle(nullptr);
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(WHITE_BRUSH);
    wc.lpszClassName = klass.c_str();
    wc.hIcon         = (HICON)LoadImageA(GetModuleHandle(nullptr), MAKEINTRESOURCEA(1),
                                          IMAGE_ICON, 0, 0, LR_DEFAULTSIZE);
    RegisterClassExW(&wc);

    int sw = GetSystemMetrics(SM_CXSCREEN);
    int sh = GetSystemMetrics(SM_CYSCREEN);
    int w  = opts.width  ? opts.width  : 1280;
    int h  = opts.height ? opts.height : 820;
    int x  = (sw - w) / 2;
    int y  = (sh - h) / 3;

    g.hwnd = CreateWindowExW(0, klass.c_str(), wtitle.c_str(),
        WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr,
        GetModuleHandle(nullptr), nullptr);
    if (!g.hwnd) return 1;

    ShowWindow(g.hwnd, SW_SHOW);
    UpdateWindow(g.hwnd);

    // Create the WebView2 environment + controller (async).
    HRESULT hr = CreateCoreWebView2EnvironmentWithOptions(
        nullptr,                       // default Edge install
        widen(ctx.userDataDir()).c_str(),  // user data folder
        nullptr,
        Callback<ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler>(
            [](HRESULT envHr, ICoreWebView2Environment* e) -> HRESULT {
                if (FAILED(envHr) || !e) { onWebviewCreated(envHr, nullptr); return S_OK; }
                e->CreateCoreWebView2Controller(g.hwnd,
                    Callback<ICoreWebView2CreateCoreWebView2ControllerCompletedHandler>(
                        [](HRESULT h, ICoreWebView2Controller* c) -> HRESULT {
                            onWebviewCreated(h, c);
                            return S_OK;
                        }).Get());
                return S_OK;
            }).Get());
    if (FAILED(hr)) {
        MessageBoxA(g.hwnd, "WebView2 runtime not found. Please install the "
                   "Microsoft Edge WebView2 Runtime.", "Hekpixle", MB_ICONERROR);
        return 1;
    }

    int rc = messageLoop();

    // Tear down.
    g.webview.Reset();
    g.controller.Reset();
    if (uninitCom) CoUninitialize();
    return rc;
}

} // namespace hek
