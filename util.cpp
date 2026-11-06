// Hekpixle — util.cpp: WinHTTP fetch, LLM chat, paths, text helpers.
//
// Replaces lib/util.js + the undici dependency. All HTTP/HTTPS goes through
// WinHTTP, which is part of Windows (no extra runtime needed).
#include "util.hpp"
#include "json.hpp"

#include <windows.h>
#include <winhttp.h>
#include <shlobj.h>
#include <shlwapi.h>
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shlwapi.lib")

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

namespace hek {

// ---------- string helpers ----------
std::string trim(const std::string& s) {
    auto b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    auto e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}
std::string toLower(const std::string& s) {
    std::string o = s; std::transform(o.begin(), o.end(), o.begin(),
                                      [](unsigned char c){ return (char)std::tolower(c); });
    return o;
}
bool startsWith(const std::string& s, const std::string& p) {
    return s.rfind(p, 0) == 0;
}
std::vector<std::string> split(const std::string& s, char delim) {
    std::vector<std::string> out; std::string cur;
    for (char c : s) { if (c == delim) { out.push_back(cur); cur.clear(); } else cur += c; }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
std::string htmlToText(const std::string& html) {
    std::string out; out.reserve(html.size());
    bool inTag = false;
    for (size_t i = 0; i < html.size(); ++i) {
        char c = html[i];
        if (c == '<') { inTag = true; continue; }
        if (c == '>') { inTag = false; continue; }
        if (!inTag) out += c;
    }
    // collapse whitespace
    std::string o; o.reserve(out.size());
    bool ws = false;
    for (char c : out) {
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            if (!ws) { o += ' '; ws = true; }
        } else { o += c; ws = false; }
    }
    return o;
}

// ---------- paths ----------
std::string appDataDir() {
    PWSTR wpath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &wpath))) {
        std::string s = narrow(wpath);
        CoTaskMemFree(wpath);
        return s;
    }
    return "C:\\Users\\Public\\AppData\\Roaming";
}

std::string exeDir() {
    char buf[MAX_PATH];
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    std::string p = buf;
    size_t s = p.find_last_of("\\/");
    return (s == std::string::npos) ? "." : p.substr(0, s);
}

std::string userDataDir(const std::string& profile) {
    std::string base = appDataDir() + "\\Hekpixle";
    if (!profile.empty()) base += "-" + profile;
    ensureDir(base);
    return base;
}

