// Hekpixle — Context implementation: tool registry, settings, userData,
// permission/clarify prompts.
//
// The permission flow mirrors main.js: the agent loop calls askPermission(),
// which emits a {type:"permission", id, title, detail} event to the UI and
// blocks on a condition variable until answerPermission(id, ok) arrives
// (or agent:stop calls denyAllPending()).
#include "context.hpp"
#include "util.hpp"
#include "tools.hpp"

#include <windows.h>
#include <shlobj.h>
#include <chrono>
#include <fstream>
#include <sstream>

namespace hek {

// ---------- JSON file helpers ----------
json read_json_file(const std::string& path, const json& fallback) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return fallback;
    try {
        json j; f >> j; return j;
    } catch (...) { return fallback; }
}

bool write_json_file(const std::string& path, const json& value) {
    ensureDir(path.substr(0, path.find_last_of("\\/")));
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        if (!f) return false;
        f << value.dump(2);
        if (!f) return false;
    }
    return MoveFileA(tmp.c_str(), path.c_str()) != 0;
}

std::wstring widen(const std::string& utf8) {
    if (utf8.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), (int)utf8.size(), w.data(), n);
    return w;
}
std::string narrow(const std::wstring& utf16) {
    if (utf16.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, utf16.c_str(), (int)utf16.size(),
                                nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, utf16.c_str(), (int)utf16.size(),
                        s.data(), n, nullptr, nullptr);
    return s;
}

// ---------- Context ----------
void Context::registerBuiltins() {
    web::registerTools(*this);
    files::registerTools(*this);
    computer::registerTools(*this);
    browser::registerTools(*this);
    mcp::registerTools(*this);
    media::registerTools(*this);
    procs::registerTools(*this);
    ps::registerTools(*this);
    skills::registerTools(*this);
    cron::registerTools(*this);
    plugins::registerTools(*this);
    agent_tools::registerTools(*this);
}

void Context::add(Tool t) { tools_.push_back(std::move(t)); }

const Tool* Context::find(const std::string& name) const {
    for (auto& t : tools_) if (t.name == name) return &t;
    return nullptr;
}

std::vector<std::string> Context::toolsets() const {
    std::vector<std::string> out;
    for (auto& t : tools_) {
        if (std::find(out.begin(), out.end(), t.toolset) == out.end())
            out.push_back(t.toolset);
    }
    return out;
}

std::string Context::userDataDir() const { return hek::userDataDir(profile_); }
void Context::setProfile(const std::string& p) { profile_ = p; }

json Context::defaultSettings() const {
    // Mirrors DEFAULT_SETTINGS in main.js. Keep the keys/structure identical
    // so renderer/app.js settings UI works unchanged.
    return {
        {"apiKey", ""},
        {"baseUrl", "https://api.openai.com/v1"},
        {"model", "gpt-4o-mini"},
        {"theme", "light"},
        {"reduceMotion", false},
        {"llmProxyUrl", ""},
        {"personality", ""},
        {"fallbackBaseUrl", ""},
        {"fallbackApiKey", ""},
        {"fallbackModel", ""},
        {"maxSteps", 30},
        {"approvalMode", "manual"},
        {"alwaysAllow", json::array()},
        {"workspaceDir", ""},
        {"extraSkillsDir", ""},
        {"browserVisible", true},
        {"compressAtChars", 90000},
        {"toolsets", json::object()},
        {"web", {
            {"provider", "duckduckgo"},
            {"apiKey", ""},
            {"searxUrl", ""},
            {"proxyUrl", ""},
        }},
        {"media", {
            {"baseUrl", ""},
            {"apiKey", ""},
            {"visionMode", "direct"},
            {"visionModel", ""},
            {"visionBaseUrl", ""},
            {"visionApiKey", ""},
            {"imageModel", ""},
            {"ttsModel", ""},
            {"ttsVoice", ""},
            {"sttModel", ""},
        }},
        {"mcpServers", json::object()},
        {"cron", {{"autoAllowReads", true}, {"autoAllowWrites", false}}},
        {"telegram", {
            {"botToken", ""},
            {"proxyUrl", ""},
            {"allowedUserIds", json::array()},
            {"replyOnUnauthorized", true},
            {"autoAllowReads", true},
            {"autoAllowWrites", false},
            {"onlyWhenMentioned", true},
            {"autoStart", false},
        }},
    };
}

