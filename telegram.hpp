// Hekpixle — Telegram bot.
//
// Mirrors telegram.js: long-polls the Telegram Bot API (over an optional
// HTTP/SOCKS proxy), routes incoming messages to the agent, and exposes
// sendMessage() for the agent's send_message tool. Status changes are
// streamed to the UI via ctx.emit() as {"type":"telegram:status", ...}.
#pragma once
#include "json.hpp"
#include <atomic>
#include <string>

namespace hek {

class TelegramBot {
public:
    // Start polling with the given settings.telegram object.
    void start(const json& telegramSettings);
    void stop();
    json status() const;
    // Test a token (and optional proxy) without keeping the bot running.
    json test(const std::string& token, const std::string& proxyUrl);
    // Send a text message to a chat (chat_id empty => the configured home chat).
    std::string sendMessage(const std::string& text, const std::string& chatId);

private:
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::thread worker_;
    std::string token_;
    std::string proxyUrl_;
    std::string botName_;
    int homeChatId_ = 0;
    int lastUpdateId_ = 0;

    void loop();
    json api(const std::string& method, const json& body);
};

} // namespace hek
