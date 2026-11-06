// Hekpixle — file tools (list_dir, read_file, write_file, patch, search_files,
// make_dir, move_or_rename, delete_path, file_history, file_restore).
// Mirrors lib/files.js. Uses Win32 file APIs + a checkpoints/ folder for
// automatic backups before write/patch (same behaviour as the JS version).
#include "tools.hpp"
#include "util.hpp"
#include "context.hpp"
#include <windows.h>
#include <shellapi.h>
#include <algorithm>
#include <fstream>
#include <functional>
#include <sstream>
#include <regex>
#pragma comment(lib, "shell32.lib")

namespace hek::files {

namespace {
std::string checkpointsDir(Context& ctx) {
    return ctx.userDataDir() + "\\checkpoints";
}
std::string normSlashes(std::string p) {
    for (auto& c : p) if (c == '/') c = '\\';
    return p;
}
// Save a checkpoint of a file before overwriting it.
void checkpoint(Context& ctx, const std::string& path) {
    if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    std::ifstream in(path, std::ios::binary);
    if (!in) return;
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    std::string dir = checkpointsDir(ctx);
    ensureDir(dir);
    // filename: sanitized path + timestamp
    std::string safe = path;
    for (auto& c : safe) if (c == '\\' || c == '/' || c == ':') c = '_';
    std::string stamp = std::to_string(GetTickCount64());
    std::string cp = dir + "\\" + stamp + "_" + safe;
    std::ofstream out(cp, std::ios::binary);
    out << content;
}
std::string readFileFull(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return "";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}

void registerTools(Context& ctx) {
    ctx.add({"list_dir", "files", "read",
        "List files and folders inside a directory.",
        json::parse(R"JS({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::string p = normSlashes(a.value("path",""));
            if (p.empty()) return "ERROR: path required";
            auto items = listDir(p);
            std::ostringstream o;
            for (auto& n : items) {
                bool dir = (GetFileAttributesA((p + "\\" + n).c_str()) & FILE_ATTRIBUTE_DIRECTORY) != 0;
                o << (dir ? "[DIR]  " : "       ") << n << "\n";
            }
            return o.str();
        }
    });

    ctx.add({"read_file", "files", "read",
        "Read a text file. Optional offset (1-based line) and limit (lines) for big files; "
        "numbered=true prefixes line numbers. Without them: first 60000 characters.",
        json::parse(R"JS({"type":"object","properties":{"path":{"type":"string"},"offset":{"type":"number"},"limit":{"type":"number"},"numbered":{"type":"boolean"}},"required":["path"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::string p = normSlashes(a.value("path",""));
            std::string content = readFileFull(p);
            if (content.empty()) return "ERROR: cannot read (empty or missing): " + p;
            if (content.size() > 60000 && !a.contains("offset")) content = content.substr(0, 60000) + "\n…(truncated)";
            if (a.contains("offset") || a.value("numbered", false)) {
                std::vector<std::string> lines;
                std::string cur;
                for (char c : content) { if (c == '\n') { lines.push_back(cur); cur.clear(); } else cur += c; }
                if (!cur.empty()) lines.push_back(cur);
                int off = a.value("offset", 1);
                int lim = a.value("limit", (int)lines.size());
                std::ostringstream o;
                for (int i = std::max(0, off-1); i < (int)lines.size() && (i - std::max(0,off-1)) < lim; ++i) {
                    if (a.value("numbered", false)) o << (i+1) << "\t";
                    o << lines[i] << "\n";
                }
                return o.str();
            }
            return content;
        }
    });

    ctx.add({"write_file", "files", "write",
        "Create or overwrite a text file (a checkpoint of the old version is saved automatically). "
        "For small edits prefer patch. Needs user permission.",
        json::parse(R"JS({"type":"object","properties":{"path":{"type":"string"},"content":{"type":"string"}},"required":["path","content"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::string p = normSlashes(a.value("path",""));
            std::string content = a.value("content","");
            checkpoint(ctx, p);
            ensureDir(p.substr(0, p.find_last_of("\\/")));
            std::ofstream out(p, std::ios::binary);
            if (!out) return "ERROR: cannot write: " + p;
            out << content;
            return "OK: wrote " + std::to_string(content.size()) + " bytes to " + p;
        },
        [&ctx](const json& a) -> json {
            std::string p = normSlashes(a.value("path",""));
            return {{"title", "نوشتن فایل"}, {"detail", p}};
        }
    });

    ctx.add({"patch", "files", "write",
        "Edit a file by replacing old_string with new_string (exact match). "
        "Fails if old_string is not found or ambiguous. A checkpoint is saved. Needs user permission.",
        json::parse(R"JS({"type":"object","properties":{"path":{"type":"string"},"old_string":{"type":"string"},"new_string":{"type":"string"},"replace_all":{"type":"boolean"}},"required":["path","old_string","new_string"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::string p = normSlashes(a.value("path",""));
            std::string oldS = a.value("old_string","");
            std::string newS = a.value("new_string","");
            std::string content = readFileFull(p);
            if (content.empty()) return "ERROR: cannot read: " + p;
            size_t pos = content.find(oldS);
            if (pos == std::string::npos) return "ERROR: old_string not found";
            if (!a.value("replace_all", false)) {
                size_t pos2 = content.find(oldS, pos + 1);
                if (pos2 != std::string::npos) return "ERROR: old_string is ambiguous; add more context or use replace_all";
            }
            checkpoint(ctx, p);
            std::string out;
            size_t last = 0;
            bool all = a.value("replace_all", false);
            while (true) {
                size_t p2 = content.find(oldS, last);
                if (p2 == std::string::npos) { out.append(content, last, std::string::npos); break; }
                out.append(content, last, p2 - last);
                out += newS;
                last = p2 + oldS.size();
                if (!all) { out.append(content, last, std::string::npos); break; }
            }
            std::ofstream f(p, std::ios::binary);
            f << out;
            return "OK: patched " + p;
        },
        [&ctx](const json& a) -> json {
            return {{"title", "ویرایش فایل"}, {"detail", normSlashes(a.value("path",""))}};
        }
    });

    ctx.add({"search_files", "files", "read",
        "Search file CONTENTS with a regex (mode=content) or find files by NAME glob "
        "(mode=name) under a folder. Skips node_modules/.git. Returns path:line: text.",
        json::parse(R"JS({"type":"object","properties":{"pattern":{"type":"string","description":"regex for content mode, glob for name mode"},"path":{"type":"string","description":"folder (default: workspace/home)"},"mode":{"type":"string","enum":["content","name"]},"glob":{"type":"string"},"case_insensitive":{"type":"boolean"},"max_results":{"type":"number"}},"required":["pattern"]})JS"),
        [&ctx](const json& a, Call& call) -> std::string {
            auto s = ctx.loadSettings();
            std::string root = a.value("path", s.value("workspaceDir",""));
            if (root.empty()) root = std::getenv("USERPROFILE") ? std::getenv("USERPROFILE") : "C:\\";
            std::string mode = a.value("mode","content");
            std::string pat = a.value("pattern","");
            bool ci = a.value("case_insensitive", false);
            int maxR = a.value("max_results", 50);
            std::regex re;
            try { re = std::regex(pat, ci ? std::regex::icase : std::regex::ECMAScript); }
            catch (...) { return "ERROR: bad regex"; }
            std::ostringstream out;
            int count = 0;
            std::function<void(const std::string&)> walk = [&](const std::string& dir) {
                if (count >= maxR || call.stop) return;
                for (auto& n : listDir(dir)) {
                    if (n == "node_modules" || n == ".git" || n == "checkpoints") continue;
                    std::string full = dir + "\\" + n;
                    if ((GetFileAttributesA(full.c_str()) & FILE_ATTRIBUTE_DIRECTORY)) {
                        walk(full);
                    } else if (mode == "name") {
                        if (std::regex_search(n, re)) { out << full << "\n"; if (++count >= maxR) return; }
                    } else {
                        std::ifstream in(full, std::ios::binary);
                        if (!in) continue;
                        std::string line; int ln = 0;
                        while (std::getline(in, line)) {
                            ++ln;
                            if (std::regex_search(line, re)) {
                                out << full << ":" << ln << ": " << line << "\n";
                                if (++count >= maxR) return;
                            }
                        }
                    }
                }
            };
            walk(root);
            return out.str().empty() ? "No matches." : out.str();
        }
    });

    ctx.add({"make_dir", "files", "write", "Create a directory. Needs user permission.",
        json::parse(R"JS({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::string p = normSlashes(a.value("path",""));
            return ensureDir(p) ? "OK: created " + p : "ERROR: cannot create " + p;
        },
        [&ctx](const json& a) -> json { return {{"title","ساخت پوشه"},{"detail",normSlashes(a.value("path",""))}}; }
    });

    ctx.add({"move_or_rename", "files", "write", "Move or rename a file/folder. Needs user permission.",
        json::parse(R"JS({"type":"object","properties":{"from":{"type":"string"},"to":{"type":"string"}},"required":["from","to"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::string from = normSlashes(a.value("from",""));
            std::string to   = normSlashes(a.value("to",""));
            ensureDir(to.substr(0, to.find_last_of("\\/")));
            return MoveFileA(from.c_str(), to.c_str()) ? "OK" : "ERROR: move failed";
        },
        [&ctx](const json& a) -> json {
            return {{"title","جابه‌جایی فایل"},{"detail",normSlashes(a.value("from",""))+" -> "+normSlashes(a.value("to",""))}};
        }
    });

    ctx.add({"delete_path", "files", "write",
        "Delete a file/folder (moved to the Recycle Bin). Needs user permission.",
        json::parse(R"JS({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::string p = normSlashes(a.value("path",""));
            // Recycle via SHFileOperation (double-null terminated wide string).
            std::wstring w = widen(p + '\0');
            SHFILEOPSTRUCTW op{};
            op.wFunc = FO_DELETE;
            op.pFrom = w.c_str();
            op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT;
            return (SHFileOperationW(&op) == 0) ? "OK: sent to Recycle Bin" : "ERROR: delete failed";
        },
        [&ctx](const json& a) -> json { return {{"title","حذف"},{"detail",normSlashes(a.value("path",""))}}; }
    });

    ctx.add({"file_history", "files", "read",
        "List automatic checkpoints (backups made before write_file/patch changed a file). "
        "Optionally filter by path.",
        json::parse(R"JS({"type":"object","properties":{"path":{"type":"string"}},"required":[]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::string dir = checkpointsDir(ctx);
            if (!dirExists(dir)) return "No checkpoints.";
            std::string filter = a.value("path","");
            if (!filter.empty()) { for (auto& c : filter) if (c=='\\'||c=='/'||c==':') c='_'; }
            std::ostringstream o; int n = 0;
            for (auto& f : listDir(dir)) {
                if (!filter.empty() && f.find(filter) == std::string::npos) continue;
                o << ++n << ". " << f << "\n";
            }
            return o.str().empty() ? "No matching checkpoints." : o.str();
        }
    });

    ctx.add({"file_restore", "files", "write",
        "Restore a file from a checkpoint (rollback). The current version is checkpointed first. "
        "Needs user permission.",
        json::parse(R"JS({"type":"object","properties":{"checkpoint":{"type":"string"},"path":{"type":"string"}},"required":[]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::string dir = checkpointsDir(ctx);
            std::string cp  = a.value("checkpoint","");
            std::string path = normSlashes(a.value("path",""));
            // find latest checkpoint matching path if cp empty
            if (cp.empty() && !path.empty()) {
                std::string filt = path; for (auto& c : filt) if (c=='\\'||c=='/'||c==':') c='_';
                std::string latest; ULONGLONG newest = 0;
                for (auto& f : listDir(dir)) {
                    if (f.find(filt) == std::string::npos) continue;
                    ULONGLONG ts = std::stoull(f.substr(0, f.find('_')));
                    if (ts > newest) { newest = ts; latest = f; }
                }
                if (latest.empty()) return "ERROR: no checkpoint for " + path;
                cp = latest;
            }
            std::string cpPath = dir + "\\" + cp;
            if (GetFileAttributesA(cpPath.c_str()) == INVALID_FILE_ATTRIBUTES) return "ERROR: checkpoint not found";
            if (!path.empty()) checkpoint(ctx, path);  // current version first
            std::string content = readFileFull(cpPath);
            std::ofstream out(path.empty() ? cp.substr(cp.find('_')+1) : path, std::ios::binary);
            out << content;
            return "OK: restored";
        },
        [&ctx](const json& a) -> json { return {{"title","بازیابی فایل"},{"detail",a.value("path",a.value("checkpoint",""))}}; }
    });
}

} // namespace hek::files