json Context::loadSettings() const {
    json raw = readFile("settings.json", json::object());
    json def = defaultSettings();
    // shallow + nested merge for the known object fields
    json out = def;
    if (raw.is_object()) {
        for (auto it = raw.begin(); it != raw.end(); ++it) {
            if (out.contains(it.key()) && out[it.key()].is_object() && it.value().is_object()) {
                json merged = out[it.key()];
                for (auto m = it.value().begin(); m != it.value().end(); ++m)
                    merged[m.key()] = m.value();
                out[it.key()] = merged;
            } else {
                out[it.key()] = it.value();
            }
        }
    }
    return out;
}

void Context::saveSettings(const json& v) { writeFile("settings.json", v); }

json Context::readFile(const std::string& name, const json& fallback) const {
    return read_json_file(userDataDir() + "\\" + name, fallback);
}
bool Context::writeFile(const std::string& name, const json& v) const {
    return write_json_file(userDataDir() + "\\" + name, v);
}

void Context::emit(const json& ev) const {
    std::lock_guard<std::mutex> lk(mu_);
    if (emitter_) emitter_(ev);
}

// ---------- permission / clarify prompts ----------
namespace {
struct Prompt { std::string answer; bool done = false; };
}

std::string Context::askPermission(const std::string& title, const std::string& detail) {
    std::lock_guard<std::mutex> lk(mu_);
    int id = nextId_++;
    auto pr = std::make_shared<Prompt>();
    pendingPerms_[id] = {[pr](const std::string& a) { pr->answer = a; pr->done = true; }};

    json ev = {{"type", "permission"}, {"id", id}, {"title", title}, {"detail", detail}};
    if (emitter_) emitter_(ev);

    // The condition variable is owned by the caller thread; we poll with sleep
    // because the answer arrives on the IPC thread and we can't share a cv
    // easily across the global mutex here. Polling at 50ms is responsive enough.
    mu_.unlock();
    while (!pr->done) Sleep(50);
    mu_.lock();
    return pr->answer.empty() ? std::string("deny") : pr->answer;
}

std::string Context::askClarify(const std::string& question, const std::vector<std::string>& choices) {
    std::lock_guard<std::mutex> lk(mu_);
    int id = nextId_++;
    auto pr = std::make_shared<Prompt>();
    pendingClarify_[id] = {[pr](const std::string& a) { pr->answer = a; pr->done = true; }};

    json ch = json::array();
    for (auto& c : choices) ch.push_back(c);
    json ev = {{"type", "clarify"}, {"id", id}, {"question", question}, {"choices", ch}};
    if (emitter_) emitter_(ev);

    mu_.unlock();
    while (!pr->done) Sleep(50);
    mu_.lock();
    return pr->answer;
}

void Context::answerPermission(int id, const std::string& ok) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = pendingPerms_.find(id);
    if (it != pendingPerms_.end()) {
        it->second.fn(ok);
        pendingPerms_.erase(it);
    }
}

void Context::answerClarify(int id, const std::string& text) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = pendingClarify_.find(id);
    if (it != pendingClarify_.end()) {
        it->second.fn(text);
        pendingClarify_.erase(it);
    }
}

void Context::denyAllPending() {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& kv : pendingPerms_) kv.second.fn("deny");
    pendingPerms_.clear();
    for (auto& kv : pendingClarify_) kv.second.fn("");
    pendingClarify_.clear();
}

} // namespace hek
