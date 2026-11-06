// Hekpixle — cron (scheduled tasks).
// Mirrors lib/cron.js. Stores jobs in userDataDir/cron.json and runs a
// background thread that fires due jobs (each job is an agent run with the
// job's message as the user turn). Supports 6-field cron expressions
// (sec min hour day month weekday) and fixed intervals.
#include "tools.hpp"
#include "util.hpp"
#include "context.hpp"
#include "agent.hpp"
#include <algorithm>
#include <chrono>
#include <ctime>
#include <thread>
#include <atomic>
#include <regex>

namespace hek::cron {

namespace {
std::atomic<bool> g_running{false};
std::thread g_thread;

// Parse the next fire time from a 6-field cron expression after `from`.
// Simplified: supports *, specific values, and */N step in any field.
time_t nextRun(const std::string& expr, time_t from) {
    auto fields = split(expr, ' ');
    if (fields.size() < 6) return 0;
    auto matchField = [](const std::string& f, int val, int lo, int hi) -> bool {
        if (f == "*") return true;
        if (f.rfind("*/",0) == 0) {
            int step = std::atoi(f.c_str()+2);
            return step > 0 && ((val - lo) % step == 0);
        }
        // comma list / range
        auto parts = split(f, ',');
        for (auto& p : parts) {
            size_t dash = p.find('-');
            if (dash != std::string::npos) {
                int a = std::atoi(p.c_str()), b = std::atoi(p.c_str()+dash+1);
                if (val >= a && val <= b) return true;
            } else if (std::atoi(p.c_str()) == val) return true;
        }
        (void)hi;
        return false;
    };
    // walk minute-by-minute (cap at ~1 day to avoid infinite loops)
    for (int i = 0; i < 1440; ++i) {
        time_t t = from + i * 60;
        std::tm lt; localtime_s(&lt, &t);
        if (matchField(fields[0], lt.tm_sec, 0, 59) &&
            matchField(fields[1], lt.tm_min, 0, 59) &&
            matchField(fields[2], lt.tm_hour, 0, 23) &&
            matchField(fields[3], lt.tm_mday, 1, 31) &&
            matchField(fields[4], lt.tm_mon+1, 1, 12) &&
            matchField(fields[5], lt.tm_wday, 0, 6)) {
            return t;
        }
    }
    return 0;
}

void loop(Context* ctx) {
    while (g_running) {
        json jobs = ctx->readFile("cron.json", json::array());
        time_t now = std::time(nullptr);
        bool changed = false;
        for (auto& j : jobs) {
            if (j.value("enabled", true) == false) continue;
            std::string sched = j.value("schedule", "");
            time_t next = j.value("nextRun", (time_t)0);
            if (next == 0) { next = nextRun(sched, now); j["nextRun"] = next; changed = true; }
            if (next > 0 && next <= now) {
                // fire
                ctx->emit({{"type","cron"},{"phase","start"},{"name",j.value("name","")},{"id",j.value("id",0)}});
                Call call;
                call.source = "desktop";
                call.depth = 0;
                call.emit = [ctx](const json& ev){ ctx->emit(ev); };
                call.askUser = [](const std::string&, const std::vector<std::string>&){ return std::string(); };
                call.sendTelegram = [](const std::string&, const std::string&){ return std::string(); };
                call.runSubAgent = [](const json&, Call&){ return std::string("ERROR: cron cannot delegate"); };
                json msgs = json::array({{{"role","user"},{"content",j.value("message","")}}});
                runAgent(*ctx, msgs, call);
                // record a run + schedule next
                json runs = ctx->readFile("cron_runs.json", json::array());
                runs.push_back({{"id",j.value("id",0)},{"name",j.value("name","")},{"at",now},{"ok",true}});
                if (runs.size() > 100) runs = json(runs.begin() + (runs.size() - 100), runs.end());
                ctx->writeFile("cron_runs.json", runs);
                j["nextRun"] = nextRun(sched, now + 60);
                changed = true;
            }
        }
        if (changed) ctx->writeFile("cron.json", jobs);
        std::this_thread::sleep_for(std::chrono::seconds(30));
    }
}

void ensureStarted(Context& ctx) {
    if (g_running.exchange(true)) return;
    g_thread = std::thread(loop, &ctx);
    g_thread.detach();
}

int nextId(Context& ctx) {
    json jobs = ctx.readFile("cron.json", json::array());
    int maxId = 0;
    for (auto& j : jobs) maxId = std::max(maxId, j.value("id", 0));
    return maxId + 1;
}
}

json list(Context& ctx) {
    ensureStarted(ctx);
    json jobs = ctx.readFile("cron.json", json::array());
    json runs = ctx.readFile("cron_runs.json", json::array());
    // last 15 runs, newest first
    if (runs.size() > 15) runs = json(runs.end() - 15, runs.end());
    std::reverse(runs.begin(), runs.end());
    return {{"jobs", jobs}, {"runs", runs}};
}

json act(Context& ctx, const json& a) {
    ensureStarted(ctx);
    std::string action = a.value("action", "list");
    json jobs = ctx.readFile("cron.json", json::array());
    if (action == "create") {
        json j = {
            {"id", nextId(ctx)},
            {"name", a.value("name", "unnamed")},
            {"schedule", a.value("schedule", "")},
            {"message", a.value("message", "")},
            {"enabled", true},
            {"nextRun", 0}
        };
        jobs.push_back(j);
        ctx.writeFile("cron.json", jobs);
        return j;
    }
    if (action == "update" || action == "delete" || action == "toggle") {
        int id = a.value("id", 0);
        json out = json::array();
        for (auto& j : jobs) {
            if (j.value("id", 0) == id) {
                if (action == "delete") continue;
                if (action == "toggle") j["enabled"] = !j.value("enabled", true);
                if (action == "update") {
                    if (a.contains("name")) j["name"] = a["name"];
                    if (a.contains("schedule")) { j["schedule"] = a["schedule"]; j["nextRun"] = 0; }
                    if (a.contains("message")) j["message"] = a["message"];
                    if (a.contains("enabled")) j["enabled"] = a["enabled"];
                }
            }
            out.push_back(j);
        }
        ctx.writeFile("cron.json", out);
        return {{"ok", true}};
    }
    return list(ctx);
}

void registerTools(Context& ctx) {
    ctx.add({"cronjob", "cron", "write",
        "Create, update, delete or list scheduled tasks (cron). Each job has a 6-field cron "
        "schedule (sec min hour day month weekday), a name and a message (run as an agent turn). "
        "action: create|update|delete|toggle|list. Needs user permission.",
        json::parse(R"JS({"type":"object","properties":{"action":{"type":"string","enum":["create","update","delete","toggle","list"]},"id":{"type":"number"},"name":{"type":"string"},"schedule":{"type":"string"},"message":{"type":"string"},"enabled":{"type":"boolean"}},"required":["action"]})JS"),
        [&ctx](const json& a, Call&) -> std::string { return act(ctx, a).dump(2); },
        [&ctx](const json& a) -> json { return {{"title","کرون"},{"detail",a.value("action","")}}; }
    });
}

} // namespace hek::cron
