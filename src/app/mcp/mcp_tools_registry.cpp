// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/mcp/mcp_tools_registry.h"

#include "app/mcp/mcp_tool_context.h"

#include <cctype>

namespace app {
namespace mcp {

std::string argString(const json11::Json& args, const char* name, const std::string& def)
{
  const json11::Json& value = args[name];
  return value.is_string() ? value.string_value() : def;
}

int argInt(const json11::Json& args, const char* name, int def)
{
  const json11::Json& value = args[name];
  return value.is_number() ? value.int_value() : def;
}

double argDouble(const json11::Json& args, const char* name, double def)
{
  const json11::Json& value = args[name];
  return value.is_number() ? value.number_value() : def;
}

bool argBool(const json11::Json& args, const char* name, bool def)
{
  const json11::Json& value = args[name];
  return value.is_bool() ? value.bool_value() : def;
}

bool parseHexColor(const std::string& value, int& r, int& g, int& b, int& a)
{
  std::string hex;
  for (const char c : value) {
    if (c != ' ' && c != '\t' && c != '\n' && c != '\r')
      hex.push_back(c);
  }
  if (!hex.empty() && hex[0] == '#')
    hex.erase(0, 1);

  // The four hex digits of an 8-bit channel, doubled from a short form.
  std::string full;
  if (hex.size() == 3 || hex.size() == 4) {
    for (const char c : hex) {
      full.push_back(c);
      full.push_back(c);
    }
  }
  else {
    full = hex;
  }
  if (full.size() != 6 && full.size() != 8)
    return false;

  int* channels[4] = { &r, &g, &b, &a };
  for (int i = 0; i < 4; ++i) {
    if (i == 3 && full.size() == 6) {
      *channels[i] = 255;
      continue;
    }
    const int last = i * 2 + 2;
    int v = 0;
    for (int j = i * 2; j < last; ++j) {
      if (!std::isxdigit(static_cast<unsigned char>(full[j])))
        return false;
      v = v * 16 + (std::isdigit(static_cast<unsigned char>(full[j])) ? full[j] - '0' :
                   std::tolower(static_cast<unsigned char>(full[j])) - 'a' + 10);
    }
    *channels[i] = v;
  }
  return true;
}

std::string luaString(const std::string& value)
{
  std::string out;
  out.reserve(value.size());
  for (const char c : value) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '"': out += "\\\""; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\0': out += "\\0"; break;
      default: out.push_back(c); break;
    }
  }
  return out;
}

McpTool LuaTool(std::string name,
                std::string description,
                json11::Json inputSchema,
                const bool openFilename,
                std::function<std::string(const json11::Json&)> buildScript)
{
  McpTool tool;
  tool.name = std::move(name);
  tool.description = std::move(description);
  tool.inputSchema = std::move(inputSchema);
  tool.run = [openFilename, buildScript](const json11::Json& args) -> ToolResult {
    if (openFilename) {
      const std::string filename = argString(args, "filename");
      if (!filename.empty()) {
        ToolContext& context = ToolContext::instance();
        std::string error;
        if (!context.openDocument(filename, error)) {
          ToolResult result;
          result.isError = true;
          result.text = error;
          return result;
        }
      }
    }
    return ToolContext::instance().runLua(buildScript(args));
  };
  return tool;
}

McpTool NativeTool(std::string name,
                   std::string description,
                   json11::Json inputSchema,
                   std::function<ToolResult(const json11::Json&)> fn)
{
  McpTool tool;
  tool.name = std::move(name);
  tool.description = std::move(description);
  tool.inputSchema = std::move(inputSchema);
  tool.run = std::move(fn);
  return tool;
}

const std::vector<McpTool>& mcp_tools()
{
  static const std::vector<McpTool> tools = [] {
    std::vector<McpTool> list;
    register_canvas_draw_tools(list);
    register_layers_tools(list);
    register_animation_tools(list);
    register_palette_transform_tools(list);
    register_io_tools(list);
    register_misc_tools(list);
    return list;
  }();
  return tools;
}

} // namespace mcp
} // namespace app