bool dirExists(const std::string& p) {
    DWORD a = GetFileAttributesA(p.c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}
bool ensureDir(const std::string& p) {
    if (p.empty() || dirExists(p)) return true;
    // create parents
    size_t pos = p.find_last_of("\\/");
    if (pos != std::string::npos) ensureDir(p.substr(0, pos));
    return CreateDirectoryA(p.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}
std::vector<std::string> listDir(const std::string& p, bool dirsOnly) {
    std::vector<std::string> out;
    std::string glob = p + "\\*";
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(glob.c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string n = fd.cFileName;
        if (n == "." || n == "..") continue;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (dirsOnly && !isDir) continue;
        out.push_back(n);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return out;
}

// ---------- WinHTTP fetch ----------
namespace {
struct Url {
    std::wstring scheme, host, path;
    int port = 0;
    bool https = false;
};
bool parseUrl(const std::string& url, Url& out) {
    // scheme://host[:port]/path
    size_t pos = url.find("://");
    if (pos == std::string::npos) return false;
    std::string scheme = toLower(url.substr(0, pos));
    out.https = (scheme == "https");
    size_t hostStart = pos + 3;
    size_t pathStart = url.find('/', hostStart);
    std::string hostport = (pathStart == std::string::npos)
        ? url.substr(hostStart) : url.substr(hostStart, pathStart - hostStart);
    out.path = widen(pathStart == std::string::npos ? "/" : url.substr(pathStart));
    size_t colon = hostport.find(':');
    if (colon != std::string::npos) {
        out.host = widen(hostport.substr(0, colon));
        out.port = std::atoi(hostport.c_str() + colon + 1);
    } else {
        out.host = widen(hostport);
        out.port = out.https ? 443 : 80;
    }
    out.scheme = widen(scheme);
    return !out.host.empty();
}
}

HttpResponse httpFetch(const std::string& method,
                       const std::string& url,
                       const std::string& body,
                       const std::vector<std::pair<std::string, std::string>>& headers,
                       const std::string& proxyUrl) {
    HttpResponse res;
    Url u;
    if (!parseUrl(url, u)) { res.error = "bad url"; return res; }

    HINTERNET session = WinHttpOpen(L"Hekpixle/0.13",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { res.error = "WinHttpOpen failed"; return res; }

    // Optional proxy (LLM proxy / Telegram proxy).
    if (!proxyUrl.empty()) {
        Url pu;
        if (parseUrl(proxyUrl, pu)) {
            WINHTTP_PROXY_INFO pi{};
            std::wstring proxyStr = pu.scheme + L"://" + pu.host + L":" + std::to_wstring(pu.port);
            pi.dwAccessType = WINHTTP_ACCESS_TYPE_NAMED_PROXY;
            pi.lpszProxy = (LPWSTR)proxyStr.c_str();
            pi.lpszProxyBypass = nullptr;
            WinHttpSetOption(session, WINHTTP_OPTION_PROXY, &pi, sizeof(pi));
        }
    }

    HINTERNET conn = WinHttpConnect(session, u.host.c_str(),
        (u.https ? INTERNET_SCHEME_HTTPS : INTERNET_SCHEME_HTTP), u.port);
    if (!conn) { res.error = "WinHttpConnect failed"; WinHttpCloseHandle(session); return res; }

    DWORD flags = (u.https ? WINHTTP_FLAG_SECURE : 0);
    HINTERNET req = WinHttpOpenRequest(conn, widen(method).c_str(), u.path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!req) { res.error = "WinHttpOpenRequest failed"; WinHttpCloseHandle(conn); WinHttpCloseHandle(session); return res; }

    for (auto& h : headers) {
        std::wstring hdr = widen(h.first + ": " + h.second);
        WinHttpAddRequestHeaders(req, hdr.c_str(), (DWORD)-1, WINHTTP_ADDREQ_FLAG_ADD);
    }

    const void* bodyPtr = body.empty() ? WINHTTP_NO_REQUEST_DATA : body.data();
    DWORD bodyLen = (DWORD)body.size();
    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
            const_cast<void*>(bodyPtr), bodyLen, bodyLen, 0)) {
        res.error = "WinHttpSendRequest failed";
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
        return res;
    }

    if (!WinHttpReceiveResponse(req, nullptr)) {
        res.error = "WinHttpReceiveResponse failed";
        WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
        return res;
    }

    DWORD statusCode = 0, sz = sizeof(statusCode);
    WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &sz, WINHTTP_NO_HEADER_INDEX);
    res.status = (int)statusCode;

    DWORD avail = 0;
    std::string buf;
    while (WinHttpQueryDataAvailable(req, &avail) && avail > 0) {
        std::string chunk(avail, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(req, &chunk[0], avail, &read) || read == 0) break;
        buf.append(chunk.data(), read);
    }
    res.body = std::move(buf);

    WinHttpCloseHandle(req); WinHttpCloseHandle(conn); WinHttpCloseHandle(session);
    return res;
}

// ---------- LLM chat (OpenAI-compatible) ----------
json llmChat(const std::string& baseUrl,
             const std::string& apiKey,
             const std::string& model,
             const json& messages,
             const json& tools,
             const std::string& proxyUrl,
             std::atomic<bool>* cancel) {
    (void)cancel; // best-effort: we don't interrupt an in-flight WinHTTP call

    std::string url = baseUrl;
    while (!url.empty() && url.back() == '/') url.pop_back();
    url += "/chat/completions";

    json body = {
        {"model", model},
        {"messages", messages},
        {"stream", false},
    };
    if (tools.is_array() && !tools.empty()) {
        body["tools"] = tools;
        body["tool_choice"] = "auto";
    }

    auto resp = httpFetch("POST", url, body.dump(),
        {
            {"Content-Type", "application/json"},
            {"Authorization", "Bearer " + apiKey},
        }, proxyUrl);

    if (!resp.error.empty()) return {{"_error", "network: " + resp.error}};
    try {
        json j = json::parse(resp.body);
        if (resp.status >= 400) {
            std::string msg = "HTTP " + std::to_string(resp.status);
            if (j.contains("error") && j["error"].contains("message"))
                msg += ": " + j["error"]["message"].get<std::string>();
            return {{"_error", msg}};
        }
        return j;
    } catch (const std::exception& e) {
        return {{"_error", std::string("parse: ") + e.what()}};
    }
}

} // namespace hek
