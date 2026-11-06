// Hekpixle — MCP (Model Context Protocol) tools.
// Mirrors lib/mcp.js. The full MCP spec is large; here we implement the
// common path: connect to a configured MCP server (stdio command or HTTP URL),
// list its tools, and call them. MCP servers speak JSON-RPC over stdio or HTTP.
#include "tools.hpp"
#include "util.hpp"
#include "context.hpp"
#include <windows.h>
#include <sstream>
#include <map>
#include <mutex>

namespace hek::mcp {

namespace {
struct Server {
    HANDLE hProcess = nullptr;
    HANDLE hIn = nullptr;   // write to child stdin
    HANDLE hOut = nullptr;  // read child stdout
    int nextId = 1;
};
std::map<std::string, Server> g_servers;
std::mutex g_mu;

bool startStdio(const std::string& name, const std::string& command) {
    SECURITY_ATTRIBUTES sa{}; sa.nInheritHandle = TRUE;
    HANDLE cInR, cInW, cOutR, cOutW;
    CreatePipe(&cInR, &cInW, &sa, 0);
    CreatePipe(&cOutR, &cOutW, &sa, 0);
    SetHandleInformation(cInW, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(cOutR, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{}; si.cb = sizeof(si);
    si.hStdInput = cInR; si.hStdOutput = cOutW; si.hStdError = cOutW;
    si.dwFlags = STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi{};
    std::string full = "cmd.exe /c " + command;
    std::vector<char> buf(full.begin(), full.end()); buf.push_back(0);
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(cInR); CloseHandle(cInW); CloseHandle(cOutR); CloseHandle(cOutW);
        return false;
    }
    CloseHandle(cInR); CloseHandle(cOutW); CloseHandle(pi.hThread);
    Server s; s.hProcess = pi.hProcess; s.hIn = cInW; s.hOut = cOutR;
    g_servers[name] = s;
    return true;
}

json rpc(Server& s, const std::string& method, const json& params) {
    json req = {{"jsonrpc","2.0"},{"id",s.nextId++},{"method",method}};
    if (!params.is_null()) req["params"] = params;
    std::string body = req.dump() + "\n";
    DWORD w; WriteFile(s.hIn, body.data(), (DWORD)body.size(), &w, nullptr);
    // read one line of response
    std::string resp; char c;
    DWORD r;
    while (ReadFile(s.hOut, &c, 1, &r, nullptr) && r) {
        if (c == '\n') break;
        resp += c;
        if (resp.size() > 1'000'000) break;
    }
    try { return json::parse(resp); } catch (...) { return {{"error","bad response"}}; }
}
}

json status() {
    std::lock_guard<std::mutex> lk(g_mu);
    json out = json::array();
    for (auto& kv : g_servers) {
        DWORD code = 0; GetExitCodeProcess(kv.second.hProcess, &code);
        out.push_back(json{{"name", kv.first}, {"running", code == STILL_ACTIVE}});
    }
    return out;
}

void registerTools(Context& ctx) {
    ctx.add({"mcp_call", "mcp", "control",
        "Call a tool exposed by a configured MCP server. The server must be listed in "
        "settings.mcpServers (stdio command or HTTP URL). Needs user permission.",
        json::parse(R"JS({"type":"object","properties":{"server":{"type":"string"},"tool":{"type":"string"},"args":{"type":"object"}},"required":["server","tool"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            auto s = ctx.loadSettings();
            auto servers = s.value("mcpServers", json::object());
            std::string name = a.value("server","");
            if (!servers.contains(name)) return "ERROR: unknown MCP server: " + name;
            std::lock_guard<std::mutex> lk(g_mu);
            auto it = g_servers.find(name);
            if (it == g_servers.end()) {
                auto cfg = servers[name];
                if (cfg.contains("command")) {
                    std::string cmd = cfg["command"].get<std::string>();
                    if (cfg.contains("args") && cfg["args"].is_array())
                        for (auto& x : cfg["args"]) cmd += " " + x.get<std::string>();
                    if (!startStdio(name, cmd)) return "ERROR: cannot start MCP server";
                    it = g_servers.find(name);
                    rpc(it->second, "initialize", {{"protocolVersion","2024-11-05"},{"capabilities",json::object()},{"clientInfo",{{"name","hekpixle"},{"version","0.13.0"}}}});
                } else {
                    return "ERROR: HTTP MCP servers not yet supported in C++ build";
                }
            }
            json r = rpc(it->second, "tools/call", {{"name", a.value("tool","")}, {"arguments", a.value("args", json::object())}});
            return r.dump();
        },
        [&ctx](const json& a) -> json { return {{"title","MCP"},{"detail",a.value("server","")+"."+a.value("tool","")}}; }
    });
}

} // namespace hek::mcp
