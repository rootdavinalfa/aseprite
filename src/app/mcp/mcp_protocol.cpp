// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/mcp/mcp_protocol.h"

#include "app/mcp/mcp_tools_registry.h"
#include "ver/info.h"

namespace app {
namespace mcp {
namespace protocol {

const char kModernVersion[] = "2026-07-28";
const char* const kLegacyVersions[] = {
  "2024-11-05",
  "2025-03-26",
  "2025-06-18",
  "2025-11-25",
};
const char kDefaultLegacyVersion[] = "2025-06-18";

namespace {

const int kTtlMs = 86400000;
const char kCacheScope[] = "public";

// Workflow guidance for clients: the diivi/aseprite-mcp draw loop.
const char kInstructions[] =
  "Aseprite pixel-art server. Tools mirror diivi/aseprite-mcp: create/load a .aseprite file, "
  "draw with the drawing tools, inspect with export_frame at 8-10x scale, iterate. "
  "run_lua_script is the escape hatch.";

json11::Json serverInfo()
{
  return json11::Json::object {
    { "name", "aseprite" },
    { "version", get_app_version() },
  };
}

json11::Json serverMeta()
{
  return json11::Json::object {
    { "io.modelcontextprotocol/serverInfo", serverInfo() },
  };
}

json11::Json makeResult(const json11::Json& id, const json11::Json& result)
{
  return json11::Json::object {
    { "jsonrpc", "2.0" },
    { "id", id },
    { "result", result },
  };
}

// Adds the fields carried by every modern result; legacy results
// carry only the method payload.
json11::Json makeResultForEra(const json11::Json& id, const Era era, json11::Json::object result)
{
  if (era == Era::Modern) {
    result["resultType"] = "complete";
    result["ttlMs"] = kTtlMs;
    result["cacheScope"] = kCacheScope;
    result["_meta"] = serverMeta();
  }
  return makeResult(id, result);
}

json11::Json handleDiscover(const json11::Json& id)
{
  return makeResultForEra(id, Era::Modern, {
    { "supportedVersions", json11::Json::array { kModernVersion } },
    { "capabilities", json11::Json::object {
        { "tools", json11::Json::object {
            { "listChanged", false },
        } },
    } },
    { "instructions", kInstructions },
  });
}

bool isLegacyVersion(const std::string& version)
{
  for (const char* const supported : kLegacyVersions) {
    if (version == supported)
      return true;
  }
  return false;
}

// Legacy handshake: echo the requested version when supported,
// otherwise reply with the fallback version.
json11::Json handleInitialize(const json11::Json& id, const json11::Json& params)
{
  const std::string requested = params["protocolVersion"].string_value();
  return makeResult(id, json11::Json::object {
    { "protocolVersion", isLegacyVersion(requested)
                           ? requested
                           : std::string(kDefaultLegacyVersion) },
    { "capabilities", json11::Json::object {
        { "tools", json11::Json::object {
            { "listChanged", false },
        } },
    } },
    { "serverInfo", serverInfo() },
  });
}

json11::Json handleToolsList(const json11::Json& id, const Era era)
{
  json11::Json::array tools;
  for (const McpTool& tool : mcp_tools()) {
    tools.push_back(json11::Json::object {
      { "name", tool.name },
      { "description", tool.description },
      { "inputSchema", tool.inputSchema },
    });
  }
  return makeResultForEra(id, era, { { "tools", tools } });
}

// Renders a tool result; "isError" marks a tool execution error while
// the JSON-RPC envelope stays successful.
json11::Json toolResultJson(const json11::Json& id, const Era era, const ToolResult& toolResult)
{
  json11::Json::array content;
  if (!toolResult.text.empty()) {
    content.push_back(json11::Json::object {
      { "type", "text" },
      { "text", toolResult.text },
    });
  }
  if (!toolResult.imageBase64.empty()) {
    content.push_back(json11::Json::object {
      { "type", "image" },
      { "data", toolResult.imageBase64 },
      { "mimeType", toolResult.imageMime },
    });
  }
  return makeResultForEra(id, era, {
    { "content", content },
    { "isError", toolResult.isError },
  });
}

json11::Json handleToolsCall(const json11::Json& id, const Era era,
                             const json11::Json& params)
{
  const std::string name = params["name"].string_value();
  for (const McpTool& tool : mcp_tools()) {
    if (tool.name != name)
      continue;

    const json11::Json& arguments = params["arguments"];

    // MCP input validation: a required argument that is missing or
    // null makes the call a tool execution error, not a protocol
    // error. Only presence is checked here, never the value type.
    for (const json11::Json& required : tool.inputSchema["required"].array_items()) {
      if (required.is_string() && arguments[required.string_value()].is_null()) {
        ToolResult result;
        result.isError = true;
        result.text = "Missing required argument: " + required.string_value();
        return toolResultJson(id, era, result);
      }
    }

    return toolResultJson(id, era, tool.run(arguments));
  }
  return makeError(id, -32602, "Unknown tool: " + name);
}

} // anonymous namespace

Era detectEra(const json11::Json& request)
{
  if (request["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"].is_string())
    return Era::Modern;
  return Era::Legacy;
}

bool isNotification(const json11::Json& request)
{
  return request["id"].is_null();
}

json11::Json handle(const json11::Json& request, const Era era)
{
  if (!request.is_object())
    return makeError(json11::Json(), -32600, "Invalid Request");

  // notifications/initialized is accepted (no reply required). A
  // notifications/cancelled has nothing to cancel: tools are short and
  // run to completion before the next message is read.
  // ponytail: no mid-tool cancellation; revisit if long-running tools appear.
  if (isNotification(request))
    return json11::Json();

  const json11::Json& id = request["id"];
  const std::string method = request["method"].string_value();

  if (method == "tools/list")
    return handleToolsList(id, era);
  if (method == "tools/call")
    return handleToolsCall(id, era, request["params"]);
  if (method == "initialize" && era == Era::Legacy)
    return handleInitialize(id, request["params"]);
  if (method == "ping" && era == Era::Legacy)
    return makeResult(id, json11::Json::object {});
  if (method == "server/discover" && era == Era::Modern)
    return handleDiscover(id);

  return makeError(id, -32601, "Method not found");
}

json11::Json makeError(const json11::Json& id, const int code, const std::string& message,
                       const json11::Json& data)
{
  json11::Json::object error = {
    { "code", code },
    { "message", message },
  };
  if (!data.is_null())
    error["data"] = data;

  return json11::Json::object {
    { "jsonrpc", "2.0" },
    { "id", id },
    { "error", error },
  };
}

std::string dumpCompact(const json11::Json& value)
{
  const std::string spaced = value.dump();
  std::string out;
  out.reserve(spaced.size());

  bool inString = false;
  bool escaped = false;
  for (const char c : spaced) {
    if (inString) {
      out.push_back(c);
      if (escaped)
        escaped = false;
      else if (c == '\\')
        escaped = true;
      else if (c == '"')
        inString = false;
      continue;
    }
    if (c == '"') {
      inString = true;
      out.push_back(c);
    }
    // ponytail: json11's dump() only emits spaces after ':' and ','
    // (outside strings), so dropping those spaces is exact for it.
    else if (c != ' ') {
      out.push_back(c);
    }
  }
  return out;
}

} // namespace protocol
} // namespace mcp
} // namespace app
