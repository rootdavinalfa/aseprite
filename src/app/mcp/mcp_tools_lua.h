// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.
//
// Portions derived from diivi/aseprite-mcp (MIT); see THIRD_PARTY_NOTICES.md.

#ifndef APP_MCP_MCP_TOOLS_LUA_H_INCLUDED
#define APP_MCP_MCP_TOOLS_LUA_H_INCLUDED
#pragma once

#ifndef ENABLE_SCRIPTING
  #error ENABLE_SCRIPTING must be defined
#endif

namespace app {
namespace mcp {

// Requires an active sprite, leaving it in a local "spr".
inline const char kSpritePreludeLua[] =
  "local spr = app.activeSprite\n"
  "if not spr then print(\"ERROR:No active sprite\") return end\n";

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

// Makes the cel of a layer/frame canvas-sized and anchored at (0,0),
// creating it on demand, so drawing code can use sprite-global
// coordinates directly.
inline const char kNormalizeCelLua[] =
  "local function normalize_cel(spr, layer, frame, create)\n"
  "  local cel = layer:cel(frame)\n"
  "  if not cel then\n"
  "    if not create then return nil end\n"
  "    local img = Image(spr.width, spr.height, spr.colorMode)\n"
  "    return spr:newCel(layer, frame, img, Point(0, 0))\n"
  "  end\n"
  "  if cel.position.x == 0 and cel.position.y == 0 and\n"
  "     cel.image.width == spr.width and cel.image.height == spr.height then\n"
  "    return cel\n"
  "  end\n"
  "  local img = Image(spr.width, spr.height, spr.colorMode)\n"
  "  img:drawImage(cel.image, cel.position)\n"
  "  cel.image = img\n"
  "  cel.position = Point(0, 0)\n"
  "  return cel\n"
  "end\n";

// putPixel that ignores out-of-bounds coordinates.
inline const char kPSetLua[] =
  "local function pset(img, x, y, color)\n"
  "  if x >= 0 and y >= 0 and x < img.width and y < img.height then\n"
  "    img:putPixel(x, y, color)\n"
  "  end\n"
  "end\n";

} // namespace mcp
} // namespace app

#endif
