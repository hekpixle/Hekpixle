// Hekpixle — agent loop.
//
// Mirrors runAgent() in main.js: send messages + tools to the LLM, parse
// tool_calls, ask permission for write/control tools, run them, append the
// results, repeat until the LLM stops calling tools or maxSteps is hit.
// Every step streams events to the UI via ctx.emit().
#pragma once
#include "context.hpp"
#include "json.hpp"
#include <atomic>
#include <string>

namespace hek {

// One agent run. `messages` is the full chat history [{role, content}].
// Streams events: {type:"delta", text}, {type:"tool", name, args, result},
// {type:"permission", id, title, detail}, {type:"clarify", id, question, choices},
// {type:"todo", todos}, {type:"done"}/{type:"error", message}.
// Returns the final assistant text.
std::string runAgent(Context& ctx, const json& messages, Call& call);

// Compress an old conversation (chat:compress IPC). Returns the shorter
// message list, or json with {"_error": ...} on failure.
json compressChat(Context& ctx, const json& messages);

} // namespace hek
