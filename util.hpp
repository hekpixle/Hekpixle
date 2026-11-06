// Hekpixle — network utilities (WinHTTP) + path helpers.
//
// Replaces lib/util.js (httpFetch, dispatcherFor) and the undici dependency.
// All HTTP goes through WinHTTP so we have zero Node.js / undici dependency.
#pragma once
#include "json.hpp"
#include <string>
#include <vector>

namespace hek {

struct HttpResponse {
    int status = 0;
    std::string body;
    std::map<std::string, std::string> headers;
    std::string error;   // empty on success
};

// Generic HTTPS/HTTP request. Mirrors undici fetch() used in the JS code.
//   method:  "GET" | "POST" | ...
//   url:     full URL including scheme
//   body:    request body (raw bytes; empty for GET)
//   headers: name->value
//   proxyUrl: optional "http://host:port" (LLM proxy / Telegram proxy)
HttpResponse httpFetch(const std::string& method,
                       const std::string& url,
                       const std::string& body,
                       const std::vector<std::pair<std::string, std::string>>& headers = {},
                       const std::string& proxyUrl = "");

// Run an OpenAI-compatible chat completion. Returns the parsed JSON response
// (choices[0].message). On HTTP/parse error, returns json with {"_error": "..."}.
json llmChat(const std::string& baseUrl,
             const std::string& apiKey,
             const std::string& model,
             const json& messages,        // [{role, content}]
             const json& tools,           // JSON-schema tool list (may be null)
             const std::string& proxyUrl,
             std::atomic<bool>* cancel = nullptr);

// Path helpers (mirror lib/util.js).
std::string exeDir();                 // directory of Hekpixle.exe
std::string userDataDir(const std::string& profile = "");
std::string appDataDir();             // %APPDATA%
bool dirExists(const std::string& p);
bool ensureDir(const std::string& p);
std::vector<std::string> listDir(const std::string& p, bool dirsOnly = false);

// Small text helpers.
std::string trim(const std::string& s);
std::string toLower(const std::string& s);
bool startsWith(const std::string& s, const std::string& p);
std::vector<std::string> split(const std::string& s, char delim);
std::string htmlToText(const std::string& html);

} // namespace hek
