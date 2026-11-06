// Hekpixle — IPC bridge between the WebView2 renderer and the C++ backend.
//
// The renderer calls window.hek.<method>(...) exactly as in the Electron
// preload.js. We inject a JS shim (ipc_shim.js) that turns each call into a
// postMessage {id, method, args}; native handles it and posts back
// {id, ok, result|error}. Streaming events (onEvent / onTelegramStatus) come
// as {event, data} messages the shim dispatches to registered callbacks.
//
// This preserves the EXACT preload.js contract, so renderer/app.js is unchanged.
#pragma once
#include "context.hpp"
#include "json.hpp"
#include <string>

namespace hek {

// The JS injected before every page load — recreates window.hek.* on top of
// window.chrome.webview.postMessage / addEventListener. Keep this in sync with
// the method list in ipcDispatch().
extern const char* kIpcShimJs;

// Handle one {id, method, args} message from the renderer.
// Returns the JSON to post back: {"id":..,"ok":true,"result":..} or
// {"id":..,"ok":false,"error":".."}. May also stream events via ctx.emit().
json ipcDispatch(Context& ctx, const json& msg);

} // namespace hek
