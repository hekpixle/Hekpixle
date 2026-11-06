// Hekpixle — skills tools (skills_list, skill_view, skill_manage).
// Mirrors lib/skills.js. Skills live in userDataDir/skills/<name>/SKILL.md
// (user skills) and skills_seed/<name>/SKILL.md next to the exe (builtin).
#include <windows.h>
#include <shellapi.h>
#include "tools.hpp"
#include "util.hpp"
#include "context.hpp"
#include <fstream>
#include <map>
#include <sstream>
#include <regex>

namespace hek::skills {

namespace {
std::string dirFor(Context& ctx, const std::string& name) {
    std::string user = ctx.userDataDir() + "\\skills\\" + name;
    if (dirExists(user)) return user;
    std::string builtin = exeDir() + "\\skills_seed\\" + name;
    if (dirExists(builtin)) return builtin;
    return "";
}
std::string readSkillMd(const std::string& dir) {
    std::ifstream in(dir + "\\SKILL.md", std::ios::binary);
    if (!in) return "";
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
// Parse YAML-ish frontmatter (name:, description:).
std::map<std::string,std::string> parseFrontmatter(const std::string& md) {
    std::map<std::string,std::string> out;
    if (md.size() < 4 || md.substr(0,3) != "---") return out;
    size_t end = md.find("\n---", 3);
    if (end == std::string::npos) return out;
    std::string body = md.substr(3, end - 3);
    std::istringstream ss(body); std::string line;
    while (std::getline(ss, line)) {
        size_t c = line.find(':');
        if (c == std::string::npos) continue;
        std::string k = trim(line.substr(0,c));
        std::string v = trim(line.substr(c+1));
        if (!v.empty() && v.front() == '"' && v.back() == '"') v = v.substr(1, v.size()-2);
        out[k] = v;
    }
    return out;
}
}

void registerTools(Context& ctx) {
    ctx.add({"skills_list", "skills", "read",
        "List available skills (name + description).",
        json::parse(R"JS({"type":"object","properties":{},"required":[]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::ostringstream o; int n = 0;
            auto emit = [&](const std::string& dir, const std::string& src) {
                if (!dirExists(dir)) return;
                for (auto& name : listDir(dir, true)) {
                    std::string md = readSkillMd(dir + "\\" + name);
                    if (md.empty()) continue;
                    auto fm = parseFrontmatter(md);
                    o << ++n << ". [" << src << "] " << (fm.count("name")?fm["name"]:name);
                    if (fm.count("description")) o << " — " << fm["description"];
                    o << "\n";
                }
            };
            emit(ctx.userDataDir() + "\\skills", "user");
            emit(exeDir() + "\\skills_seed", "builtin");
            return o.str().empty() ? "No skills installed." : o.str();
        }
    });

    ctx.add({"skill_view", "skills", "read",
        "Load a skill's full instructions (SKILL.md), or one of its extra files via file_path. "
        "Do this BEFORE starting a task that matches a skill in the index.",
        json::parse(R"JS({"type":"object","properties":{"name":{"type":"string"},"file_path":{"type":"string","description":"optional file inside the skill folder"}},"required":["name"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::string name = a.value("name","");
            std::string fp = a.value("file_path","");
            std::string dir = dirFor(ctx, name);
            if (dir.empty()) return "ERROR: skill not found: " + name;
            std::string target = dir + "\\" + (fp.empty() ? "SKILL.md" : fp);
            std::ifstream in(target, std::ios::binary);
            if (!in) return "ERROR: file not found: " + fp;
            return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        }
    });

    ctx.add({"skill_manage", "skills", "write",
        "Create, edit, patch or delete YOUR skills (self-improvement). "
        "Actions: create (name, content = full SKILL.md with frontmatter name/description), "
        "edit (name, content), patch (name, old_string, new_string), write_file (name, file_path, content), "
        "delete (name). Needs user permission.",
        json::parse(R"JS({"type":"object","properties":{"action":{"type":"string","enum":["create","edit","patch","write_file","delete"]},"name":{"type":"string"},"content":{"type":"string"},"old_string":{"type":"string"},"new_string":{"type":"string"},"file_path":{"type":"string"}},"required":["action","name"]})JS"),
        [&ctx](const json& a, Call&) -> std::string {
            std::string action = a.value("action","");
            std::string name = a.value("name","");
            std::string dir = ctx.userDataDir() + "\\skills\\" + name;
            if (action == "create") {
                std::string content = a.value("content","");
                auto fm = parseFrontmatter(content);
                if (!fm.count("name") || !fm.count("description"))
                    return "ERROR: content must start with frontmatter containing name: and description:";
                ensureDir(dir);
                std::ofstream(dir + "\\SKILL.md", std::ios::binary) << content;
                return "OK: created skill " + name;
            }
            if (action == "edit") {
                std::string content = a.value("content","");
                auto fm = parseFrontmatter(content);
                if (!fm.count("name") || !fm.count("description"))
                    return "ERROR: content must keep frontmatter name:/description:";
                ensureDir(dir);
                std::ofstream(dir + "\\SKILL.md", std::ios::binary) << content;
                return "OK: edited skill " + name;
            }
            if (action == "patch") {
                std::string path = dir + "\\SKILL.md";
                std::ifstream in(path, std::ios::binary);
                if (!in) return "ERROR: skill not found";
                std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
                std::string oldS = a.value("old_string","");
                std::string newS = a.value("new_string","");
                size_t p = content.find(oldS);
                if (p == std::string::npos) return "ERROR: old_string not found";
                content.replace(p, oldS.size(), newS);
                std::ofstream(path, std::ios::binary) << content;
                return "OK: patched";
            }
            if (action == "write_file") {
                ensureDir(dir);
                std::string fp = a.value("file_path","");
                std::ofstream(dir + "\\" + fp, std::ios::binary) << a.value("content","");
                return "OK: wrote " + fp;
            }
            if (action == "delete") {
                // recursive delete
                std::wstring w = widen(dir + '\0');
                SHFILEOPSTRUCTW op{}; op.wFunc = FO_DELETE; op.pFrom = w.c_str();
                op.fFlags = FOF_NOCONFIRMATION | FOF_SILENT;
                return SHFileOperationW(&op) == 0 ? "OK: deleted" : "ERROR: delete failed";
            }
            return "ERROR: unknown action";
        },
        [&ctx](const json& a) -> json { return {{"title","مدیریت اسکیل"},{"detail",a.value("action","")+" "+a.value("name","")}}; }
    });
}

} // namespace hek::skills
