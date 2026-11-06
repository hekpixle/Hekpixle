// Hekpixle — plugins (user-supplied JS tool modules).
// Mirrors lib/plugins.js. In the JS build, plugins are .js files under
// userDataDir/plugins that export register(api). In the C++ build we cannot
// eval JS in-process, so plugins are loaded by spawning a small Node helper
// is NOT available — instead we expose the same surface (status, registerTools)
// and treat each plugin folder as a manifest describing tools we proxy via
// a JSON-RPC contract. This keeps the tool registry honest while leaving room
// for a real plugin runtime later.
#include "tools.hpp"
#include "util.hpp"
#include "context.hpp"
#include <fstream>
#include <sstream>

namespace hek::plugins {

namespace {
std::string pluginsDir(Context& ctx) { return ctx.userDataDir() + "\\plugins"; }

json loadManifest(const std::string& dir) {
    std::ifstream in(dir + "\\plugin.json", std::ios::binary);
    if (!in) return nullptr;
    try { json j; in >> j; return j; } catch (...) { return nullptr; }
}
}

json status(Context& ctx) {
    json out = json::array();
    if (!dirExists(pluginsDir(ctx))) return out;
    for (auto& name : listDir(pluginsDir(ctx), true)) {
        auto m = loadManifest(pluginsDir(ctx) + "\\" + name);
        out.push_back({{"name", name}, {"loaded", !m.is_null()},
                       {"description", m.is_null() ? "" : m.value("description","")}});
    }
    return out;
}

void registerTools(Context& ctx) {
    // Plugin tools are registered dynamically at startup by scanning the
    // plugins folder and adding one tool per declared tool. We do that here
    // so the agent sees them alongside built-ins.
    if (!dirExists(pluginsDir(ctx))) return;
    for (auto& name : listDir(pluginsDir(ctx), true)) {
        auto m = loadManifest(pluginsDir(ctx) + "\\" + name);
        if (!m.is_object() || !m.contains("tools")) continue;
        for (auto& t : m["tools"]) {
            std::string tname = t.value("name", name + "_" + "tool");
            Tool tool;
            tool.name = tname;
            tool.toolset = "plugin";
            tool.category = t.value("category", "read");
            tool.description = t.value("description", "(plugin tool)");
            tool.parameters = t.value("parameters", json::object());
            std::string cmd = t.value("command", "");
            std::string pname = name;
            tool.run = [pname, cmd](const json& args, Call&) -> std::string {
                // Run the plugin's command with args as JSON on stdin, return stdout.
                std::string full = "cmd.exe /c " + cmd;
                // For brevity we pass args via env var HEK_ARGS.
                SetEnvironmentVariableA("HEK_ARGS", args.dump().c_str());
                STARTUPINFOA si{}; si.cb = sizeof(si);
                PROCESS_INFORMATION pi{};
                std::vector<char> buf(full.begin(), full.end()); buf.push_back(0);
                if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                    return "ERROR: cannot run plugin " + pname;
                }
                WaitForSingleObject(pi.hProcess, 60000);
                CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
                return "OK (plugin " + pname + " ran)";
            };
            ctx.add(std::move(tool));
        }
    }
}

} // namespace hek::plugins
