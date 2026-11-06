// Hekpixle — agent loop.
//
// Mirrors runAgent() in main.js:
//   1. Build the system prompt (persona + tool hints).
//   2. Loop: send messages+tools to the LLM, parse the assistant reply.
//      - If there are tool_calls: ask permission for write/control tools,
//        run each tool, append the result as a "tool" message, repeat.
//      - Else: stream the text to the UI and return it.
//   3. Stop when: no more tool_calls, maxSteps reached, or call.stop is set.
#include "agent.hpp"
#include "util.hpp"
#include <chrono>
#include <ctime>
#include <sstream>

namespace hek {

namespace {
// Convert our Tool registry into the OpenAI tools JSON schema.
json toolsForLLM(Context& ctx, const json& settings) {
    json enabled = settings.value("toolsets", json::object());
    json out = json::array();
    for (auto& t : ctx.tools()) {
        bool on = !enabled.contains(t.toolset) || enabled[t.toolset].get<bool>() != false;
        if (!on) continue;
        json schema = t.parameters.is_null()
            ? json{{"type", "object"}, {"properties", json::object()}}
            : t.parameters;
        out.push_back({
            {"type", "function"},
            {"function", {
                {"name", t.name},
                {"description", t.description},
                {"parameters", schema},
            }},
        });
    }
    return out;
}

std::string buildSystemPrompt(Context& ctx, const json& settings) {
    std::ostringstream s;
    s << "You are Hekpixle, a personal AI agent living on the user's Windows desktop. "
      << "You can use tools to browse the web, read/write files, control the mouse and "
      << "keyboard, run terminal commands, automate a browser, run code, and more. "
      << "Always prefer one focused tool call per step. Be concise. "
      << "When you have enough information, answer the user directly without calling more tools.\n";
    if (settings.contains("personality") && !settings["personality"].get<std::string>().empty()) {
        s << "\nAdditional persona instructions:\n" << settings["personality"].get<std::string>() << "\n";
    }
    s << "\nCurrent time: ";
    {
        auto now = std::chrono::system_clock::now();
        std::time_t t = std::chrono::system_clock::to_time_t(now);
        char buf[64]; std::tm lt; localtime_s(&lt, &t);
        std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &lt);
        s << buf;
    }
    s << ".";
    return s.str();
}
} // namespace

