// Hekpixle — IPC dispatcher.
//
// Receives {id, method, args} from the renderer (via window.cpp) and routes it
// to the right backend function. Returns {id, ok, result|error} for window.cpp
// to post back. Streaming methods (agent:run, telegram:start) emit events via
// ctx.emit() as they run, then resolve with the final result.
//
// Method list MUST stay in sync with kIpcShimJs (ipc_shim.cpp).
#include "ipc.hpp"
#include "context.hpp"
#include "agent.hpp"
#include "util.hpp"
#include "tools.hpp"
#include "telegram.hpp"

#include <windows.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <commdlg.h>
#include <thread>
#include <atomic>
#include <memory>

namespace hek {

// ----- running agent state (one at a time, like main.js currentAbortController) -----
namespace {
struct AgentRun {
    std::atomic<bool> stop{false};
    Call call;
};
std::shared_ptr<AgentRun> g_currentRun;
std::mutex g_runMu;

// Telegram module — long-lived, singleton.
TelegramBot* g_telegram = nullptr;
std::mutex g_tgMu;
TelegramBot* telegram() {
    std::lock_guard<std::mutex> lk(g_tgMu);
    if (!g_telegram) g_telegram = new TelegramBot();
    return g_telegram;
}

// Forward decls for the handlers.
json handleSettingsGet(Context& ctx);
json handleSettingsSet(Context& ctx, const json& args);
json handleChatsLoad(Context& ctx);
json handleChatsSave(Context& ctx, const json& args);
json handleWorksLoad(Context& ctx);
json handleWorksSave(Context& ctx, const json& args);
json handleAgentRun(Context& ctx, const json& args);
json handleAgentStop(Context& ctx);
json handlePermissionAnswer(Context& ctx, const json& args);
json handleClarifyAnswer(Context& ctx, const json& args);
json handleShellOpenExternal(const json& args);
json handleDialogOpenFile();
json handleFileReadInline(Context& ctx, const json& args);
json handleSkillsList(Context& ctx);
json handleCronList(Context& ctx);
json handleCronAction(Context& ctx, const json& args);
json handleToolsInfo(Context& ctx);
json handleToolsOpen(Context& ctx, const json& args);
json handleChatCompress(Context& ctx, const json& args);
json handleTelegramStart(Context& ctx);
json handleTelegramStop();
json handleTelegramStatus();
json handleTelegramTest(const json& args);

// Run a handler that may throw; convert exceptions to {ok:false,error}.
template <typename Fn>
json safeRun(int id, Fn fn) {
    try {
        return {{"id", id}, {"ok", true}, {"result", fn()}};
    } catch (const std::exception& e) {
        return {{"id", id}, {"ok", false}, {"error", e.what()}};
    } catch (...) {
        return {{"id", id}, {"ok", false}, {"error", "unknown error"}};
    }
}
} // namespace

json ipcDispatch(Context& ctx, const json& msg) {
    int id = msg.value("id", 0);
    std::string method = msg.value("method", "");
    const json& args = msg.value("args", json::array());

    auto arg0 = [&]() -> json { return args.size() > 0 ? args[0] : json(nullptr); };
    auto arg1 = [&]() -> json { return args.size() > 1 ? args[1] : json(nullptr); };

    // --- settings ---
    if (method == "settings:get")   return safeRun(id, [&] { return handleSettingsGet(ctx); });
    if (method == "settings:set")   return safeRun(id, [&] { return handleSettingsSet(ctx, args); });
    // --- chats / works ---
    if (method == "chats:load")     return safeRun(id, [&] { return handleChatsLoad(ctx); });
    if (method == "chats:save")     return safeRun(id, [&] { return handleChatsSave(ctx, args); });
    if (method == "works:load")     return safeRun(id, [&] { return handleWorksLoad(ctx); });
    if (method == "works:save")     return safeRun(id, [&] { return handleWorksSave(ctx, args); });
    // --- agent ---
    if (method == "agent:run")      return safeRun(id, [&] { return handleAgentRun(ctx, args); });
    if (method == "agent:stop")     return safeRun(id, [&] { return handleAgentStop(ctx); });
    if (method == "permission:answer") return safeRun(id, [&] { return handlePermissionAnswer(ctx, args); });
    if (method == "clarify:answer") return safeRun(id, [&] { return handleClarifyAnswer(ctx, args); });
    // --- skills / cron / tools ---
    if (method == "skills:list")    return safeRun(id, [&] { return handleSkillsList(ctx); });
    if (method == "cron:list")      return safeRun(id, [&] { return handleCronList(ctx); });
    if (method == "cron:action")    return safeRun(id, [&] { return handleCronAction(ctx, args); });
    if (method == "tools:info")     return safeRun(id, [&] { return handleToolsInfo(ctx); });
    if (method == "tools:open")     return safeRun(id, [&] { return handleToolsOpen(ctx, args); });
    if (method == "chat:compress")  return safeRun(id, [&] { return handleChatCompress(ctx, args); });
    // --- shell / dialog / files ---
    if (method == "shell:openExternal") return safeRun(id, [&] { return handleShellOpenExternal(args); });
    if (method == "dialog:openFile")    return safeRun(id, [&] { return handleDialogOpenFile(); });
    if (method == "file:readInline")    return safeRun(id, [&] { return handleFileReadInline(ctx, args); });
    // --- telegram ---
    if (method == "telegram:start")  return safeRun(id, [&] { return handleTelegramStart(ctx); });
    if (method == "telegram:stop")   return safeRun(id, [&] { return handleTelegramStop(); });
    if (method == "telegram:status") return safeRun(id, [&] { return handleTelegramStatus(); });
    if (method == "telegram:test")   return safeRun(id, [&] { return handleTelegramTest(args); });

    return {{"id", id}, {"ok", false}, {"error", "unknown method: " + method}};
}

// ============================ handlers ============================

json handleSettingsGet(Context& ctx)  { return ctx.loadSettings(); }
json handleSettingsSet(Context& ctx, const json& args) {
    ctx.saveSettings(args.at(0));
    return true;
}
json handleChatsLoad(Context& ctx) { return ctx.readFile("chats.json", json::array()); }
json handleChatsSave(Context& ctx, const json& args) {
    ctx.writeFile("chats.json", args.at(0));
    return true;
}
json handleWorksLoad(Context& ctx) { return ctx.readFile("works.json", json::array()); }
json handleWorksSave(Context& ctx, const json& args) {
    ctx.writeFile("works.json", args.at(0));
    return true;
}

json handleAgentRun(Context& ctx, const json& args) {
    json messages = args.at(0);
    auto run = std::make_shared<AgentRun>();
    {
        std::lock_guard<std::mutex> lk(g_runMu);
        // Cancel any previous run first.
        if (g_currentRun) g_currentRun->stop = true;
        g_currentRun = run;
    }

    Call& call = run->call;
    call.source = "desktop";
    call.depth  = 0;
    call.emit   = [&ctx](const json& ev) { ctx.emit(ev); };
    call.stop   = false;
    call.askUser = [&ctx](const std::string& q, const std::vector<std::string>& choices) {
        return ctx.askClarify(q, choices);
    };
    call.sendTelegram = [](const std::string& text, const std::string& chatId) {
        return telegram()->sendMessage(text, chatId);
    };
    call.runSubAgent = [&ctx](const json& task, Call& /*parent*/) -> std::string {
        Call sub;
        sub.source = "desktop";
        sub.depth  = 1;
        sub.emit   = [](const json&){};
        sub.askUser = [](const std::string&, const std::vector<std::string>&){ return std::string(); };
        sub.sendTelegram = [](const std::string&, const std::string&){ return std::string("ERROR: sub-agents cannot send Telegram"); };
        sub.runSubAgent = [](const json&, Call&){ return std::string("ERROR: sub-agents cannot delegate further"); };
        // Build a minimal message list: system + the goal as a user turn.
        json msgs = json::array();
        msgs.push_back({{"role", "user"}, {"content",
            task.value("goal", "") + (task.contains("context") ? "\n\nContext:\n" + task["context"].get<std::string>() : "")}});
        return runAgent(ctx, msgs, sub);
    };

    std::string finalText = runAgent(ctx, messages, call);

    {
        std::lock_guard<std::mutex> lk(g_runMu);
        if (g_currentRun.get() == run.get()) g_currentRun.reset();
    }
    return finalText;
}

json handleAgentStop(Context& ctx) {
    std::lock_guard<std::mutex> lk(g_runMu);
    if (g_currentRun) {
        g_currentRun->stop = true;
        g_currentRun.reset();
    }
    ctx.denyAllPending();
    return true;
}

json handlePermissionAnswer(Context& ctx, const json& args) {
    int id = args.at(0).get<int>();
    std::string ok = args.at(1).is_boolean()
        ? (args.at(1).get<bool>() ? "allow" : "deny")
        : args.at(1).get<std::string>();
    ctx.answerPermission(id, ok);
    return true;
}

json handleClarifyAnswer(Context& ctx, const json& args) {
    int id = args.at(0).get<int>();
    std::string text = args.at(1).is_string() ? args.at(1).get<std::string>() : "";
    ctx.answerClarify(id, text);
    return true;
}

json handleShellOpenExternal(const json& args) {
    std::string url = args.at(0).get<std::string>();
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return true;
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return true;
}

json handleDialogOpenFile() {
    char buf[MAX_PATH] = {0};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    json out = json::array();
    if (GetOpenFileNameA(&ofn)) out.push_back(std::string(buf));
    return out;
}

json handleFileReadInline(Context& ctx, const json& args) {
    std::string p = args.at(0).get<std::string>();
    HANDLE h = CreateFileA(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot read file: " + p);
    LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
    std::string content((size_t)sz.QuadPart, '\0');
    DWORD read = 0;
    ReadFile(h, content.data(), (DWORD)content.size(), &read, nullptr);
    CloseHandle(h);
    return content;
}

json handleSkillsList(Context& ctx) {
    // Walk userDataDir/skills/*/SKILL.md + skills_seed/*/SKILL.md.
    json out = json::array();
    auto collect = [&](const std::string& dir, const std::string& source) {
        for (auto& name : listDir(dir, /*dirsOnly*/true)) {
            std::string md = dir + "\\" + name + "\\SKILL.md";
            HANDLE h = CreateFileA(md.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h == INVALID_HANDLE_VALUE) continue;
            LARGE_INTEGER sz; GetFileSizeEx(h, &sz);
            std::string s((size_t)sz.QuadPart, '\0');
            DWORD r = 0; ReadFile(h, s.data(), (DWORD)s.size(), &r, nullptr);
            CloseHandle(h);
            // First line starting with "# " is the title; first paragraph is description.
            std::string title = name, desc;
            size_t pos = s.find("# ");
            if (pos != std::string::npos) {
                size_t e = s.find('\n', pos);
                title = trim(s.substr(pos + 2, e - pos - 2));
            }
            // Description: first non-empty line that isn't a heading.
            size_t p = 0;
            while (p < s.size()) {
                size_t nl = s.find('\n', p);
                std::string line = trim(s.substr(p, nl - p));
                p = (nl == std::string::npos) ? s.size() : nl + 1;
                if (line.empty() || line[0] == '#') continue;
                desc = line; break;
            }
            out.push_back({{"name", name}, {"description", desc}, {"source", source}});
        }
    };
    collect(ctx.userDataDir() + "\\skills", "user");
    collect(exeDir() + "\\skills_seed", "builtin");
    return out;
}

json handleCronList(Context& ctx)      { return cron::list(ctx); }
json handleCronAction(Context& ctx, const json& args) { return cron::act(ctx, args.at(0)); }

json handleToolsInfo(Context& ctx) {
    json toolsets = json::array();
    auto s = ctx.loadSettings();
    json enabled = s.value("toolsets", json::object());
    for (auto& ts : ctx.toolsets()) {
        bool on = !enabled.contains(ts) || enabled[ts].get<bool>() != false;
        int count = 0;
        for (auto& t : ctx.tools()) if (t.toolset == ts) ++count;
        toolsets.push_back({{"name", ts}, {"enabled", on}, {"count", count}});
    }
    return {
        {"toolsets", toolsets},
        {"mcp", mcp::status()},
        {"plugins", plugins::status(ctx)},
        {"skillsDir", ctx.userDataDir() + "\\skills"},
        {"userData", ctx.userDataDir()},
    };
}

json handleToolsOpen(Context& ctx, const json& args) {
    std::string what = args.at(0).get<std::string>();
    std::string dir = (what == "plugins")
        ? ctx.userDataDir() + "\\plugins"
        : ctx.userDataDir() + "\\skills";
    ensureDir(dir);
    ShellExecuteA(nullptr, "open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return true;
}

json handleChatCompress(Context& ctx, const json& args) {
    json messages = args.at(0);
    json out = compressChat(ctx, messages);
    if (out.contains("_error")) throw std::runtime_error(out["_error"].get<std::string>());
    return out;
}

json handleTelegramStart(Context& ctx) {
    auto s = ctx.loadSettings();
    auto tg = s.value("telegram", json::object());
    telegram()->start(tg);
    return true;
}
json handleTelegramStop()   { telegram()->stop(); return true; }
json handleTelegramStatus() { return telegram()->status(); }
json handleTelegramTest(const json& args) {
    std::string token = args.at(0).get<std::string>();
    std::string proxy = args.size() > 1 ? args.at(1).get<std::string>() : "";
    return telegram()->test(token, proxy);
}

} // namespace hek
