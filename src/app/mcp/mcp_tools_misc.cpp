// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/mcp/mcp_tools_registry.h"

namespace app {
namespace mcp {

void register_misc_tools(std::vector<McpTool>& tools)
{
  tools.push_back(LuaTool(
    "run_lua_script",
    "Execute arbitrary Aseprite Lua code (escape hatch). When filename is given, the file is "
    "opened first as app.activeSprite. print() is the only way to return data; changes are not "
    "saved automatically (call spr:saveAs(spr.filename) if you modified the sprite). "
    "WARNING: runs unrestricted code on the host.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "script", json11::Json::object {
            { "type", "string" },
            { "description", "Lua source code to execute" } } },
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Optional Aseprite file to open before running" } } },
      } },
      { "required", json11::Json::array { "script" } },
    },
    true,
    [](const json11::Json& args) {
      const std::string script = argString(args, "script");
      if (script.empty())
        return std::string("print(\"ERROR:Script cannot be empty\")");
      return script;
    }));
}

} // namespace mcp
} // namespace app