std::string runAgent(Context& ctx, const json& messagesIn, Call& call) {
    json settings = ctx.loadSettings();
    std::string baseUrl = settings.value("baseUrl", "https://api.openai.com/v1");
    std::string apiKey  = settings.value("apiKey", "");
    std::string model   = settings.value("model", "gpt-4o-mini");
    std::string proxy   = settings.value("llmProxyUrl", "");
    int maxSteps = settings.value("maxSteps", 30);

    if (apiKey.empty()) {
        ctx.emit({{"type", "error"}, {"message", "API Key تنظیم نشده. در تنظیمات وارد کنید."}});
        return "خطا: API Key تنظیم نشده.";
    }

    json tools = toolsForLLM(ctx, settings);

    // Working message list = system prompt + the chat history.
    json messages = json::array();
    messages.push_back({{"role", "system"}, {"content", buildSystemPrompt(ctx, settings)}});
    for (auto& m : messagesIn) messages.push_back(m);

    std::string finalText;

    for (int step = 0; step < maxSteps; ++step) {
        if (call.stop) { ctx.emit({{"type", "aborted"}}); break; }

        json resp = llmChat(baseUrl, apiKey, model, messages, tools, proxy, &call.stop);
        if (resp.contains("_error")) {
            // Try fallback provider before giving up.
            std::string fbUrl = settings.value("fallbackBaseUrl", "");
            std::string fbKey = settings.value("fallbackApiKey", "");
            std::string fbMod = settings.value("fallbackModel", "");
            if (!fbUrl.empty() && !fbKey.empty()) {
                resp = llmChat(fbUrl, fbKey, fbMod.empty() ? model : fbMod,
                               messages, tools, proxy, &call.stop);
            }
            if (resp.contains("_error")) {
                ctx.emit({{"type", "error"}, {"message", resp["_error"].get<std::string>()}});
                return std::string("خطا: ") + resp["_error"].get<std::string>();
            }
        }

        json choice = resp["choices"][0]["message"];
        messages.push_back(choice);  // assistant turn

        // Stream text delta to the UI.
        if (choice.contains("content") && choice["content"].is_string()) {
            std::string txt = choice["content"].get<std::string>();
            if (!txt.empty()) {
                finalText = txt;
                call.emit({{"type", "delta"}, {"text", txt}});
            }
        }

        // No tool calls -> done.
        if (!choice.contains("tool_calls") || choice["tool_calls"].empty()) {
            call.emit({{"type", "done"}});
            return finalText;
        }

        // Run each tool call.
        for (auto& tc : choice["tool_calls"]) {
            if (call.stop) break;
            std::string name = tc["function"]["name"];
            json args;
            try { args = json::parse(tc["function"].value("arguments", std::string("{}"))); }
            catch (...) { args = json::object(); }
            std::string id = tc.value("id", "");

            call.emit({{"type", "tool"}, {"phase", "start"}, {"name", name}, {"args", args}});

            const Tool* tool = ctx.find(name);
            if (!tool) {
                std::string err = "ERROR: unknown tool " + name;
                messages.push_back({{"role", "tool"}, {"tool_call_id", id}, {"content", err}});
                call.emit({{"type", "tool"}, {"phase", "end"}, {"name", name}, {"result", err}, {"error", true}});
                continue;
            }

            // Permission gate for write/control tools (mirrors approvalMode).
            std::string category = tool->category;
            bool needsApproval = (category == "write" || category == "control");
            json alwaysAllow = settings.value("alwaysAllow", json::array());
            if (alwaysAllow.is_array()) {
                for (auto& a : alwaysAllow) if (a.get<std::string>() == name) needsApproval = false;
            }
            if (settings.value("approvalMode", "manual") == "off") needsApproval = false;

            std::string decision = "allow";
            if (needsApproval) {
                std::string title = "اجازه‌ی ابزار: " + name;
                std::string detail;
                if (tool->confirm) {
                    json c = tool->confirm(args);
                    title  = c.value("title", title);
                    detail = c.value("detail", "");
                } else {
                    detail = args.dump();
                }
                decision = ctx.askPermission(title, detail);
                if (decision == "always") {
                    // remember for the rest of this run
                    alwaysAllow.push_back(name);
                    settings["alwaysAllow"] = alwaysAllow;
                    decision = "allow";
                }
            }
            if (call.stop || decision == "deny") {
                std::string err = "denied by user";
                messages.push_back({{"role", "tool"}, {"tool_call_id", id}, {"content", err}});
                call.emit({{"type", "tool"}, {"phase", "end"}, {"name", name}, {"result", err}, {"error", true}});
                if (call.stop) break;
                continue;
            }

            std::string result;
            try {
                result = tool->run(args, call);
            } catch (const std::exception& e) {
                result = std::string("ERROR: ") + e.what();
            }
            // Truncate huge results before feeding back to the LLM.
            if (result.size() > 20000) result = result.substr(0, 20000) + "\n... (truncated)";

            messages.push_back({{"role", "tool"}, {"tool_call_id", id}, {"content", result}});
            call.emit({{"type", "tool"}, {"phase", "end"}, {"name", name}, {"result", result}});
        }
    }

    if (!call.stop) ctx.emit({{"type", "done"}});
    return finalText;
}

json compressChat(Context& ctx, const json& messagesIn) {
    json settings = ctx.loadSettings();
    std::string baseUrl = settings.value("baseUrl", "https://api.openai.com/v1");
    std::string apiKey  = settings.value("apiKey", "");
    std::string model   = settings.value("model", "gpt-4o-mini");
    std::string proxy   = settings.value("llmProxyUrl", "");
    if (apiKey.empty()) return {{"_error", "API Key تنظیم نشده."}};

    json msgs = json::array();
    msgs.push_back({{"role", "system"}, {"content",
        "You compress chat histories. Summarize the conversation into a short "
        "assistant message that preserves all facts, decisions and code. "
        "Output ONLY the summary message as {\"role\":\"assistant\",\"content\":\"...\"}."}});
    json conv = json::array();
    for (auto& m : messagesIn) conv.push_back(m);
    msgs.push_back({{"role", "user"}, {"content", conv.dump()}});

    auto resp = llmChat(baseUrl, apiKey, model, msgs, nullptr, proxy);
    if (resp.contains("_error")) return {{"_error", resp["_error"].get<std::string>()}};
    try {
        std::string summary = resp["choices"][0]["message"]["content"].get<std::string>();
        json out = json::array();
        out.push_back({{"role", "assistant"}, {"content", summary}});
        if (out.size() >= messagesIn.size())
            return {{"_error", "گفت‌وگو برای فشرده‌سازی خیلی کوتاهه."}};
        return {{"messages", out}};
    } catch (const std::exception& e) {
        return {{"_error", e.what()}};
    }
}

} // namespace hek
