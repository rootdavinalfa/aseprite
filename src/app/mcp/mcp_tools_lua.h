// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_MCP_MCP_TOOLS_LUA_H_INCLUDED
#define APP_MCP_MCP_TOOLS_LUA_H_INCLUDED
#pragma once

#ifndef ENABLE_SCRIPTING
  #error ENABLE_SCRIPTING must be defined
#endif

namespace app {
namespace mcp {

// Resolves a layer by name, searching inside groups. An exact full-name
// match is tried first, breadth-first; otherwise the name is read as a
// "group/child" path whose segments are matched longest-prefix-first
// with backtracking, so names that themselves contain "/" still work.
// Requires a local "spr" in scope, defines the function find_layer().
inline const char kFindLayerLua[] =
  "local function find_layer(spr, name)\n"
  "  local queue = {}\n"
  "  for _, layer in ipairs(spr.layers) do queue[#queue + 1] = layer end\n"
  "  local head = 1\n"
  "  while head <= #queue do\n"
  "    local layer = queue[head]\n"
  "    head = head + 1\n"
  "    if layer.name == name then return layer end\n"
  "    if layer.isGroup then\n"
  "      for _, child in ipairs(layer.layers) do queue[#queue + 1] = child end\n"
  "    end\n"
  "  end\n"
  "  if not string.find(name, \"/\", 1, true) then return nil end\n"
  "  local segs = {}\n"
  "  for segment in string.gmatch(name, \"[^/]+\") do segs[#segs + 1] = segment end\n"
  "  local function resolve(layers, i)\n"
  "    for j = #segs, i, -1 do\n"
  "      local cand = table.concat(segs, \"/\", i, j)\n"
  "      for _, layer in ipairs(layers) do\n"
  "        if layer.name == cand then\n"
  "          if j == #segs then return layer end\n"
  "          if layer.isGroup then\n"
  "            local found = resolve(layer.layers, j + 1)\n"
  "            if found then return found end\n"
  "          end\n"
  "        end\n"
  "      end\n"
  "    end\n"
  "    return nil\n"
  "  end\n"
  "  return resolve(spr.layers, 1)\n"
  "end\n";

} // namespace mcp
} // namespace app

#endif
