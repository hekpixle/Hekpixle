// Hekpixle — agent context + tool registry.
//
// This mirrors the JS architecture in main.js + lib/*.js:
//   - Tool:  { name, toolset, category, description, parameters(JSON Schema),
//              run(args, call) -> string,  optional confirm(args)->{title,detail} }
//   - Call:   per-invocation state passed to a tool: emit(), askUser(),
//             sendTelegram(), runSubAgent(), source, depth, todos, stop flag.
//   - Context: holds userDataDir(), settings, tool registry, emit() to UI.
//
// The agent loop (agent.cpp) calls the LLM, parses tool_calls, asks for
// permission (category=='write'/'control'), runs the tool, and streams events.
#pragma once
#include "json.hpp"
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace hek {

// One running agent invocation. Tools read/write state through it.
struct Call {
    std::string source;            // "desktop" | "telegram"
    int depth = 0;                 // 0 = top-level agent, 1 = sub-agent
    std::atomic<bool> stop{false}; // set by agent:stop
    json todos = json::array();    // shared todo list (todo tool)
    std::function<void(const json&)> emit;          // stream event to UI
    std::function<std::string(const std::string&, const std::vector<std::string>&)> askUser;        // clarify
    std::function<std::string(const std::string&, const std::string&)> sendTelegram;                // (text, chat_id)
    std::function<std::string(const json& task, Call& parent)> runSubAgent;
};

// A registered tool. `run` returns the text fed back to the LLM.
struct Tool {
    std::string name;
    std::string toolset;
    std::string category;          // "read" | "write" | "control"
    std::string description;
    json parameters;               // JSON Schema
    std::function<std::string(const json& args, Call& call)> run;
    // Optional: produce a permission prompt. Empty title => no prompt needed.
    std::function<json(const json& args)> confirm;
};

// Answer to a permission / clarify prompt.
using AnswerFn = std::function<void(const std::string&)>;

// Context — long-lived, owns the tool registry + data dir + settings cache.
class Context {
public:
    // Register every built-in tool module (web, files, computer, ...).
    void registerBuiltins();

    // Tool registry.
    void add(Tool t);
    const Tool* find(const std::string& name) const;
    const std::vector<Tool>& tools() const { return tools_; }
    std::vector<std::string> toolsets() const;

    // Data directory (mirrors Electron app.getPath('userData')).
    // %APPDATA%\Hekpixle  (or %APPDATA%\Hekpixle-<profile> with --profile=)
    std::string userDataDir() const;
    void setProfile(const std::string& p);

    // Settings (settings.json) with DEFAULT_SETTINGS merge.
    json defaultSettings() const;
    json loadSettings() const;
    void saveSettings(const json& v);

    // Persistent JSON files in userDataDir (chats.json, works.json, memory.json...).
    json readFile(const std::string& name, const json& fallback) const;
    bool writeFile(const std::string& name, const json& v) const;

    // UI event streaming. Wired to WebView2 in main.cpp.
    void setEmitter(std::function<void(const json&)> e) { emitter_ = std::move(e); }
    void emit(const json& ev) const;

    // Permission / clarify prompt plumbing (UI answers via IPC).
    // Returns "allow" | "always" | "deny" for permissions; arbitrary text for clarify.
    std::string askPermission(const std::string& title, const std::string& detail);
    std::string askClarify(const std::string& question, const std::vector<std::string>& choices);
    void answerPermission(int id, const std::string& ok);
    void answerClarify(int id, const std::string& text);
    void denyAllPending();  // called on agent:stop

private:
    std::vector<Tool> tools_;
    std::string profile_;
    mutable std::mutex mu_;
    std::function<void(const json&)> emitter_;

    struct Pending { AnswerFn fn; };
    std::map<int, Pending> pendingPerms_;
    std::map<int, Pending> pendingClarify_;
    int nextId_ = 1;
};

} // namespace hek
