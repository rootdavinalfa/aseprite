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

void register_canvas_draw_tools(std::vector<McpTool>& tools)
{
  tools.push_back(LuaTool(
    "create_canvas",
    "Create a new Aseprite canvas with the given dimensions and save it to filename.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "width", json11::Json::object { { "type", "integer" },
                                           { "description", "Canvas width in pixels" } } },
        { "height", json11::Json::object { { "type", "integer" },
                                            { "description", "Canvas height in pixels" } } },
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Output .aseprite file (default: canvas.aseprite)" } } },
      } },
      { "required", json11::Json::array { "width", "height" } },
    },
    false,
    [](const json11::Json& args) {
      const int width = argInt(args, "width");
      const int height = argInt(args, "height");
      if (width <= 0 || height <= 0)
        return std::string("print(\"ERROR:Width and height must be > 0\")");

      const std::string filename = argString(args, "filename", "canvas.aseprite");
      return "local spr = Sprite(" + std::to_string(width) + ", " + std::to_string(height) + ")\n"
             "spr:saveAs(\"" + luaString(filename) + "\")\n"
             "print(\"" + luaString(filename) + "\")";
    }));
}

} // namespace mcp
} // namespace app
