// Hekpixle — Telegram bot.
// Mirrors telegram.js: long-polls the Telegram Bot API (over an optional
// HTTP/SOCKS proxy), routes incoming messages to the agent, and exposes
// sendMessage() for the agent's send_message tool.
#include "telegram.hpp"
#include "util.hpp"
#include "context.hpp"
#include "agent.hpp"
#include <windows.h>
#include <sstream>
#include <chrono>
#include <thread>

namespace hek {

void TelegramBot::start(const json& s) {
    if (running_) return;
    token_ = s.value("botToken", "");
    proxyUrl_ = s.value("proxyUrl", "");
    if (token_.empty()) return;
    running_ = true;
    worker_ = std::thread([this]{ loop(); });
    worker_.detach();
}

void TelegramBot::stop() {
    running_ = false;
    connected_ = false;
}

json TelegramBot::status() const {
    return {
        {"running", (bool)running_},
        {"connected", (bool)connected_},
        {"botName", botName_},
        {"homeChatId", homeChatId_},
    };
}

json TelegramBot::test(const std::string& token, const std::string& proxyUrl) {
    auto r = httpFetch("GET", "https://api.telegram.org/bot" + token + "/getMe", "",
                       {{"Content-Type","application/json"}}, proxyUrl);
    if (!r.error.empty()) return {{"ok", false}, {"error", r.error}};
    try {
        json j = json::parse(r.body);
        return j;
    } catch (const std::exception& e) {
        return {{"ok", false}, {"error", e.what()}};
    }
}

std::string TelegramBot::sendMessage(const std::string& text, const std::string& chatId) {
    if (token_.empty()) return "ERROR: Telegram bot not started";
    int chat = chatId.empty() ? homeChatId_ : std::atoi(chatId.c_str());
    if (chat == 0) return "ERROR: no chat id (set /sethome in Telegram first)";
    json body = {{"chat_id", chat}, {"text", text}};
    auto r = httpFetch("POST", "https://api.telegram.org/bot" + token_ + "/sendMessage",
                       body.dump(), {{"Content-Type","application/json"}}, proxyUrl_);
    if (!r.error.empty()) return std::string("ERROR: ") + r.error;
    return "OK";
}

json TelegramBot::api(const std::string& method, const json& body) {
    auto r = httpFetch("POST", "https://api.telegram.org/bot" + token_ + "/" + method,
                       body.dump(), {{"Content-Type","application/json"}}, proxyUrl_);
    if (!r.error.empty()) return {{"ok", false}, {"error", r.error}};
    try { return json::parse(r.body); } catch (...) { return {{"ok", false}, {"error","bad response"}}; }
}

void TelegramBot::loop() {
    // getMe to record the bot name + confirm the token works.
    auto me = api("getMe", json::object());
    if (me.value("ok", false)) {
        botName_ = me["result"].value("username", "");
        connected_ = true;
    } else {
        running_ = false;
        return;
    }

    while (running_) {
        json body = {{"timeout", 25}, {"offset", lastUpdateId_ + 1}};
        auto r = api("getUpdates", body);
        if (!r.value("ok", false)) {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }
        for (auto& u : r["result"]) {
            lastUpdateId_ = u.value("update_id", lastUpdateId_);
            if (!u.contains("message")) continue;
            auto& msg = u["message"];
            int chatId = msg.value("chat", json::object()).value("id", 0);
            std::string text = msg.value("text", "");
            std::string from = msg.value("from", json::object()).value("first_name", "user");

            // /sethome records this chat as the agent's home chat.
            if (text == "/sethome") { homeChatId_ = chatId; continue; }
            if (text == "/start") {
                sendMessage("سلام! من ایجنت Hekpixle هستم. بفرست تا کارت رو انجام بدم.", std::to_string(chatId));
                continue;
            }
            if (text.empty()) continue;

            // Run an agent turn with this message.
            // (We don't have a Context pointer here in this minimal wiring;
            // a production build would inject it. For now we echo so the
            // surface is honest.)
            sendMessage("[" + from + "] " + text + "\n(پاسخ ایجنت: این یک پایه‌ی تلگرام است؛ برای فعال‌سازی کامل، Context را به TelegramBot تزریق کنید.)",
                        std::to_string(chatId));
        }
    }
    connected_ = false;
}

} // namespace hek
