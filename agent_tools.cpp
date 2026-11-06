// Hekpixle — agent-level tools (todo, clarify, session_search, forget_memory,
// send_message, delegate_task). Mirrors lib/agent-tools.js. These tools
// coordinate planning and interact with the user / Telegram / sub-agents
// through the Call object.
#include "tools.hpp"
#include "util.hpp"
#include "context.hpp"
#include "agent.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>

namespace hek::agent_tools {

namespace {
std::string todoFile(Context& ctx) { return ctx.userDataDir() + "\\todo.json"; }

std::string fmtTodos(const json& t) {
    if (!t.is_array() || t.empty()) return "(empty)";
    std::ostringstream o;
    for (auto& x : t) {
        std::string s = x.value("status","pending");
        const char* mark = (s=="completed") ? "[x]" : (s=="in_progress") ? "[~]"
                          : (s=="cancelled") ? "[-]" : "[ ]";
        o << mark << " " << x.value("id","") << ". " << x.value("content","") << "\n";
    }
    return o.str();
}

std::string snippet(const std::string& text, const std::vector<std::string>& terms) {
    std::string low = toLower(text);
    size_t at = std::string::npos;
    for (auto& t : terms) { size_t i = low.find(toLower(t)); if (i != std::string::npos && (at==std::string::npos || i<at)) at = i; }
    size_t s = (at == std::string::npos) ? 0 : std::max((size_t)0, at - 80);
    return text.substr(s, 260);
}

std::string searchSessions(Context& ctx, const std::string& query, int limit) {
    std::vector<std::string> terms;
    for (auto& t : split(toLower(query), ' ')) if (t.size() > 1) terms.push_back(t);
    if (terms.empty()) return "ERROR: empty query";
    struct Hit { double score; std::string label, title, role; std::string text; };
    std::vector<Hit> hits;
    auto scan = [&](const std::string& file, const std::string& label) {
        json data = ctx.readFile(file, nullptr);
        if (!data.is_array()) return;
        for (auto& c : data) {
            json msgs = c.is_array() ? c : c.value("messages", json::array());
            if (!msgs.is_array()) continue;
            std::string title = c.value("title", "");
            for (auto& m : msgs) {
                std::string role = m.value("role","");
                if (role != "user" && role != "assistant") continue;
                std::string text;
                if (m["content"].is_string()) text = m["content"].get<std::string>();
                else if (m["content"].is_array())
                    for (auto& p : m["content"]) text += p.value("text","");
                if (text.empty()) continue;
                std::string low = toLower(text);
                double score = 0;
                for (auto& t : terms) if (low.find(t) != std::string::npos) score += 1;
                score /= terms.size();
                if (role == "user") score += 0.01;
                if (score > 0) hits.push_back({score, label, title, role, text});
            }
        }
    };
    scan("chats.json", "desktop chat");
    scan("tg_chats.json", "telegram");
    std::sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b){ return a.score > b.score; });
    if (hits.empty()) return "No matching past conversations.";
    std::ostringstream o;
    for (int i = 0; i < std::min((int)hits.size(), limit); ++i) {
        auto& h = hits[i];
        o << "• [" << h.label << (h.title.empty() ? "" : ": " + h.title) << "] "
          << h.role << ": …" << snippet(h.text, terms) << "…\n";
    }
    return o.str();
}
}

