// Hekpixle — terminal tools (run_command, process, execute_code).
// Mirrors lib/procs.js. Uses CreateProcess with pipes for foreground commands
// and a registry of background processes for background=true.
#include "tools.hpp"
#include "util.hpp"
#include "context.hpp"
#include <windows.h>
#include <algorithm>
#include <fstream>
#include <sstream>
#include <map>
#include <mutex>

namespace hek::procs {

bool isDangerous(const std::string& cmd) {
    std::string c = toLower(cmd);
    static const char* patterns[] = {
        "rm -rf", "rmdir /s", "format ", "del /f", "shutdown",
        "mkfs", "dd if=", ":(){", "fork bomb",
        "git push --force", "git push -f", "> /dev/sda",
        "reg delete", "reg add", "diskpart"
    };
    for (auto p : patterns) if (c.find(p) != std::string::npos) return true;
    return false;
}

namespace {
struct BgProc {
    HANDLE hProcess = nullptr;
    HANDLE hOut = nullptr;  // pipe we read from
    std::string output;
};
std::map<int, BgProc> g_bg;
std::mutex g_mu;
int g_nextId = 1;

std::string runForeground(const std::string& cmd, int timeoutS) {
    SECURITY_ATTRIBUTES sa{}; sa.nInheritHandle = TRUE;
    HANDLE rOut, wOut; CreatePipe(&rOut, &wOut, &sa, 0);
    SetHandleInformation(rOut, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si{}; si.cb = sizeof(si);
    si.hStdOutput = wOut; si.hStdError = wOut;
    si.dwFlags = STARTF_USESTDHANDLES;
    PROCESS_INFORMATION pi{};
    // Run via cmd.exe /c so shell features (pipes, redirects) work.
    std::string full = "cmd.exe /c " + cmd;
    std::vector<char> buf(full.begin(), full.end()); buf.push_back(0);
    if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(wOut); CloseHandle(rOut);
        return "ERROR: CreateProcess failed: " + std::to_string(GetLastError());
    }
    CloseHandle(wOut);

    std::string out;
    DWORD wait = WaitForSingleObject(pi.hProcess, timeoutS * 1000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        out += "\n[timeout after " + std::to_string(timeoutS) + "s]";
    }
    // drain pipe
    char tmp[4096]; DWORD got;
    while (ReadFile(rOut, tmp, sizeof(tmp), &got, nullptr) && got) out.append(tmp, got);
    DWORD code = 0; GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rOut);
    std::ostringstream o;
    o << "exit code: " << code << "\n--- output ---\n" << out;
    return o.str();
}
}

