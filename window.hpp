// Hekpixle — Win32 + WebView2 window host.
//
// Creates the main window, embeds WebView2, loads renderer/index.html, injects
// the IPC shim, and bridges WebMessageReceived -> ipcDispatch -> PostWebMessageAsJson.
// Mirrors the Electron BrowserWindow + preload.js setup in main.js.
#pragma once
#include "context.hpp"
#include <string>

namespace hek {

struct WindowOptions {
    int width = 1280;
    int height = 820;
    std::string title = "Hekpixle";
};

// Run the app message loop. Returns the Win32 exit code.
int runWindow(Context& ctx, const WindowOptions& opts);

} // namespace hek