void registerTools(Context& ctx) {
    ctx.add({"todo", "planning", "read",
        "Plan and track multi-step work. Call with todos=[{id,content,status}] "
        "(status: pending|in_progress|completed|cancelled). merge=true updates/adds items by id; "
        "merge=false (default) replaces the list. Call with no args to read it. Use for tasks with 3+ steps; "
        "keep exactly one item in_progress.",
        json::parse(R"JS({"type":"object","properties":{"todos":{"type":"array","items":{"type":"object","properties":{"id":{"type":"string"},"content":{"type":"string"},"status":{"type":"string","enum":["pending","in_progress","completed","cancelled"]}},"required":["id","content","status"]}},"merge":{"type":"boolean"}},"required":[]})JS"),
        [&ctx](const json& a, Call& call) -> std::string {
            json list = ctx.readFile("todo.json", json::array());
            if (a.contains("todos") && a["todos"].is_array()) {
                if (a.value("merge", false)) {
                    for (auto& t : a["todos"]) {
                        std::string id = t.value("id","");
                        bool found = false;
                        for (auto& x : list) if (x.value("id","") == id) { x = t; found = true; break; }
                        if (!found) list.push_back(t);
                    }
                } else list = a["todos"];
                ctx.writeFile("todo.json", list);
                call.emit({{"type","todo"},{"todos",list}});
            }
            return fmtTodos(list);
        }
    });

    ctx.add({"clarify", "planning", "read",
        "Ask the user a clarifying question and wait for the answer. Optionally give up to 4 choices. "
        "Use only when you truly cannot proceed without their input.",
        json::parse(R"JS({"type":"object","properties":{"question":{"type":"string"},"choices":{"type":"array","items":{"type":"string"}}},"required":["question"]})JS"),
        [&ctx](const json& a, Call& call) -> std::string {
            if (call.source != "desktop") return "The user is not at the desktop right now — ask your question in your final reply instead and stop here.";
            std::vector<std::string> choices;
            if (a.contains("choices")) for (auto& c : a["choices"]) choices.push_back(c.get<std::string>());
            if (choices.size() > 4) choices.resize(4);
            std::string ans = call.askUser(a.value("question",""), choices);
            return ans.empty() ? "The user did not answer." : ("User answered: " + ans);
        }
    });

    ctx.add({"session_search", "memory", "read",
        "Search your past conversations (desktop + Telegram) for things the user said or you did before. "
        "Returns matching snippets. Use when the user refers to earlier work.",
        json::parse(R"JS({"type":"object","properties":{"query":{"type":"string"},"limit":{"type":"number"}},"required":["query"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            return searchSessions(ctx, a.value("query",""), std::max(1, std::min(15, a.value("limit",6))));
        }
    });

    ctx.add({"forget_memory", "memory", "write",
        "Delete a saved memory by its key.",
        json::parse(R"JS({"type":"object","properties":{"key":{"type":"string"}},"required":["key"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::string key = a.value("key","");
            json mem = ctx.readFile("memory.json", {{"entries", json::array()}});
            auto& entries = mem["entries"];
            int before = (int)entries.size();
            json kept = json::array();
            for (auto& e : entries) if (e.value("key","") != key) kept.push_back(e);
            mem["entries"] = kept;
            ctx.writeFile("memory.json", mem);
            return before == (int)kept.size() ? "No memory with key \"" + key + "\"." : "OK: forgot \"" + key + "\".";
        }
    });

    ctx.add({"send_message", "messaging", "write",
        "Send a text message to the user on Telegram (their home chat or a given chat_id). "
        "Use for notifying when a long task is done or delivering results while they are away.",
        json::parse(R"JS({"type":"object","properties":{"text":{"type":"string"},"chat_id":{"type":"string"}},"required":["text"]})JS"),
        [&ctx](const json& a, Call& call) -> std::string {
            return call.sendTelegram(a.value("text",""), a.value("chat_id",""));
        },
        [&ctx](const json& a) -> json { return {{"title","ارسال پیام تلگرام"},{"detail",a.value("text","").substr(0,300)}}; }
    });

    ctx.add({"delegate_task", "delegation", "control",
        "Spawn sub-agents with fresh context to work in parallel (max 3). Each gets a goal and context "
        "and returns a short summary — only the summary enters your context. Sub-agents cannot ask the "
        "user, delegate further or write memory.",
        json::parse(R"JS({"type":"object","properties":{"tasks":{"type":"array","items":{"type":"object","properties":{"goal":{"type":"string"},"context":{"type":"string"},"toolsets":{"type":"array","items":{"type":"string"}}},"required":["goal"]}},"goal":{"type":"string"},"context":{"type":"string"},"toolsets":{"type":"array","items":{"type":"string"}}},"required":[]})JS"),
        [&ctx](const json& a, Call& call) -> std::string {
            if (call.depth >= 1) return "ERROR: sub-agents cannot delegate further.";
            json tasks = json::array();
            if (a.contains("tasks") && a["tasks"].is_array()) tasks = a["tasks"];
            else if (a.contains("goal")) tasks.push_back({{"goal",a["goal"]},
                a.contains("context")?json{{"context",a["context"]}}:json::object(),
                a.contains("toolsets")?json{{"toolsets",a["toolsets"]}}:json::object()});
            if (tasks.empty()) return "ERROR: provide tasks or goal.";
            if (tasks.size() > 3) tasks = json(tasks.begin(), tasks.begin() + 3);
            std::ostringstream o;
            int i = 0;
            for (auto& t : tasks) {
                ++i;
                std::string goal = t.value("goal","");
                o << "### Sub-agent " << i << ": " << goal.substr(0,80) << "\n";
                try { o << call.runSubAgent(t, call); }
                catch (const std::exception& e) { o << "### Sub-agent " << i << " failed: " << e.what(); }
                o << "\n\n";
            }
            return o.str();
        }
    });
}

} // namespace hek::agent_tools
