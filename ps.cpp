// Hekpixle — process listing tool (ps).
// Mirrors lib/ps.js. Uses the Toolhelp32 snapshot.
#include "tools.hpp"
#include "util.hpp"
#include "context.hpp"
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#pragma comment(lib, "psapi.lib")
#include <sstream>

namespace hek::ps {

namespace {
struct Proc { DWORD pid; std::string name; std::string title; };
std::vector<Proc> snapshot() {
    std::vector<Proc> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            Proc p; p.pid = pe.th32ProcessID; p.name = narrow(pe.szExeFile);
            out.push_back(p);
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    // window titles for the foreground-relevant ones
    EnumWindows([](HWND hw, LPARAM lp)->BOOL{
        if (!IsWindowVisible(hw)) return TRUE;
        wchar_t title[256]; if (!GetWindowTextW(hw, title, 256)) return TRUE;
        DWORD pid; GetWindowThreadProcessId(hw, &pid);
        auto* vec = (std::vector<Proc>*)lp;
        for (auto& p : *vec) if (p.pid == pid && p.title.empty()) { p.title = narrow(title); break; }
        return TRUE;
    }, (LPARAM)&out);
    return out;
}
}

void registerTools(Context& ctx) {
    ctx.add({"ps", "terminal", "read",
        "List running processes with their PID, name and window title (if any). "
        "Use to find an app to control with computer(action=window) or to kill a hung process.",
        json::parse(R"JS({"type":"object","properties":{"filter":{"type":"string","description":"case-insensitive name substring"}},"required":[]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::string filt = toLower(a.value("filter",""));
            auto procs = snapshot();
            std::ostringstream o; int n = 0;
            for (auto& p : procs) {
                if (!filt.empty() && toLower(p.name).find(filt) == std::string::npos
                    && toLower(p.title).find(filt) == std::string::npos) continue;
                o << p.pid << "\t" << p.name;
                if (!p.title.empty()) o << "\t[" << p.title << "]";
                o << "\n";
                if (++n >= 200) { o << "…(truncated)\n"; break; }
            }
            return o.str().empty() ? "No matching processes." : o.str();
        }
    });
}

} // namespace hek::ps
