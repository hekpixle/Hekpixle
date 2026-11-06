// Hekpixle — shared declarations for the lib tool modules.
//
// Each module exposes registerTools(Context&) that adds its tools to the
// registry. A few also expose helper functions used elsewhere (cron parsing,
// mcp/plugin status, telegram messaging). Namespaced per module to avoid
// collisions, mirroring lib/<name>.js module.exports.
#pragma once
#include "context.hpp"

namespace hek::web        { void registerTools(Context& ctx); }
namespace hek::files      { void registerTools(Context& ctx); }
namespace hek::computer   { void registerTools(Context& ctx); }
namespace hek::browser    { void registerTools(Context& ctx); }
namespace hek::mcp        { void registerTools(Context& ctx); json status(); }
namespace hek::media      { void registerTools(Context& ctx); }
namespace hek::procs      { void registerTools(Context& ctx); bool isDangerous(const std::string& cmd); }
namespace hek::ps         { void registerTools(Context& ctx); }
namespace hek::skills     { void registerTools(Context& ctx); }
namespace hek::cron       {
    void registerTools(Context& ctx);
    json list(Context& ctx);
    json act(Context& ctx, const json& a);
}
namespace hek::plugins    { void registerTools(Context& ctx); json status(Context& ctx); }
namespace hek::agent_tools { void registerTools(Context& ctx); }