void registerTools(Context& ctx) {
    ctx.add({"run_command", "terminal", "control",
        "Run a shell command (cmd.exe) and return stdout/stderr/exit code. "
        "timeout_s default 60 (max 600). For long-running jobs set background=true and "
        "manage them with the process tool. Dangerous commands always require explicit approval. "
        "Needs user permission.",
        json::parse(R"JS({"type":"object","properties":{"command":{"type":"string"},"timeout_s":{"type":"number"},"background":{"type":"boolean"}},"required":["command"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::string cmd = a.value("command","");
            int timeout = std::min(600, a.value("timeout_s", 60));
            if (a.value("background", false)) {
                std::lock_guard<std::mutex> lk(g_mu);
                int id = g_nextId++;
                SECURITY_ATTRIBUTES sa{}; sa.nInheritHandle = TRUE;
                HANDLE rOut, wOut; CreatePipe(&rOut, &wOut, &sa, 0);
                SetHandleInformation(rOut, HANDLE_FLAG_INHERIT, 0);
                STARTUPINFOA si{}; si.cb = sizeof(si);
                si.hStdOutput = wOut; si.hStdError = wOut;
                si.dwFlags = STARTF_USESTDHANDLES;
                PROCESS_INFORMATION pi{};
                std::string full = "cmd.exe /c " + cmd;
                std::vector<char> buf(full.begin(), full.end()); buf.push_back(0);
                if (!CreateProcessA(nullptr, buf.data(), nullptr, nullptr, TRUE,
                        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
                    CloseHandle(wOut); CloseHandle(rOut);
                    return "ERROR: cannot start";
                }
                CloseHandle(wOut);
                BgProc bp; bp.hProcess = pi.hProcess; bp.hOut = rOut;
                g_bg[id] = bp;
                std::ostringstream o; o << "Started background process #" << id;
                return o.str();
            }
            return runForeground(cmd, timeout);
        },
        [&ctx](const json& a) -> json {
            std::string cmd = a.value("command","");
            std::string title = isDangerous(cmd) ? "⚠️ دستور خطرناک" : "اجرای دستور";
            return {{"title", title}, {"detail", cmd}};
        }
    });

    ctx.add({"process", "terminal", "control",
        "Manage background processes started with run_command(background=true): "
        "list, log, write, wait, kill.",
        json::parse(R"JS({"type":"object","properties":{"action":{"type":"string","enum":["list","log","write","wait","kill"]},"id":{"type":"number"},"text":{"type":"string"},"timeout_s":{"type":"number"}},"required":["action"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::string action = a.value("action","");
            std::lock_guard<std::mutex> lk(g_mu);
            if (action == "list") {
                std::ostringstream o;
                for (auto& kv : g_bg) {
                    DWORD code = 0; GetExitCodeProcess(kv.second.hProcess, &code);
                    o << "#" << kv.first << " (exit=" << code << ")\n";
                }
                return o.str().empty() ? "No background processes." : o.str();
            }
            int id = a.value("id", 0);
            auto it = g_bg.find(id);
            if (it == g_bg.end()) return "ERROR: no such process";
            BgProc& bp = it->second;
            if (action == "log") {
                char tmp[4096]; DWORD got;
                while (ReadFile(bp.hOut, tmp, sizeof(tmp), &got, nullptr) && got)
                    bp.output.append(tmp, got);
                return bp.output.empty() ? "(no output yet)" : bp.output;
            }
            if (action == "write") {
                // We didn't keep a stdin handle; report so the agent falls back
                // to a foreground command for interactive jobs.
                return "ERROR: stdin write not supported; use a foreground command";
            }
            if (action == "wait") {
                int t = a.value("timeout_s", 30);
                WaitForSingleObject(bp.hProcess, t * 1000);
                DWORD code = 0; GetExitCodeProcess(bp.hProcess, &code);
                return "exit code: " + std::to_string(code);
            }
            if (action == "kill") {
                TerminateProcess(bp.hProcess, 1);
                CloseHandle(bp.hProcess); CloseHandle(bp.hOut);
                g_bg.erase(it);
                return "OK: killed #" + std::to_string(id);
            }
            return "ERROR: unknown action";
        }
    });

    ctx.add({"execute_code", "code", "write",
        "Run a JavaScript or Python script to do multi-step work in ONE call. "
        "Inside the script use tools.web_search({query}), tools.read_file({path}) etc. "
        "print/console.log what you want back. Timeout default 60s (max 300). Needs user permission.",
        json::parse(R"JS({"type":"object","properties":{"language":{"type":"string","enum":["javascript","python"]},"code":{"type":"string"},"timeout_s":{"type":"number"}},"required":["language","code"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::string lang = a.value("language","javascript");
            std::string code = a.value("code","");
            // Write to a temp file and invoke node / python.
            char tmpPath[MAX_PATH]; GetTempFileNameA(".", "hek", 0, tmpPath);
            std::string ext = (lang == "python") ? ".py" : ".js";
            std::string scriptPath = std::string(tmpPath) + ext;
            std::ofstream f(scriptPath, std::ios::binary); f << code; f.close();
            std::string exe = (lang == "python") ? "python" : "node";
            std::string cmd = exe + " \"" + scriptPath + "\"";
            std::string out = runForeground(cmd, std::min(300, a.value("timeout_s", 60)));
            DeleteFileA(scriptPath.c_str()); DeleteFileA(tmpPath);
            return out;
        },
        [&ctx](const json& a) -> json { return {{"title","اجرای کد"},{"detail",a.value("language","")}}; }
    });
}

} // namespace hek::procs
