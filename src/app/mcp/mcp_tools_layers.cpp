// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.
//
// Portions derived from diivi/aseprite-mcp (MIT); see THIRD_PARTY_NOTICES.md.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/mcp/mcp_tools_lua.h"
#include "app/mcp/mcp_tools_registry.h"

#include <cctype>
#include <cstdio>
#include <string>

namespace app {
namespace mcp {

namespace {

// Blend modes accepted by set_layer_blend_mode, mapped to their Lua
// BlendMode constants.
struct BlendModeLua {
  const char* name;
  const char* lua;
};

const BlendModeLua kBlendModes[] = {
  { "normal", "BlendMode.NORMAL" },
  { "darken", "BlendMode.DARKEN" },
  { "multiply", "BlendMode.MULTIPLY" },
  { "color_burn", "BlendMode.COLOR_BURN" },
  { "lighten", "BlendMode.LIGHTEN" },
  { "screen", "BlendMode.SCREEN" },
  { "color_dodge", "BlendMode.COLOR_DODGE" },
  { "addition", "BlendMode.ADDITION" },
  { "overlay", "BlendMode.OVERLAY" },
  { "soft_light", "BlendMode.SOFT_LIGHT" },
  { "hard_light", "BlendMode.HARD_LIGHT" },
  { "difference", "BlendMode.DIFFERENCE" },
  { "exclusion", "BlendMode.EXCLUSION" },
  { "subtract", "BlendMode.SUBTRACT" },
  { "divide", "BlendMode.DIVIDE" },
  { "hue", "BlendMode.HSL_HUE" },
  { "saturation", "BlendMode.HSL_SATURATION" },
  { "color", "BlendMode.HSL_COLOR" },
  { "luminosity", "BlendMode.HSL_LUMINOSITY" },
};

// Bayer 4x4 ordered-dither matrix shared by the dither tools.
const char kBayerLua[] =
  "local bayer = {\n"
  "  {  0,  8,  2, 10 },\n"
  "  { 12,  4, 14,  6 },\n"
  "  {  3, 11,  1,  9 },\n"
  "  { 15,  7, 13,  5 },\n"
  "}\n";

// RGB <-> HSL conversion; h in [0,360), s and l in [0,1].
const char kHslLua[] =
  "local function rgb_to_hsl(r, g, b)\n"
  "  r, g, b = r / 255, g / 255, b / 255\n"
  "  local maxc = math.max(r, g, b)\n"
  "  local minc = math.min(r, g, b)\n"
  "  local l = (maxc + minc) / 2\n"
  "  if maxc == minc then return 0, 0, l end\n"
  "  local d = maxc - minc\n"
  "  local s\n"
  "  if l > 0.5 then s = d / (2 - maxc - minc) else s = d / (maxc + minc) end\n"
  "  local h\n"
  "  if maxc == r then\n"
  "    h = (g - b) / d\n"
  "    if g < b then h = h + 6 end\n"
  "  elseif maxc == g then\n"
  "    h = (b - r) / d + 2\n"
  "  else\n"
  "    h = (r - g) / d + 4\n"
  "  end\n"
  "  return h * 60, s, l\n"
  "end\n"
  "\n"
  "local function hsl_to_rgb(h, s, l)\n"
  "  h = h % 360\n"
  "  if s <= 0 then\n"
  "    local v = math.floor(l * 255 + 0.5)\n"
  "    return v, v, v\n"
  "  end\n"
  "  local c = (1 - math.abs(2 * l - 1)) * s\n"
  "  local hp = h / 60\n"
  "  local x = c * (1 - math.abs(hp % 2 - 1))\n"
  "  local r1, g1, b1 = 0, 0, 0\n"
  "  if hp < 1 then r1, g1, b1 = c, x, 0\n"
  "  elseif hp < 2 then r1, g1, b1 = x, c, 0\n"
  "  elseif hp < 3 then r1, g1, b1 = 0, c, x\n"
  "  elseif hp < 4 then r1, g1, b1 = 0, x, c\n"
  "  elseif hp < 5 then r1, g1, b1 = x, 0, c\n"
  "  else r1, g1, b1 = c, 0, x end\n"
  "  local m = l - c / 2\n"
  "  return math.floor((r1 + m) * 255 + 0.5),\n"
  "         math.floor((g1 + m) * 255 + 0.5),\n"
  "         math.floor((b1 + m) * 255 + 0.5)\n"
  "end\n";

// Saves the file and reports success; appended after a transaction's
// "end)" or as the tail of a script that manages its own undo step.
const char kSaveOk[] =
  "\n"
  "spr:saveAs(spr.filename)\n"
  "print(\"OK\")";

// Formats a value as a Lua literal, keeping a decimal point so Lua
// reads it as a float.
std::string luaNumber(const double value)
{
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%.6g", value);
  std::string out = buf;
  if (out.find_first_of(".eE") == std::string::npos)
    out += ".0";
  return out;
}

// Checks a 1-based frame index carried in the local "idx".
std::string frameIndexCheck(const int frameIndex)
{
  return "local idx = " + std::to_string(frameIndex) + "\n"
         "if idx < 1 or idx > #spr.frames then print(\"ERROR:Frame index out of range\") return end\n";
}

// Starts a script that opens a transaction on the cel of the named
// layer and 1-based frame, in cel-local coordinates: the body uses
// "img" bound to cel.image. "before" is emitted before the transaction
// (e.g. to declare counters).
std::string celEditStart(const std::string& layerName,
                         const int frameIndex,
                         const std::string& before = {})
{
  return frameIndexCheck(frameIndex) +
         kFindLayerLua +
         "local target = find_layer(spr, \"" + luaString(layerName) + "\")\n"
         "if not target then print(\"ERROR:Layer not found\") return end\n"
         "\n"
         "local cel = target:cel(spr.frames[idx])\n"
         "if not cel then print(\"ERROR:No cel at that layer/frame\") return end\n"
         "\n" +
         before +
         "app.transaction(function()\n"
         "  local img = cel.image\n";
}

// Like celEditStart(), but the cel is normalized to canvas-sized and
// anchored at (0,0) first (optionally creating it), so the body uses
// sprite-global x/y directly.
std::string layerEditStart(const std::string& layerName,
                           const int frameIndex,
                           const bool createIfMissing)
{
  return frameIndexCheck(frameIndex) +
         kFindLayerLua +
         kNormalizeCelLua +
         "local target = find_layer(spr, \"" + luaString(layerName) + "\")\n"
         "if not target then print(\"ERROR:Layer not found\") return end\n"
         "\n"
         "app.transaction(function()\n"
         "  local cel = normalize_cel(spr, target, spr.frames[idx], " +
         (createIfMissing ? "true" : "false") + ")\n"
         "  if not cel then print(\"ERROR:No cel at that layer/frame\") return end\n"
         "  local img = cel.image\n";
}

// Closes the transaction opened by celEditStart()/layerEditStart().
std::string okEnd()
{
  return "end)\n" + std::string(kSaveOk);
}

// Like okEnd(), but reports the pixel count accumulated in "count".
std::string countEnd()
{
  return "end)\n"
         "\n"
         "spr:saveAs(spr.filename)\n"
         "print(\"COUNT:\" .. count)";
}

} // anonymous namespace

void register_layers_tools(std::vector<McpTool>& tools)
{
  // Layers.

  tools.push_back(LuaTool(
    "delete_layer",
    "Delete a layer and its cels by name, searching inside groups too. A sprite must keep "
    "at least one layer. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to delete, by name or group/child path" } } },
      } },
      { "required", json11::Json::array { "filename", "layer_name" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      std::string script = kSpritePreludeLua;
      script += kFindLayerLua;
      script += "local target = find_layer(spr, \"" + luaString(argString(args, "layer_name")) +
                "\")\n"
                "if not target then print(\"ERROR:Layer not found\") return end\n"
                "if #spr.layers <= 1 then print(\"ERROR:Cannot delete the only layer\") return end\n"
                "\n"
                "app.transaction(function()\n"
                "  spr:deleteLayer(target)\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  tools.push_back(LuaTool(
    "rename_layer",
    "Rename a layer found by name or group/child path. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Current layer name, or group/child path" } } },
        { "new_name", json11::Json::object {
            { "type", "string" },
            { "description", "New layer name (cannot be empty)" } } },
      } },
      { "required", json11::Json::array { "filename", "layer_name", "new_name" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      if (argString(args, "new_name").empty())
        return std::string("print(\"ERROR:New name cannot be empty\")");

      std::string script = kSpritePreludeLua;
      script += kFindLayerLua;
      script += "local target = find_layer(spr, \"" + luaString(argString(args, "layer_name")) +
                "\")\n"
                "if not target then print(\"ERROR:Layer not found\") return end\n"
                "\n"
                "app.transaction(function()\n"
                "  target.name = \"" + luaString(argString(args, "new_name")) + "\"\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  tools.push_back(LuaTool(
    "duplicate_layer",
    "Duplicate a layer with all its cels, copying the source's opacity and blend mode. The "
    "copy goes directly above the source unless group places it inside a group. The file "
    "is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to duplicate, by name or group/child path" } } },
        { "new_name", json11::Json::object {
            { "type", "string" },
            { "description", "Name for the copy (default: \"<layer_name> copy\")" } } },
        { "group", json11::Json::object {
            { "type", "string" },
            { "description", "Optional group to place the copy inside, by name or "
                             "group/subgroup path (default: directly above the source)" } } },
      } },
      { "required", json11::Json::array { "filename", "layer_name" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const std::string layerName = argString(args, "layer_name");
      const std::string newName = argString(args, "new_name");
      const std::string group = argString(args, "group");

      std::string script = kSpritePreludeLua;
      script += kFindLayerLua;
      script += "local src = find_layer(spr, \"" + luaString(layerName) + "\")\n"
                "if not src then print(\"ERROR:Layer not found\") return end\n"
                "\n"
                "local parent = nil\n"
                "if \"" + luaString(group) + "\" ~= \"\" then\n"
                "  parent = find_layer(spr, \"" + luaString(group) + "\")\n"
                "  if not parent then print(\"ERROR:Group not found\") return end\n"
                "  if not parent.isGroup then print(\"ERROR:Target is not a group\") return end\n"
                "end\n"
                "\n"
                "app.transaction(function()\n"
                "  local copy = spr:newLayer()\n"
                "  copy.name = \"" + luaString(newName.empty() ? layerName + " copy" : newName) +
                "\"\n"
                "  copy.opacity = src.opacity\n"
                "  copy.blendMode = src.blendMode\n"
                "  if parent then\n"
                "    copy.parent = parent\n"
                "  else\n"
                "    copy.stackIndex = src.stackIndex + 1\n"
                "  end\n"
                "  for _, frame in ipairs(spr.frames) do\n"
                "    local cel = src:cel(frame)\n"
                "    if cel then\n"
                "      local c = spr:newCel(copy, frame, cel.image:clone(), cel.position)\n"
                "      c.opacity = cel.opacity\n"
                "    end\n"
                "  end\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  tools.push_back(LuaTool(
    "reorder_layer",
    "Move a layer to another position in its stack. Positions are 1-based, where 1 is the "
    "bottom layer. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to move, by name or group/child path" } } },
        { "position", json11::Json::object {
            { "type", "integer" },
            { "description", "Target stack position, 1-based (1 is the bottom layer)" } } },
      } },
      { "required", json11::Json::array { "filename", "layer_name", "position" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int position = argInt(args, "position");
      if (position < 1)
        return std::string("print(\"ERROR:Position must be >= 1\")");

      std::string script = kSpritePreludeLua;
      script += kFindLayerLua;
      script += "local target = find_layer(spr, \"" + luaString(argString(args, "layer_name")) +
                "\")\n"
                "if not target then print(\"ERROR:Layer not found\") return end\n"
                "if " + std::to_string(position) + " > #spr.layers then\n"
                "  print(\"ERROR:Position out of range\") return\n"
                "end\n"
                "\n"
                "app.transaction(function()\n"
                "  target.stackIndex = " + std::to_string(position) + "\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  tools.push_back(LuaTool(
    "set_layer_blend_mode",
    "Set the blend mode of a layer found by name or group/child path. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to modify, by name or group/child path" } } },
        { "mode", json11::Json::object {
            { "type", "string" },
            { "description", "Blend mode: normal, darken, multiply, color_burn, lighten, "
                             "screen, color_dodge, addition, overlay, soft_light, hard_light, "
                             "difference, exclusion, subtract, divide, hue, saturation, "
                             "color, or luminosity" } } },
      } },
      { "required", json11::Json::array { "filename", "layer_name", "mode" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      std::string mode = argString(args, "mode");
      for (char& c : mode)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

      const char* blend = nullptr;
      for (const BlendModeLua& entry : kBlendModes) {
        if (mode == entry.name) {
          blend = entry.lua;
          break;
        }
      }
      if (!blend)
        return std::string("print(\"ERROR:Unknown blend mode: " +
                           luaString(argString(args, "mode")) + "\")");

      std::string script = kSpritePreludeLua;
      script += kFindLayerLua;
      script += "local target = find_layer(spr, \"" + luaString(argString(args, "layer_name")) +
                "\")\n"
                "if not target then print(\"ERROR:Layer not found\") return end\n"
                "\n"
                "app.transaction(function()\n"
                "  target.blendMode = " + blend + "\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  tools.push_back(LuaTool(
    "merge_layer_down",
    "Merge a layer into the layer directly below it. The layer must not be the bottom "
    "layer. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to merge down, by name or group/child path" } } },
      } },
      { "required", json11::Json::array { "filename", "layer_name" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      std::string script = kSpritePreludeLua;
      script += kFindLayerLua;
      script += "local target = find_layer(spr, \"" + luaString(argString(args, "layer_name")) +
                "\")\n"
                "if not target then print(\"ERROR:Layer not found\") return end\n"
                "if target.stackIndex <= 1 then\n"
                "  print(\"ERROR:Layer is the bottom layer; nothing to merge into\") return\n"
                "end\n"
                "\n"
                "app.activeLayer = target\n"
                "app.command.MergeDownLayer()\n";
      script += kSaveOk;
      return script;
    }));

  tools.push_back(LuaTool(
    "flatten_sprite",
    "Flatten every layer of the sprite into a single layer. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
      } },
      { "required", json11::Json::array { "filename" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      return std::string(kSpritePreludeLua) +
             "app.transaction(function()\n"
             "  spr:flatten()\n"
             "end)\n" + kSaveOk;
    }));

  // Selection.

  tools.push_back(LuaTool(
    "move_region",
    "Cut a rectangle of pixels and paste it at a new position. Coordinates are sprite-global; "
    "the source area becomes transparent and pixels pasted outside the canvas are dropped. "
    "Transparent source pixels do not overwrite the destination. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to operate on, by name or group/child path" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "x", json11::Json::object { { "type", "integer" },
                                      { "description", "Left edge of the source region" } } },
        { "y", json11::Json::object { { "type", "integer" },
                                      { "description", "Top edge of the source region" } } },
        { "width", json11::Json::object {
            { "type", "integer" },
            { "description", "Region width in pixels (must be > 0)" } } },
        { "height", json11::Json::object {
            { "type", "integer" },
            { "description", "Region height in pixels (must be > 0)" } } },
        { "dest_x", json11::Json::object {
            { "type", "integer" },
            { "description", "Left edge of the new region position" } } },
        { "dest_y", json11::Json::object {
            { "type", "integer" },
            { "description", "Top edge of the new region position" } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "frame_index", "x", "y", "width", "height",
        "dest_x", "dest_y" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int width = argInt(args, "width");
      const int height = argInt(args, "height");
      if (width <= 0 || height <= 0)
        return std::string("print(\"ERROR:Width and height must be > 0\")");

      const int x = argInt(args, "x");
      const int y = argInt(args, "y");
      const int destX = argInt(args, "dest_x");
      const int destY = argInt(args, "dest_y");

      return std::string(kSpritePreludeLua) +
             kPSetLua +
             layerEditStart(argString(args, "layer_name"), argInt(args, "frame_index"), false) +
             "  local buf = {}\n"
             "  for ry = 0, " + std::to_string(height) + " - 1 do\n"
             "    buf[ry] = {}\n"
             "    for rx = 0, " + std::to_string(width) + " - 1 do\n"
             "      local sx = " + std::to_string(x) + " + rx\n"
             "      local sy = " + std::to_string(y) + " + ry\n"
             "      if sx >= 0 and sy >= 0 and sx < img.width and sy < img.height then\n"
             "        buf[ry][rx] = img:getPixel(sx, sy)\n"
             "        img:putPixel(sx, sy, 0)\n"
             "      else\n"
             "        buf[ry][rx] = 0\n"
             "      end\n"
             "    end\n"
             "  end\n"
             "\n"
             "  for ry = 0, " + std::to_string(height) + " - 1 do\n"
             "    for rx = 0, " + std::to_string(width) + " - 1 do\n"
             "      local v = buf[ry][rx]\n"
             "      if app.pixelColor.rgbaA(v) > 0 then\n"
             "        pset(img, " + std::to_string(destX) + " + rx, " + std::to_string(destY) +
             " + ry, v)\n"
             "      end\n"
             "    end\n"
             "  end\n" + okEnd();
    }));

  tools.push_back(LuaTool(
    "copy_region",
    "Copy a rectangle of pixels to another position, layer, or frame. Coordinates are "
    "sprite-global; transparent source pixels do not overwrite the destination, and the "
    "destination cel is created when missing. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Source layer, by name or group/child path" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Source frame index starting at 1" } } },
        { "x", json11::Json::object { { "type", "integer" },
                                      { "description", "Left edge of the source region" } } },
        { "y", json11::Json::object { { "type", "integer" },
                                      { "description", "Top edge of the source region" } } },
        { "width", json11::Json::object {
            { "type", "integer" },
            { "description", "Region width in pixels (must be > 0)" } } },
        { "height", json11::Json::object {
            { "type", "integer" },
            { "description", "Region height in pixels (must be > 0)" } } },
        { "dest_x", json11::Json::object {
            { "type", "integer" },
            { "description", "Left edge of the destination" } } },
        { "dest_y", json11::Json::object {
            { "type", "integer" },
            { "description", "Top edge of the destination" } } },
        { "target_layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Destination layer (default: same as the source)" } } },
        { "target_frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Destination frame index starting at 1 (default: same as the "
                             "source)" } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "frame_index", "x", "y", "width", "height",
        "dest_x", "dest_y" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int width = argInt(args, "width");
      const int height = argInt(args, "height");
      if (width <= 0 || height <= 0)
        return std::string("print(\"ERROR:Width and height must be > 0\")");

      const int x = argInt(args, "x");
      const int y = argInt(args, "y");
      const int destX = argInt(args, "dest_x");
      const int destY = argInt(args, "dest_y");
      const std::string srcLayerName = argString(args, "layer_name");
      const std::string targetLayer = argString(args, "target_layer_name");
      const std::string dstLayerName = targetLayer.empty() ? srcLayerName : targetLayer;
      const int frameIndex = argInt(args, "frame_index");
      const int targetFrameArg = argInt(args, "target_frame_index", 0);
      const int dstFrameIndex = targetFrameArg >= 1 ? targetFrameArg : frameIndex;

      std::string script = kSpritePreludeLua;
      script += kFindLayerLua;
      script += kNormalizeCelLua;
      script += kPSetLua;
      script += "local src_idx = " + std::to_string(frameIndex) + "\n"
                "local dst_idx = " + std::to_string(dstFrameIndex) + "\n"
                "if src_idx < 1 or src_idx > #spr.frames then\n"
                "  print(\"ERROR:Source frame index out of range\") return\n"
                "end\n"
                "if dst_idx < 1 or dst_idx > #spr.frames then\n"
                "  print(\"ERROR:Target frame index out of range\") return\n"
                "end\n"
                "\n"
                "local src_layer = find_layer(spr, \"" + luaString(srcLayerName) + "\")\n"
                "if not src_layer then print(\"ERROR:Source layer not found\") return end\n"
                "local dst_layer = find_layer(spr, \"" + luaString(dstLayerName) + "\")\n"
                "if not dst_layer then print(\"ERROR:Target layer not found\") return end\n"
                "\n"
                "app.transaction(function()\n"
                "  local src_cel = normalize_cel(spr, src_layer, spr.frames[src_idx], false)\n"
                "  if not src_cel then print(\"ERROR:No cel at source layer/frame\") return end\n"
                "  local src_img = src_cel.image\n"
                "\n"
                "  local buf = {}\n"
                "  for ry = 0, " + std::to_string(height) + " - 1 do\n"
                "    buf[ry] = {}\n"
                "    for rx = 0, " + std::to_string(width) + " - 1 do\n"
                "      local sx = " + std::to_string(x) + " + rx\n"
                "      local sy = " + std::to_string(y) + " + ry\n"
                "      if sx >= 0 and sy >= 0 and sx < src_img.width and sy < src_img.height then\n"
                "        buf[ry][rx] = src_img:getPixel(sx, sy)\n"
                "      else\n"
                "        buf[ry][rx] = 0\n"
                "      end\n"
                "    end\n"
                "  end\n"
                "\n"
                "  local dst_cel = normalize_cel(spr, dst_layer, spr.frames[dst_idx], true)\n"
                "  local dst_img = dst_cel.image\n"
                "  for ry = 0, " + std::to_string(height) + " - 1 do\n"
                "    for rx = 0, " + std::to_string(width) + " - 1 do\n"
                "      local v = buf[ry][rx]\n"
                "      if app.pixelColor.rgbaA(v) > 0 then\n"
                "        pset(dst_img, " + std::to_string(destX) + " + rx, " +
                std::to_string(destY) + " + ry, v)\n"
                "      end\n"
                "    end\n"
                "  end\n" + okEnd();
      return script;
    }));

  tools.push_back(LuaTool(
    "erase_region",
    "Erase a rectangle of pixels, making it transparent. Coordinates are sprite-global. "
    "The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to operate on, by name or group/child path" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "x", json11::Json::object { { "type", "integer" },
                                      { "description", "Left edge of the region" } } },
        { "y", json11::Json::object { { "type", "integer" },
                                      { "description", "Top edge of the region" } } },
        { "width", json11::Json::object {
            { "type", "integer" },
            { "description", "Region width in pixels (must be > 0)" } } },
        { "height", json11::Json::object {
            { "type", "integer" },
            { "description", "Region height in pixels (must be > 0)" } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "frame_index", "x", "y", "width", "height" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int width = argInt(args, "width");
      const int height = argInt(args, "height");
      if (width <= 0 || height <= 0)
        return std::string("print(\"ERROR:Width and height must be > 0\")");

      const int x = argInt(args, "x");
      const int y = argInt(args, "y");

      return std::string(kSpritePreludeLua) +
             layerEditStart(argString(args, "layer_name"), argInt(args, "frame_index"), false) +
             "  for py = math.max(0, " + std::to_string(y) + "), math.min(img.height - 1, " +
             std::to_string(y + height - 1) + ") do\n"
             "    for px = math.max(0, " + std::to_string(x) + "), math.min(img.width - 1, " +
             std::to_string(x + width - 1) + ") do\n"
             "      img:putPixel(px, py, 0)\n"
             "    end\n"
             "  end\n" + okEnd();
    }));

  tools.push_back(LuaTool(
    "erase_color",
    "Make every pixel of a color transparent, like a magic eraser. Antialiased or partially "
    "transparent pixels are ignored, and the tolerance is per RGB channel. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to operate on, by name or group/child path" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "color", json11::Json::object {
            { "type", "string" },
            { "description", "Hex color to erase, e.g. #FF00FF" } } },
        { "tolerance", json11::Json::object {
            { "type", "integer" },
            { "description", "Per-channel tolerance 0-255 (default: 0 = exact match)" } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "frame_index", "color" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      int r = 0, g = 0, b = 0, a = 0;
      if (!parseHexColor(argString(args, "color"), r, g, b, a))
        return std::string("print(\"ERROR:Invalid color value: " +
                           luaString(argString(args, "color")) + "\")");

      const int tolerance = argInt(args, "tolerance", 0);
      if (tolerance < 0 || tolerance > 255)
        return std::string("print(\"ERROR:Tolerance must be between 0 and 255\")");

      return std::string(kSpritePreludeLua) +
             celEditStart(argString(args, "layer_name"),
                          argInt(args, "frame_index"),
                          "local count = 0\n") +
             "  for py = 0, img.height - 1 do\n"
             "    for px = 0, img.width - 1 do\n"
             "      local v = img:getPixel(px, py)\n"
             "      if app.pixelColor.rgbaA(v) > 0 then\n"
             "        local dr = math.abs(app.pixelColor.rgbaR(v) - " + std::to_string(r) + ")\n"
             "        local dg = math.abs(app.pixelColor.rgbaG(v) - " + std::to_string(g) + ")\n"
             "        local db = math.abs(app.pixelColor.rgbaB(v) - " + std::to_string(b) + ")\n"
             "        if dr <= " + std::to_string(tolerance) + " and dg <= " +
             std::to_string(tolerance) + " and db <= " + std::to_string(tolerance) + " then\n"
             "          img:putPixel(px, py, 0)\n"
             "          count = count + 1\n"
             "        end\n"
             "      end\n"
             "    end\n"
             "  end\n" + countEnd();
    }));

  // Effects.

  tools.push_back(LuaTool(
    "outline_cel",
    "Add a 1px outline around the opaque pixels of a cel, filling transparent pixels that "
    "touch an opaque one with the outline color. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to outline, by name or group/child path" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "color", json11::Json::object {
            { "type", "string" },
            { "description", "Outline hex color (default: #000000)" } } },
        { "include_diagonals", json11::Json::object {
            { "type", "boolean" },
            { "description", "Also outline diagonal neighbors for a thicker, rounded look "
                             "(default: false)" } } },
      } },
      { "required", json11::Json::array { "filename", "layer_name", "frame_index" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      int r = 0, g = 0, b = 0, a = 0;
      if (!parseHexColor(argString(args, "color", "#000000"), r, g, b, a))
        return std::string("print(\"ERROR:Invalid color value: " +
                           luaString(argString(args, "color")) + "\")");

      const bool diagonals = argBool(args, "include_diagonals", false);

      std::string script = kSpritePreludeLua;
      script += layerEditStart(argString(args, "layer_name"), argInt(args, "frame_index"), false);
      script += "  local src = img:clone()\n"
                "  local outline = app.pixelColor.rgba(" + std::to_string(r) + ", " +
                std::to_string(g) + ", " + std::to_string(b) + ", 255)\n"
                "\n"
                "  local function opaque(x, y)\n"
                "    if x < 0 or y < 0 or x >= src.width or y >= src.height then return false end\n"
                "    return app.pixelColor.rgbaA(src:getPixel(x, y)) > 0\n"
                "  end\n"
                "\n"
                "  for py = 0, img.height - 1 do\n"
                "    for px = 0, img.width - 1 do\n"
                "      if app.pixelColor.rgbaA(src:getPixel(px, py)) == 0 then\n"
                "        local touch = opaque(px - 1, py) or opaque(px + 1, py) or\n"
                "                      opaque(px, py - 1) or opaque(px, py + 1)\n" +
                (diagonals ?
                   "        if not touch then\n"
                   "          touch = opaque(px - 1, py - 1) or opaque(px + 1, py - 1) or\n"
                   "                  opaque(px - 1, py + 1) or opaque(px + 1, py + 1)\n"
                   "        end\n" :
                   std::string()) +
                "        if touch then\n"
                "          img:putPixel(px, py, outline)\n"
                "        end\n"
                "      end\n"
                "    end\n"
                "  end\n" + okEnd();
      return script;
    }));

  tools.push_back(LuaTool(
    "replace_color",
    "Replace one color with another in a cel, keeping each pixel's alpha. The tolerance is "
    "per RGB channel. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to operate on, by name or group/child path" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "from_color", json11::Json::object {
            { "type", "string" },
            { "description", "Hex color to replace, e.g. #FF0000" } } },
        { "to_color", json11::Json::object {
            { "type", "string" },
            { "description", "Replacement hex color" } } },
        { "tolerance", json11::Json::object {
            { "type", "integer" },
            { "description", "Per-channel tolerance 0-255 (default: 0 = exact match)" } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "frame_index", "from_color", "to_color" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      int sr = 0, sg = 0, sb = 0, sa = 0;
      int dr = 0, dg = 0, db = 0, da = 0;
      if (!parseHexColor(argString(args, "from_color"), sr, sg, sb, sa) ||
          !parseHexColor(argString(args, "to_color"), dr, dg, db, da))
        return std::string("print(\"ERROR:Colors must use #RRGGBB values\")");

      const int tolerance = argInt(args, "tolerance", 0);
      if (tolerance < 0 || tolerance > 255)
        return std::string("print(\"ERROR:Tolerance must be between 0 and 255\")");

      return std::string(kSpritePreludeLua) +
             celEditStart(argString(args, "layer_name"),
                          argInt(args, "frame_index"),
                          "local count = 0\n") +
             "  for py = 0, img.height - 1 do\n"
             "    for px = 0, img.width - 1 do\n"
             "      local v = img:getPixel(px, py)\n"
             "      local a = app.pixelColor.rgbaA(v)\n"
             "      if a > 0 then\n"
             "        local cr = math.abs(app.pixelColor.rgbaR(v) - " + std::to_string(sr) + ")\n"
             "        local cg = math.abs(app.pixelColor.rgbaG(v) - " + std::to_string(sg) + ")\n"
             "        local cb = math.abs(app.pixelColor.rgbaB(v) - " + std::to_string(sb) + ")\n"
             "        if cr <= " + std::to_string(tolerance) + " and cg <= " +
             std::to_string(tolerance) + " and cb <= " + std::to_string(tolerance) + " then\n"
             "          img:putPixel(px, py, app.pixelColor.rgba(" + std::to_string(dr) + ", " +
             std::to_string(dg) + ", " + std::to_string(db) + ", a))\n"
             "          count = count + 1\n"
             "        end\n"
             "      end\n"
             "    end\n"
             "  end\n" + countEnd();
    }));

  tools.push_back(LuaTool(
    "adjust_hsl",
    "Shift the hue, saturation, and lightness of the opaque pixels of a cel. Handy for "
    "palette swaps and shading. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to adjust, by name or group/child path" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "hue_shift", json11::Json::object {
            { "type", "number" },
            { "description", "Degrees to rotate the hue, -360 to 360 (default: 0)" } } },
        { "saturation_shift", json11::Json::object {
            { "type", "number" },
            { "description", "Saturation delta, -100 to 100 (default: 0)" } } },
        { "lightness_shift", json11::Json::object {
            { "type", "number" },
            { "description", "Lightness delta, -100 to 100 (default: 0)" } } },
      } },
      { "required", json11::Json::array { "filename", "layer_name", "frame_index" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const double hue = argDouble(args, "hue_shift", 0.0);
      const double saturation = argDouble(args, "saturation_shift", 0.0);
      const double lightness = argDouble(args, "lightness_shift", 0.0);
      if (hue < -360 || hue > 360)
        return std::string("print(\"ERROR:hue_shift must be between -360 and 360\")");
      if (saturation < -100 || saturation > 100)
        return std::string("print(\"ERROR:saturation_shift must be between -100 and 100\")");
      if (lightness < -100 || lightness > 100)
        return std::string("print(\"ERROR:lightness_shift must be between -100 and 100\")");

      std::string script = kSpritePreludeLua;
      script += kHslLua;
      script += celEditStart(argString(args, "layer_name"), argInt(args, "frame_index"));
      script += "  for py = 0, img.height - 1 do\n"
                "    for px = 0, img.width - 1 do\n"
                "      local v = img:getPixel(px, py)\n"
                "      local a = app.pixelColor.rgbaA(v)\n"
                "      if a > 0 then\n"
                "        local h, s, l = rgb_to_hsl(app.pixelColor.rgbaR(v),\n"
                "                                   app.pixelColor.rgbaG(v),\n"
                "                                   app.pixelColor.rgbaB(v))\n"
                "        h = h + (" + luaNumber(hue) + ")\n"
                "        s = math.min(1, math.max(0, s + (" + luaNumber(saturation) + ") / 100))\n"
                "        l = math.min(1, math.max(0, l + (" + luaNumber(lightness) + ") / 100))\n"
                "        local nr, ng, nb = hsl_to_rgb(h, s, l)\n"
                "        img:putPixel(px, py, app.pixelColor.rgba(nr, ng, nb, a))\n"
                "      end\n"
                "    end\n"
                "  end\n" + okEnd();
      return script;
    }));

  tools.push_back(LuaTool(
    "apply_dither_gradient",
    "Fill a rectangle with a two-color gradient blended by Bayer 4x4 ordered dithering, so "
    "no intermediate colors are introduced. The gradient runs from color_start at the "
    "top/left to color_end at the bottom/right. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to draw on, by name or group/child path" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "x", json11::Json::object { { "type", "integer" },
                                      { "description", "Left edge of the rectangle" } } },
        { "y", json11::Json::object { { "type", "integer" },
                                      { "description", "Top edge of the rectangle" } } },
        { "width", json11::Json::object {
            { "type", "integer" },
            { "description", "Rectangle width in pixels (must be > 0)" } } },
        { "height", json11::Json::object {
            { "type", "integer" },
            { "description", "Rectangle height in pixels (must be > 0)" } } },
        { "color_start", json11::Json::object {
            { "type", "string" },
            { "description", "Hex color at the start of the gradient" } } },
        { "color_end", json11::Json::object {
            { "type", "string" },
            { "description", "Hex color at the end of the gradient" } } },
        { "horizontal", json11::Json::object {
            { "type", "boolean" },
            { "description", "Run the gradient left-to-right instead of top-to-bottom "
                             "(default: false)" } } },
        { "create_if_missing", json11::Json::object {
            { "type", "boolean" },
            { "description", "Create the cel when it does not exist (default: true)" } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "frame_index", "x", "y", "width", "height",
        "color_start", "color_end" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int width = argInt(args, "width");
      const int height = argInt(args, "height");
      if (width <= 0 || height <= 0)
        return std::string("print(\"ERROR:Width and height must be > 0\")");

      int r1 = 0, g1 = 0, b1 = 0, a1 = 0;
      int r2 = 0, g2 = 0, b2 = 0, a2 = 0;
      if (!parseHexColor(argString(args, "color_start"), r1, g1, b1, a1) ||
          !parseHexColor(argString(args, "color_end"), r2, g2, b2, a2))
        return std::string("print(\"ERROR:Colors must use #RRGGBB values\")");

      const int x = argInt(args, "x");
      const int y = argInt(args, "y");
      const bool horizontal = argBool(args, "horizontal", false);
      const std::string axis = horizontal ? ("px - " + std::to_string(x))
                                          : ("py - " + std::to_string(y));
      const int span = horizontal ? width : height;

      return std::string(kSpritePreludeLua) +
             kPSetLua +
             kBayerLua +
             layerEditStart(argString(args, "layer_name"),
                            argInt(args, "frame_index"),
                            argBool(args, "create_if_missing", true)) +
             "  local c1 = app.pixelColor.rgba(" + std::to_string(r1) + ", " +
             std::to_string(g1) + ", " + std::to_string(b1) + ", 255)\n"
             "  local c2 = app.pixelColor.rgba(" + std::to_string(r2) + ", " +
             std::to_string(g2) + ", " + std::to_string(b2) + ", 255)\n"
             "  for py = " + std::to_string(y) + ", " + std::to_string(y + height - 1) + " do\n"
             "    for px = " + std::to_string(x) + ", " + std::to_string(x + width - 1) + " do\n"
             "      local f = (" + axis + ") / math.max(1, " + std::to_string(span) + " - 1)\n"
             "      local threshold = (bayer[(py % 4) + 1][(px % 4) + 1] + 0.5) / 16\n"
             "      if f >= threshold then\n"
             "        pset(img, px, py, c2)\n"
             "      else\n"
             "        pset(img, px, py, c1)\n"
             "      end\n"
             "    end\n"
             "  end\n" + okEnd();
    }));

  tools.push_back(LuaTool(
    "apply_dither_pattern",
    "Fill a rectangle with a uniform Bayer 4x4 dither mix of two colors. Density is the "
    "fraction of color_b: 0 is all color_a, 0.5 a checkerboard, 1 all color_b. The file "
    "is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to draw on, by name or group/child path" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "x", json11::Json::object { { "type", "integer" },
                                      { "description", "Left edge of the rectangle" } } },
        { "y", json11::Json::object { { "type", "integer" },
                                      { "description", "Top edge of the rectangle" } } },
        { "width", json11::Json::object {
            { "type", "integer" },
            { "description", "Rectangle width in pixels (must be > 0)" } } },
        { "height", json11::Json::object {
            { "type", "integer" },
            { "description", "Rectangle height in pixels (must be > 0)" } } },
        { "color_a", json11::Json::object {
            { "type", "string" },
            { "description", "Base hex color" } } },
        { "color_b", json11::Json::object {
            { "type", "string" },
            { "description", "Mixed-in hex color" } } },
        { "density", json11::Json::object {
            { "type", "number" },
            { "description", "Fraction of color_b, 0.0-1.0 (default: 0.5)" } } },
        { "create_if_missing", json11::Json::object {
            { "type", "boolean" },
            { "description", "Create the cel when it does not exist (default: true)" } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "frame_index", "x", "y", "width", "height",
        "color_a", "color_b" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int width = argInt(args, "width");
      const int height = argInt(args, "height");
      if (width <= 0 || height <= 0)
        return std::string("print(\"ERROR:Width and height must be > 0\")");

      const double density = argDouble(args, "density", 0.5);
      if (density < 0 || density > 1)
        return std::string("print(\"ERROR:density must be between 0.0 and 1.0\")");

      int r1 = 0, g1 = 0, b1 = 0, a1 = 0;
      int r2 = 0, g2 = 0, b2 = 0, a2 = 0;
      if (!parseHexColor(argString(args, "color_a"), r1, g1, b1, a1) ||
          !parseHexColor(argString(args, "color_b"), r2, g2, b2, a2))
        return std::string("print(\"ERROR:Colors must use #RRGGBB values\")");

      const int x = argInt(args, "x");
      const int y = argInt(args, "y");

      return std::string(kSpritePreludeLua) +
             kPSetLua +
             kBayerLua +
             layerEditStart(argString(args, "layer_name"),
                            argInt(args, "frame_index"),
                            argBool(args, "create_if_missing", true)) +
             "  local ca = app.pixelColor.rgba(" + std::to_string(r1) + ", " +
             std::to_string(g1) + ", " + std::to_string(b1) + ", 255)\n"
             "  local cb = app.pixelColor.rgba(" + std::to_string(r2) + ", " +
             std::to_string(g2) + ", " + std::to_string(b2) + ", 255)\n"
             "  for py = " + std::to_string(y) + ", " + std::to_string(y + height - 1) + " do\n"
             "    for px = " + std::to_string(x) + ", " + std::to_string(x + width - 1) + " do\n"
             "      local threshold = (bayer[(py % 4) + 1][(px % 4) + 1] + 0.5) / 16\n"
             "      if " + luaNumber(density) + " > threshold then\n"
             "        pset(img, px, py, cb)\n"
             "      else\n"
             "        pset(img, px, py, ca)\n"
             "      end\n"
             "    end\n"
             "  end\n" + okEnd();
    }));
}

} // namespace mcp
} // namespace app
