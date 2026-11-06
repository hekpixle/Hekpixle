// Hekpixle — shared JSON type (nlohmann/json single-header, vendored via vcpkg).
// The whole IPC + agent layer speaks nlohmann::json. We alias it here so the
// rest of the code stays short and consistent.
#pragma once
#include "nlohmann_json.hpp"
#include <string>

namespace hek {

using json = nlohmann::json;
using jobj = nlohmann::ordered_json;

// Convenience: read a JSON file (returns fallback on any error).
json read_json_file(const std::string& path, const json& fallback);

// Atomic write: tmp file + rename (mirrors the JS db.save() pattern).
bool write_json_file(const std::string& path, const json& value);

// UTF-8 <-> UTF-16 helpers (Win32 APIs are wide).
std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& utf16);

} // namespace hek
