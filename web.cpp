// Hekpixle — web tools (web_search, web_extract).
// Mirrors lib/web.js. Uses WinHTTP (util.hpp) instead of undici.
#include "tools.hpp"
#include "util.hpp"
#include "context.hpp"
#include <regex>

namespace hek::web {

// DuckDuckGo HTML endpoint — parses result links/snippets (same as JS).
static std::string duckSearch(const std::string& query, int limit, const std::string& proxy) {
    std::string q = query;
    // URL-encode
    std::string enc;
    enc.reserve(q.size() * 3);
    for (unsigned char c : q) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') enc += (char)c;
        else { char b[4]; sprintf(b, "%%%02X", c); enc += b; }
    }
    std::string url = "https://html.duckduckgo.com/html/?q=" + enc;
    auto r = httpFetch("GET", url, "", {{"User-Agent", "Mozilla/5.0"}}, proxy);
    if (!r.error.empty()) return std::string("ERROR: ") + r.error;
    std::string html = r.body;

    std::vector<std::string> out;
    // result anchors look like: <a class="result__a" href="...">Title</a>
    std::regex reA("<a[^>]*class=\"result__a\"[^>]*href=\"([^\"]+)\"[^>]*>(.*?)</a>", std::regex::icase);
    std::regex reS("<a[^>]*class=\"result__snippet\"[^>]*>(.*?)</a>", std::regex::icase);
    auto beginA = std::sregex_iterator(html.begin(), html.end(), reA);
    auto endA   = std::sregex_iterator();
    auto beginS = std::sregex_iterator(html.begin(), html.end(), reS);
    auto endS   = std::sregex_iterator();
    std::vector<std::string> snippets;
    for (auto it = beginS; it != endS; ++it) snippets.push_back(htmlToText((*it)[1].str()));
    int idx = 0;
    for (auto it = beginA; it != endA && (int)out.size() < limit; ++it, ++idx) {
        std::string rawUrl = (*it)[1].str();
        // DDG wraps URLs in /l/?uddg=...
        std::smatch m;
        if (std::regex_search(rawUrl, m, std::regex("uddg=([^&]+)"))) {
            std::string u = m[1].str();
            // simple percent-decode
            std::string dec; dec.reserve(u.size());
            for (size_t i = 0; i < u.size(); ++i) {
                if (u[i] == '%' && i + 2 < u.size()) {
                    int hi = (u[i+1] >= 'A') ? (u[i+1]-'A'+10) : (u[i+1]-'0');
                    int lo = (u[i+2] >= 'A') ? (u[i+2]-'A'+10) : (u[i+2]-'0');
                    dec += (char)((hi<<4)|lo); i += 2;
                } else dec += u[i];
            }
            rawUrl = dec;
        }
        std::string title = htmlToText((*it)[2].str());
        std::string snip = (size_t)idx < snippets.size() ? snippets[idx] : "";
        out.push_back("• " + title + "\n  " + rawUrl + (snip.empty() ? "" : "\n  " + snip));
    }
    if (out.empty()) return "No results.";
    std::string joined = "Top results:\n\n";
    for (auto& s : out) joined += s + "\n\n";
    return joined;
}

static std::string extractUrl(const std::string& url, int maxChars, const std::string& proxy) {
    auto r = httpFetch("GET", url, "", {{"User-Agent", "Mozilla/5.0"}}, proxy);
    if (!r.error.empty()) return std::string("ERROR: ") + r.error + " (" + url + ")";
    std::string text = htmlToText(r.body);
    if ((int)text.size() > maxChars) text = text.substr(0, maxChars) + "\n…(truncated)";
    return "=== " + url + " ===\n" + text;
}

void registerTools(Context& ctx) {
    ctx.add({
        "web_search", "web", "read",
        "Search the web. Returns titles, URLs and snippets. Follow up with web_extract "
        "(or browser_navigate) to read pages. Use for current events, documentation, "
        "facts you are unsure about.",
        {{"type","object"},{"properties",{
            {"query",{{"type","string"}}},
            {"limit",{{"type","number"},{"description","max results (default 8)"}}}
        }},{"required",{"query"}}},
        [&ctx](const json& a, Call&) -> std::string {
            auto s = ctx.loadSettings();
            std::string proxy = s["web"].value("proxyUrl", "");
            return duckSearch(a.value("query",""), a.value("limit",8), proxy);
        }
    });

    ctx.add({
        "web_extract", "web", "read",
        "Fetch one or more web pages (up to 5) and return their readable text. "
        "Fast and cheap; for JS-heavy or login-protected pages use browser_navigate.",
        {{"type","object"},{"properties",{
            {"urls",{{"type","array"},{"items",{{"type","string"}}}}},
            {"url",{{"type","string"}}},
            {"max_chars",{{"type","number"},{"description","per page, default 15000, max 60000"}}}
        }},{"required",{}}},
        [&ctx](const json& a, Call& call) -> std::string {
            auto s = ctx.loadSettings();
            std::string proxy = s["web"].value("proxyUrl", "");
            int maxChars = std::min(60000, a.value("max_chars", 15000));
            std::vector<std::string> urls;
            if (a.contains("urls") && a["urls"].is_array())
                for (auto& u : a["urls"]) urls.push_back(u.get<std::string>());
            if (a.contains("url") && a["url"].is_string()) urls.push_back(a["url"].get<std::string>());
            if (urls.size() > 5) urls.resize(5);
            if (urls.empty()) return "ERROR: provide url or urls";
            std::string out;
            for (auto& u : urls) { if (call.stop) break; out += extractUrl(u, maxChars, proxy) + "\n\n"; }
            return out;
        }
    });
}

} // namespace hek::web
