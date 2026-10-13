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

void register_animation_tools(std::vector<McpTool>& tools)
{
  tools.push_back(LuaTool(
    "get_sprite_info",
    "Return sprite info as a JSON string (size, color mode, frame durations, layers, tags).",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to inspect" } } },
      } },
      { "required", json11::Json::array { "filename" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const std::string filename = argString(args, "filename");
      if (filename.empty())
        return "print(\"ERROR:File not found\")";

      return R"LUA(local spr = app.activeSprite
if not spr then error("No active sprite") end

local color_mode = "unknown"
if spr.colorMode == ColorMode.RGB then color_mode = "rgb"
elseif spr.colorMode == ColorMode.INDEXED then color_mode = "indexed"
elseif spr.colorMode == ColorMode.GRAYSCALE then color_mode = "gray"
elseif spr.colorMode == ColorMode.TILEMAP then color_mode = "tilemap" end

local dirs = { [0] = "forward", [1] = "reverse", [2] = "pingpong", [3] = "pingpong_reverse" }
local durations = {}
for _, frame in ipairs(spr.frames) do
  table.insert(durations, math.floor(frame.duration * 1000 + 0.5))
end

-- Recurses into groups so nested layers are enumerated too; each entry
-- carries its immediate parent group name (absent at the top level).
local layers = {}
local function walk_layers(list, parent)
  for _, layer in ipairs(list) do
    table.insert(layers, {
      name = layer.name,
      visible = layer.isVisible,
      opacity = layer.opacity or 255,
      is_group = layer.isGroup,
      parent = parent,
    })
    if layer.isGroup then walk_layers(layer.layers, layer.name) end
  end
end
walk_layers(spr.layers, nil)

local tags = {}
for _, tag in ipairs(spr.tags) do
  table.insert(tags, {
    name = tag.name,
    from = tag.fromFrame.frameNumber,
    to = tag.toFrame.frameNumber,
    direction = dirs[tag.aniDir] or tostring(tag.aniDir),
  })
end

print(json.encode({
  width = spr.width,
  height = spr.height,
  color_mode = color_mode,
  frames = #spr.frames,
  durations_ms = durations,
  layers = layers,
  tags = tags,
})) )LUA";
    }));
}

} // namespace mcp
} // namespace app
