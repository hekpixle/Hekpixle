// Hekpixle — IPC shim injected into every page.
//
// This recreates the EXACT window.hek.* surface from the original Electron
// preload.js, on top of WebView2's chrome.webview messaging. Because of this,
// renderer/app.js is unchanged: it still calls window.hek.runAgent(...), etc.
//
// Transport:
//   page -> native : window.chrome.webview.postMessage(JSON.stringify({id, method, args}))
//   native -> page : window.chrome.webview.addEventListener('message', ...)
//                    {id, ok, result}        -> resolves/rejects the promise for that call
//                    {event, data}           -> dispatches to the registered onEvent callback
//
// Each method returns a Promise, matching ipcRenderer.invoke() semantics.

#include "ipc.hpp"

namespace hek {

// Keep in sync with the method table in ipcDispatch() (ipc.cpp).
const char* kIpcShimJs = R"JS(
(function () {
  if (window.__hekInstalled) return;
  window.__hekInstalled = true;

  var nextId = 1;
  var pending = {};           // id -> {resolve, reject}
  var listeners = {           // event name -> callback
    'agent:event': null,
    'telegram:status': null,
  };

  function post(msg) {
    window.chrome.webview.postMessage(JSON.stringify(msg));
  }

  function invoke(method, args) {
    return new Promise(function (resolve, reject) {
      var id = nextId++;
      pending[id] = { resolve: resolve, reject: reject };
      post({ id: id, method: method, args: args || [] });
    });
  }

  window.chrome.webview.addEventListener('message', function (ev) {
    var data = ev.data;
    if (typeof data === 'string') { try { data = JSON.parse(data); } catch (e) { return; } }
    if (!data || typeof data !== 'object') return;

    if (data.event) {
      // streaming event
      var cb = listeners[data.event];
      if (typeof cb === 'function') {
        try { cb(data.data); } catch (e) { console.error('hek event handler', e); }
      }
      return;
    }
    // response to a call
    var p = pending[data.id];
    if (!p) return;
    delete pending[data.id];
    if (data.ok) p.resolve(data.result);
    else p.reject(new Error(data.error || 'IPC error'));
  });

  // ----- The window.hek surface (matches preload.js exactly) -----
  window.hek = {
    getSettings:      function ()            { return invoke('settings:get'); },
    setSettings:      function (v)           { return invoke('settings:set', [v]); },
    loadChats:        function ()            { return invoke('chats:load'); },
    saveChats:        function (v)           { return invoke('chats:save', [v]); },
    loadWorks:        function ()            { return invoke('works:load'); },
    saveWorks:        function (v)           { return invoke('works:save', [v]); },
    runAgent:         function (messages)    { return invoke('agent:run', [messages]); },
    stopAgent:        function ()            { return invoke('agent:stop'); },
    answerPermission: function (id, ok)      { return invoke('permission:answer', [id, ok]); },
    openExternal:     function (url)         { return invoke('shell:openExternal', [url]); },
    answerClarify:    function (id, text)    { return invoke('clarify:answer', [id, text]); },
    listSkills:       function ()            { return invoke('skills:list'); },
    cronList:         function ()            { return invoke('cron:list'); },
    cronAction:       function (a)           { return invoke('cron:action', [a]); },
    toolsInfo:        function ()            { return invoke('tools:info'); },
    openToolsDir:     function (what)        { return invoke('tools:open', [what]); },
    compressChat:     function (messages)    { return invoke('chat:compress', [messages]); },
    onEvent:          function (cb)          { listeners['agent:event'] = cb; },
    pickFiles:        function ()            { return invoke('dialog:openFile'); },
    readInline:       function (filePath)    { return invoke('file:readInline', [filePath]); },
    telegramStart:    function ()            { return invoke('telegram:start'); },
    telegramStop:     function ()            { return invoke('telegram:stop'); },
    telegramStatus:   function ()            { return invoke('telegram:status'); },
    telegramTest:     function (token, p)    { return invoke('telegram:test', [token, p]); },
    onTelegramStatus: function (cb)          { listeners['telegram:status'] = cb; },
    nodeVersion:      function ()            { return '20.10.0'; },       // stubs for renderer version badges
    electronVersion:  function ()            { return '0.0.0'; },
    chromeVersion:    function ()            {
      // Real Edge/Chromium version, used only for an "about" badge.
      return (navigator.userAgent.match(/Chrome\/([\d.]+)/) || [])[1] || '0.0.0';
    },
  };

  // Fallback: some renderer code may call window.hek.tts / window.hek.imageGen
  // that were declared-but-undefined in the original preload too. Mirror that
  // by leaving them undefined (Promise.reject) so behaviour matches Electron.
})();
)JS";

} // namespace hek
