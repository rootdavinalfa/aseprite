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
#include <cstdlib>
#include <string>

namespace app {
namespace mcp {

namespace {

using json11::Json;

// Saves the file and reports success; appended after a transaction's
// closing "end)".
const std::string kSaveOk =
  "\n"
  "spr:saveAs(spr.filename)\n"
  "print(\"OK\")";

Json intProp(const char* description)
{
  return Json::object { { "type", "integer" }, { "description", description } };
}

Json numberProp(const char* description)
{
  return Json::object { { "type", "number" }, { "description", description } };
}

Json boolProp(const char* description)
{
  return Json::object { { "type", "boolean" }, { "description", description } };
}

Json stringProp(const char* description)
{
  return Json::object { { "type", "string" }, { "description", description } };
}

Json stringListProp(const char* description)
{
  return Json::object {
    { "type", "array" },
    { "description", description },
    { "items", Json::object { { "type", "string" } } },
  };
}

Json fileSchema(Json::object properties, Json::array required)
{
  properties["filename"] = stringProp("Aseprite file to modify");
  return Json::object {
    { "type", "object" },
    { "properties", properties },
    { "required", required },
  };
}

std::string luaBool(const bool value)
{
  return value ? "true" : "false";
}

// A call that reports a validation failure; the "ERROR:" prefix turns
// the tool result into an error.
std::string luaError(const std::string& message)
{
  return "print(\"ERROR:" + luaString(message) + "\")";
}

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
         "if idx < 1 or idx > #spr.frames then\n"
         "  print(\"ERROR:Frame index out of range\") return\n"
         "end\n";
}

// Checks a 1-based inclusive frame range carried in the locals
// start_idx and end_idx.
std::string frameRangeCheck(const int startFrame, const int endFrame)
{
  return "local start_idx = " + std::to_string(startFrame) + "\n"
         "local end_idx = " + std::to_string(endFrame) + "\n"
         "if start_idx < 1 or end_idx > #spr.frames or start_idx > end_idx then\n"
         "  print(\"ERROR:Frame range out of bounds\") return\n"
         "end\n";
}

// Like frameRangeCheck(), but a "nil" end literal means the last frame.
std::string frameRangeOpenEndCheck(const int startFrame, const std::string& endLiteral)
{
  return "local start_idx = " + std::to_string(startFrame) + "\n"
         "local end_idx = " + endLiteral + "\n"
         "if end_idx == nil then end_idx = #spr.frames end\n"
         "if start_idx < 1 or end_idx > #spr.frames or start_idx > end_idx then\n"
         "  print(\"ERROR:Frame range out of bounds\") return\n"
         "end\n";
}

// The optional integer arguments are rendered as Lua literals; an
// absent argument becomes nil.
std::string optionalIntLiteral(const Json& args, const char* name)
{
  const Json& value = args[name];
  return value.is_number() ? std::to_string(value.int_value()) : "nil";
}

// Resolves the layer named by "layer_name" into the local "target".
std::string layerLookup(const std::string& layerName)
{
  std::string out = kFindLayerLua;
  out += "local target = find_layer(spr, \"" + luaString(layerName) + "\")\n"
         "if not target then print(\"ERROR:Layer not found\") return end\n";
  return out;
}

// Resolves the optional source frame into the local "src", defaulting
// to the expression in "defaultExpr" when absent. An explicit source
// frame outside the frame range is an error (a blank cel would silently
// replace the intended copy source).
std::string sourceFrameLookup(const Json& args, const char* defaultExpr)
{
  const std::string literal = optionalIntLiteral(args, "source_frame_index");
  return "local src = " + literal + "\n"
         "if src == nil then src = " + std::string(defaultExpr) + " end\n"
         "if src < 1 or src > #spr.frames then\n"
         "  print(\"ERROR:Source frame out of range\") return\n"
         "end\n";
}

// Creates the cel of a layer/frame, cloning the cel at "source_frame"
// when it has one and falling back to an empty canvas-sized image.
const char kEnsureCelLua[] =
  "local function ensure_cel(spr, layer, frame, source_frame)\n"
  "  if source_frame >= 1 and source_frame <= #spr.frames then\n"
  "    local src = layer:cel(spr.frames[source_frame])\n"
  "    if src then\n"
  "      return spr:newCel(layer, frame, src.image:clone(), src.position)\n"
  "    end\n"
  "  end\n"
  "  return spr:newCel(layer, frame, Image(spr.width, spr.height, spr.colorMode), Point(0, 0))\n"
  "end\n";

// Ease functions shared by the tween tools; "mode" comes from the
// validated easing names.
std::string easeFunction(const std::string& mode)
{
  return "local function ease(t)\n"
         "  local mode = \"" + luaString(mode) + "\"\n"
         "  if mode == \"linear\" then return t end\n"
         "  if mode == \"ease_in\" then return t * t end\n"
         "  if mode == \"ease_out\" then return 1 - (1 - t) * (1 - t) end\n"
         "  if mode == \"ease_in_out\" then\n"
         "    if t < 0.5 then return 2 * t * t end\n"
         "    local u = -2 * t + 2\n"
         "    return 1 - (u * u) / 2\n"
         "  end\n"
         "  return t * t * (3 - 2 * t)\n"
         "end\n";
}

std::string toLowerTrimmed(const std::string& value)
{
  std::string out;
  out.reserve(value.size());
  for (const char c : value)
    out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  const size_t begin = out.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos)
    return {};
  const size_t end = out.find_last_not_of(" \t\r\n");
  return out.substr(begin, end - begin + 1);
}

bool isEasing(const std::string& mode)
{
  return mode == "linear" || mode == "ease_in" || mode == "ease_out" ||
         mode == "ease_in_out" || mode == "smoothstep";
}

const char* const kEasingError = "Unsupported easing (linear, ease_in, ease_out, ease_in_out, smoothstep)";

// Renders a JSON array of strings as a Lua list literal.
std::string stringListLua(const Json& list)
{
  std::string out = "{";
  bool first = true;
  for (const Json& item : list.array_items()) {
    if (!item.is_string())
      continue;
    if (!first)
      out += ", ";
    first = false;
    out += "\"" + luaString(item.string_value()) + "\"";
  }
  return out + "}";
}

// Like stringListLua(), but the literal is nil for an absent or empty
// list, which the scripts treat as "no filter".
std::string optionalStringListLua(const Json& list)
{
  if (!list.is_array() || list.array_items().empty())
    return "nil";
  return stringListLua(list);
}

std::string trimmed(const std::string& value)
{
  size_t begin = 0;
  size_t end = value.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(value[begin])))
    ++begin;
  while (end > begin && std::isspace(static_cast<unsigned char>(value[end - 1])))
    --end;
  return value.substr(begin, end - begin);
}

bool parseInt(const std::string& value, long& out)
{
  if (value.empty())
    return false;
  const char* begin = value.c_str();
  char* end = nullptr;
  const long parsed = std::strtol(begin, &end, 10);
  if (end == begin)
    return false;
  while (*end == ' ' || *end == '\t')
    ++end;
  if (*end != '\0')
    return false;
  out = parsed;
  return true;
}

// Parses "layer:1-8,17-24" entries into a Lua map from layer name to a
// list of {start, end} spans.
std::string layerFrameRangesLua(const Json& entries)
{
  std::string out = "{";
  for (const Json& entry : entries.array_items()) {
    if (!entry.is_string())
      continue;
    const std::string text = entry.string_value();
    const size_t colon = text.find(':');
    if (colon == std::string::npos)
      continue;
    const std::string layer = trimmed(text.substr(0, colon));
    if (layer.empty())
      continue;

    std::string spans;
    size_t pos = colon + 1;
    while (pos <= text.size()) {
      const size_t comma = text.find(',', pos);
      const std::string span = trimmed(
        text.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos));
      const size_t dash = span.find('-');
      long start = 0;
      long end = 0;
      if (dash != std::string::npos &&
          parseInt(trimmed(span.substr(0, dash)), start) &&
          parseInt(trimmed(span.substr(dash + 1)), end) &&
          start > 0 && end >= start) {
        if (!spans.empty())
          spans += ",";
        spans += "{" + std::to_string(start) + "," + std::to_string(end) + "}";
      }
      if (comma == std::string::npos)
        break;
      pos = comma + 1;
    }
    if (spans.empty())
      continue;
    out += "[\"" + luaString(layer) + "\"]={" + spans + "},";
  }
  return out + "}";
}

// Parses "layerA,layerB" or "layerA:layerB" entries into a Lua list of
// name pairs.
std::string overlapPairsLua(const Json& entries)
{
  std::string out = "{";
  for (const Json& entry : entries.array_items()) {
    if (!entry.is_string())
      continue;
    const std::string text = entry.string_value();
    size_t sep = text.find(',');
    if (sep == std::string::npos)
      sep = text.find(':');
    if (sep == std::string::npos)
      continue;
    const std::string a = trimmed(text.substr(0, sep));
    const std::string b = trimmed(text.substr(sep + 1));
    if (a.empty() || b.empty())
      continue;
    out += "{\"" + luaString(a) + "\",\"" + luaString(b) + "\"},";
  }
  return out + "}";
}

// Resolves the local "target_names" (nil means every non-group layer)
// into "target_layers" plus a "layer_map" from name to layer.
const char kTargetLayersLua[] =
  "local target_layers = {}\n"
  "if target_names == nil then\n"
  "  for _, layer in ipairs(spr.layers) do\n"
  "    if not layer.isGroup then table.insert(target_layers, layer) end\n"
  "  end\n"
  "else\n"
  "  for _, name in ipairs(target_names) do\n"
  "    local layer = find_layer(spr, name)\n"
  "    if layer then table.insert(target_layers, layer) end\n"
  "  end\n"
  "end\n"
  "local layer_map = {}\n"
  "for _, layer in ipairs(target_layers) do layer_map[layer.name] = layer end\n";

// Checks that a frame index is inside one of the declared ranges of a
// layer; layers without an entry are always in range.
const char kInRangesLua[] =
  "local function in_ranges(layer_name, frame_index)\n"
  "  local ranges = ranges_by_layer[layer_name]\n"
  "  if not ranges then return true end\n"
  "  for _, range in ipairs(ranges) do\n"
  "    if frame_index >= range[1] and frame_index <= range[2] then return true end\n"
  "  end\n"
  "  return false\n"
  "end\n";

// A JSON null usable as a table value: json.encode() writes it back as
// null, unlike a nil field which disappears.
const char kJsonNullLua[] = "local JSON_NULL = json.decode(\"null\")\n";

} // anonymous namespace

void register_animation_tools(std::vector<McpTool>& tools)
{
  // Sprite info.

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

  // Frames.

  tools.push_back(LuaTool(
    "add_frames",
    "Append frames to the end of the animation. Each new frame starts as a copy of the "
    "previous one, and duration_ms (when positive) is applied to every frame added. "
    "The file is saved.",
    fileSchema({
      { "count", intProp("Number of frames to add (must be >= 1)") },
      { "duration_ms", intProp("Duration in milliseconds for the added frames "
                               "(default: keep the frame's own duration)") },
    }, { "filename", "count" }),
    true,
    [](const Json& args) -> std::string {
      const int count = argInt(args, "count");
      if (count < 1)
        return luaError("Count must be >= 1");

      const int durationMs = argInt(args, "duration_ms", 0);
      return std::string(kSpritePreludeLua) +
             "local count = " + std::to_string(count) + "\n"
             "local duration = " + std::to_string(durationMs) + "\n"
             "\n"
             "app.transaction(function()\n"
             "  for i = 1, count do\n"
             "    local frame = spr:newFrame()\n"
             "    if duration > 0 then frame.duration = duration / 1000.0 end\n"
             "  end\n"
             "end)\n" + kSaveOk;
    }));

  tools.push_back(LuaTool(
    "set_frame_duration_all",
    "Set the duration of every frame in the animation, in milliseconds. The file is saved.",
    fileSchema({
      { "duration_ms", intProp("Duration in milliseconds (must be > 0)") },
    }, { "filename", "duration_ms" }),
    true,
    [](const Json& args) -> std::string {
      const int durationMs = argInt(args, "duration_ms");
      if (durationMs <= 0)
        return luaError("Duration must be > 0");

      return std::string(kSpritePreludeLua) +
             "app.transaction(function()\n"
             "  for i = 1, #spr.frames do\n"
             "    spr.frames[i].duration = " + std::to_string(durationMs) + " / 1000.0\n"
             "  end\n"
             "end)\n" + kSaveOk;
    }));

  tools.push_back(LuaTool(
    "duplicate_frame_range",
    "Duplicate a frame range and append the copies at the end of the animation. Each copy "
    "takes the cels of the frame it duplicates, times times over. The file is saved.",
    fileSchema({
      { "start_frame", intProp("First frame of the range (1-based)") },
      { "end_frame", intProp("Last frame of the range (1-based, inclusive)") },
      { "times", intProp("Number of copies to append (default: 1, must be >= 1)") },
    }, { "filename", "start_frame", "end_frame" }),
    true,
    [](const Json& args) -> std::string {
      const int times = argInt(args, "times", 1);
      if (times < 1)
        return luaError("Times must be >= 1");

      return std::string(kSpritePreludeLua) +
             frameRangeCheck(argInt(args, "start_frame"), argInt(args, "end_frame")) +
             "local times = " + std::to_string(times) + "\n"
             "\n"
             "app.transaction(function()\n"
             "  for t = 1, times do\n"
             "    for fi = start_idx, end_idx do\n"
             "      local frame = spr:newFrame()\n"
             "      for _, layer in ipairs(spr.layers) do\n"
             "        if not layer.isGroup then\n"
             "          local cel = layer:cel(spr.frames[fi])\n"
             "          if cel then\n"
             "            spr:newCel(layer, frame, cel.image:clone(), cel.position)\n"
             "          end\n"
             "        end\n"
             "      end\n"
             "    end\n"
             "  end\n"
             "end)\n" + kSaveOk;
    }));

  tools.push_back(LuaTool(
    "delete_frame",
    "Delete a frame and its cels by 1-based index. The last remaining frame cannot be "
    "deleted. The file is saved.",
    fileSchema({
      { "frame_index", intProp("Frame index starting at 1") },
    }, { "filename", "frame_index" }),
    true,
    [](const Json& args) -> std::string {
      return std::string(kSpritePreludeLua) +
             frameIndexCheck(argInt(args, "frame_index")) +
             "if #spr.frames <= 1 then\n"
             "  print(\"ERROR:Cannot delete the only frame\") return\n"
             "end\n"
             "\n"
             "app.transaction(function()\n"
             "  spr:deleteFrame(spr.frames[idx])\n"
             "end)\n" + kSaveOk;
    }));

  tools.push_back(LuaTool(
    "copy_frame",
    "Copy every cel of a source frame to another frame, or to a new frame appended at the "
    "end when target_frame is omitted. With overwrite the target's cels are replaced first. "
    "The file is saved.",
    fileSchema({
      { "source_frame", intProp("Frame index to copy from (1-based)") },
      { "target_frame", intProp("Frame index to copy to (1-based; default: append a new "
                                "frame at the end)") },
      { "overwrite", boolProp("Replace the cels already on the target frame "
                              "(default: true)") },
    }, { "filename", "source_frame" }),
    true,
    [](const Json& args) -> std::string {
      const int sourceFrame = argInt(args, "source_frame");
      const bool overwrite = argBool(args, "overwrite", true);

      std::string script = kSpritePreludeLua;
      script += "local src_idx = " + std::to_string(sourceFrame) + "\n"
                "local dst_idx = " + optionalIntLiteral(args, "target_frame") + "\n"
                "if src_idx < 1 or src_idx > #spr.frames then\n"
                "  print(\"ERROR:Source frame out of range\") return\n"
                "end\n"
                "\n"
                "app.transaction(function()\n"
                "  local dst_frame = nil\n"
                "  if dst_idx == nil then\n"
                "    dst_frame = spr:newFrame()\n"
                "  else\n"
                "    if dst_idx < 1 or dst_idx > #spr.frames then return end\n"
                "    dst_frame = spr.frames[dst_idx]\n"
                "    if " + luaBool(overwrite) + " then\n"
                "      for _, layer in ipairs(spr.layers) do\n"
                "        if not layer.isGroup then\n"
                "          local cel = layer:cel(dst_frame)\n"
                "          if cel then spr:deleteCel(cel) end\n"
                "        end\n"
                "      end\n"
                "    end\n"
                "  end\n"
                "\n"
                "  for _, layer in ipairs(spr.layers) do\n"
                "    if not layer.isGroup then\n"
                "      local cel = layer:cel(spr.frames[src_idx])\n"
                "      if cel then\n"
                "        spr:newCel(layer, dst_frame, cel.image:clone(), cel.position)\n"
                "      end\n"
                "    end\n"
                "  end\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  tools.push_back(LuaTool(
    "propagate_frame_to_range",
    "Copy every cel of a source frame onto each frame of a range (except the source "
    "itself). With overwrite the destination cels are replaced first. The file is saved.",
    fileSchema({
      { "source_frame", intProp("Frame index to copy from (1-based)") },
      { "start_frame", intProp("First frame of the range (1-based)") },
      { "end_frame", intProp("Last frame of the range (1-based, inclusive)") },
      { "overwrite", boolProp("Replace the cels already on each destination frame "
                              "(default: true)") },
    }, { "filename", "source_frame", "start_frame", "end_frame" }),
    true,
    [](const Json& args) -> std::string {
      const bool overwrite = argBool(args, "overwrite", true);

      std::string script = kSpritePreludeLua;
      script += frameRangeCheck(argInt(args, "start_frame"), argInt(args, "end_frame"));
      script += "local src_idx = " + std::to_string(argInt(args, "source_frame")) + "\n"
                "if src_idx < 1 or src_idx > #spr.frames then\n"
                "  print(\"ERROR:Source frame out of range\") return\n"
                "end\n"
                "\n"
                "app.transaction(function()\n"
                "  for fi = start_idx, end_idx do\n"
                "    if fi ~= src_idx then\n"
                "      local dst_frame = spr.frames[fi]\n"
                "      if " + luaBool(overwrite) + " then\n"
                "        for _, layer in ipairs(spr.layers) do\n"
                "          if not layer.isGroup then\n"
                "            local cel = layer:cel(dst_frame)\n"
                "            if cel then spr:deleteCel(cel) end\n"
                "          end\n"
                "        end\n"
                "      end\n"
                "      for _, layer in ipairs(spr.layers) do\n"
                "        if not layer.isGroup then\n"
                "          local cel = layer:cel(spr.frames[src_idx])\n"
                "          if cel then\n"
                "            spr:newCel(layer, dst_frame, cel.image:clone(), cel.position)\n"
                "          end\n"
                "        end\n"
                "      end\n"
                "    end\n"
                "  end\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  tools.push_back(LuaTool(
    "propagate_cels",
    "Copy the cels of a source frame onto a range of frames for the given layers only. "
    "With replace the destination cels are replaced first; layers with no cel on the "
    "source frame are left untouched. The file is saved.",
    fileSchema({
      { "layer_names", stringListProp("Layer names to copy (must not be empty)") },
      { "source_frame", intProp("Frame index to copy from (1-based)") },
      { "start_frame", intProp("First frame of the range (1-based)") },
      { "end_frame", intProp("Last frame of the range (1-based, inclusive)") },
      { "replace", boolProp("Replace the cels already on each destination frame "
                            "(default: true)") },
    }, { "filename", "layer_names", "source_frame", "start_frame", "end_frame" }),
    true,
    [](const Json& args) -> std::string {
      const Json& layerNames = args["layer_names"];
      if (!layerNames.is_array() || layerNames.array_items().empty())
        return luaError("Layer names list cannot be empty");

      const bool replace = argBool(args, "replace", true);

      std::string script = kSpritePreludeLua;
      script += frameRangeCheck(argInt(args, "start_frame"), argInt(args, "end_frame"));
      script += kFindLayerLua;
      script += "local src_idx = " + std::to_string(argInt(args, "source_frame")) + "\n"
                "if src_idx < 1 or src_idx > #spr.frames then\n"
                "  print(\"ERROR:Source frame out of range\") return\n"
                "end\n"
                "\n"
                "local names = " + stringListLua(layerNames) + "\n"
                "local targets = {}\n"
                "for _, name in ipairs(names) do\n"
                "  local layer = find_layer(spr, name)\n"
                "  if layer then table.insert(targets, layer) end\n"
                "end\n"
                "if #targets == 0 then print(\"ERROR:No layers found\") return end\n"
                "\n"
                "app.transaction(function()\n"
                "  for fi = start_idx, end_idx do\n"
                "    if fi ~= src_idx then\n"
                "      local dst_frame = spr.frames[fi]\n"
                "      for _, layer in ipairs(targets) do\n"
                "        local src_cel = layer:cel(spr.frames[src_idx])\n"
                "        if src_cel then\n"
                "          local dst_cel = layer:cel(dst_frame)\n"
                "          if dst_cel and " + luaBool(replace) + " then\n"
                "            spr:deleteCel(dst_cel)\n"
                "            dst_cel = nil\n"
                "          end\n"
                "          if not dst_cel then\n"
                "            spr:newCel(layer, dst_frame, src_cel.image:clone(), src_cel.position)\n"
                "          end\n"
                "        end\n"
                "      end\n"
                "    end\n"
                "  end\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  // Tags.

  tools.push_back(LuaTool(
    "set_tag",
    "Create an animation tag from_frame..to_frame, or move and rename the existing tag with "
    "the same name. direction is forward, reverse, pingpong, or pingpong_reverse. "
    "The file is saved.",
    fileSchema({
      { "name", stringProp("Tag name") },
      { "from_frame", intProp("First frame of the tag (1-based)") },
      { "to_frame", intProp("Last frame of the tag (1-based, inclusive)") },
      { "direction", stringProp("Animation direction: forward, reverse, pingpong, or "
                                "pingpong_reverse (default: forward)") },
    }, { "filename", "name", "from_frame", "to_frame" }),
    true,
    [](const Json& args) -> std::string {
      const std::string name = argString(args, "name");
      const std::string direction = argString(args, "direction", "forward");
      const std::string aniDir =
        direction == "forward" ? "AniDir.FORWARD" :
        direction == "reverse" ? "AniDir.REVERSE" :
        direction == "pingpong" ? "AniDir.PING_PONG" :
        direction == "pingpong_reverse" ? "AniDir.PING_PONG_REVERSE" : std::string();
      if (aniDir.empty())
        return luaError("Unsupported direction '" + direction +
                        "' (forward, reverse, pingpong, pingpong_reverse)");

      std::string script = kSpritePreludeLua;
      script += frameRangeCheck(argInt(args, "from_frame"), argInt(args, "to_frame"));
      script += "local name = \"" + luaString(name) + "\"\n"
                "\n"
                "local tag = nil\n"
                "for _, t in ipairs(spr.tags) do\n"
                "  if t.name == name then tag = t break end\n"
                "end\n"
                "\n"
                "app.transaction(function()\n"
                "  if tag then\n"
                "    tag.fromFrame = spr.frames[start_idx]\n"
                "    tag.toFrame = spr.frames[end_idx]\n"
                "  else\n"
                "    tag = spr:newTag(start_idx, end_idx)\n"
                "  end\n"
                "  tag.name = name\n"
                "  tag.aniDir = " + aniDir + "\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  tools.push_back(LuaTool(
    "delete_tag",
    "Delete the animation tag with the given name. The file is saved.",
    fileSchema({
      { "name", stringProp("Tag name to delete") },
    }, { "filename", "name" }),
    true,
    [](const Json& args) -> std::string {
      return std::string(kSpritePreludeLua) +
             "local target_tag = nil\n"
             "for _, tag in ipairs(spr.tags) do\n"
             "  if tag.name == \"" + luaString(argString(args, "name")) + "\" then\n"
             "    target_tag = tag break\n"
             "  end\n"
             "end\n"
             "if not target_tag then print(\"ERROR:Tag not found\") return end\n"
             "\n"
             "app.transaction(function()\n"
             "  spr:deleteTag(target_tag)\n"
             "end)\n" + kSaveOk;
    }));

  tools.push_back(LuaTool(
    "set_onion_skin",
    "Report the onion skin settings that were requested. Onion skin is a timeline UI "
    "preference with no scripting surface, so no change is applied and the file is left "
    "untouched.",
    fileSchema({
      { "enabled", boolProp("Whether onion skin should be enabled (default: true)") },
      { "before", intProp("Number of previous frames shown (default: 2, must be >= 0)") },
      { "after", intProp("Number of following frames shown (default: 2, must be >= 0)") },
      { "opacity", intProp("Onion skin opacity 0-255 (default: 128)") },
    }, { "filename" }),
    true,
    [](const Json& args) -> std::string {
      const bool enabled = argBool(args, "enabled", true);
      const int before = argInt(args, "before", 2);
      const int after = argInt(args, "after", 2);
      const int opacity = argInt(args, "opacity", 128);
      if (before < 0 || after < 0)
        return luaError("Before/after must be >= 0");
      if (opacity < 0 || opacity > 255)
        return luaError("Opacity must be between 0 and 255");

      return "print(\"Onion skin is a UI-only setting; no changes applied "
             "(enabled=" + luaBool(enabled) +
             ", before=" + std::to_string(before) +
             ", after=" + std::to_string(after) +
             ", opacity=" + std::to_string(opacity) + ")\")";
    }));

  // Layer state.

  tools.push_back(LuaTool(
    "set_layer_visibility",
    "Show or hide a layer, searching inside groups too. The file is saved.",
    fileSchema({
      { "layer_name", stringProp("Layer name, or group/child path") },
      { "visible", boolProp("Whether the layer should be visible (default: true)") },
    }, { "filename", "layer_name" }),
    true,
    [](const Json& args) -> std::string {
      return std::string(kSpritePreludeLua) +
             layerLookup(argString(args, "layer_name")) +
             "\n"
             "app.transaction(function()\n"
             "  target.isVisible = " + luaBool(argBool(args, "visible", true)) + "\n"
             "end)\n" + kSaveOk;
    }));

  tools.push_back(LuaTool(
    "set_layer_opacity",
    "Set a layer's opacity from 0 (transparent) to 255 (opaque), searching inside groups "
    "too. The file is saved.",
    fileSchema({
      { "layer_name", stringProp("Layer name, or group/child path") },
      { "opacity", intProp("Opacity 0-255") },
    }, { "filename", "layer_name", "opacity" }),
    true,
    [](const Json& args) -> std::string {
      const int opacity = argInt(args, "opacity");
      if (opacity < 0 || opacity > 255)
        return luaError("Opacity must be between 0 and 255");

      return std::string(kSpritePreludeLua) +
             layerLookup(argString(args, "layer_name")) +
             "\n"
             "app.transaction(function()\n"
             "  target.opacity = " + std::to_string(opacity) + "\n"
             "end)\n" + kSaveOk;
    }));

  // Cels.

  tools.push_back(LuaTool(
    "create_cel",
    "Create an empty canvas-sized cel on a layer/frame; an existing cel is left alone. "
    "The file is saved.",
    fileSchema({
      { "layer_name", stringProp("Layer name, or group/child path") },
      { "frame_index", intProp("Frame index starting at 1") },
      { "x", intProp("Cel X position in pixels (default: 0)") },
      { "y", intProp("Cel Y position in pixels (default: 0)") },
    }, { "filename", "layer_name", "frame_index" }),
    true,
    [](const Json& args) -> std::string {
      return std::string(kSpritePreludeLua) +
             frameIndexCheck(argInt(args, "frame_index")) +
             layerLookup(argString(args, "layer_name")) +
             "\n"
             "app.transaction(function()\n"
             "  local frame = spr.frames[idx]\n"
             "  if target:cel(frame) then return end\n"
             "  spr:newCel(target, frame,\n"
             "             Image(spr.width, spr.height, spr.colorMode),\n"
             "             Point(" + std::to_string(argInt(args, "x")) + ", " +
             std::to_string(argInt(args, "y")) + "))\n"
             "end)\n" + kSaveOk;
    }));

  tools.push_back(LuaTool(
    "clear_cel",
    "Delete the cel on a layer/frame, if any. The file is saved.",
    fileSchema({
      { "layer_name", stringProp("Layer name, or group/child path") },
      { "frame_index", intProp("Frame index starting at 1") },
    }, { "filename", "layer_name", "frame_index" }),
    true,
    [](const Json& args) -> std::string {
      return std::string(kSpritePreludeLua) +
             frameIndexCheck(argInt(args, "frame_index")) +
             layerLookup(argString(args, "layer_name")) +
             "\n"
             "app.transaction(function()\n"
             "  local cel = target:cel(spr.frames[idx])\n"
             "  if cel then spr:deleteCel(cel) end\n"
             "end)\n" + kSaveOk;
    }));

  tools.push_back(LuaTool(
    "copy_cel",
    "Copy a cel from one frame to another on the same layer. With replace the target cel "
    "is deleted first; when the source has no cel nothing happens. The file is saved.",
    fileSchema({
      { "layer_name", stringProp("Layer name, or group/child path") },
      { "source_frame", intProp("Frame index to copy from (1-based)") },
      { "target_frame", intProp("Frame index to copy to (1-based)") },
      { "replace", boolProp("Replace an existing target cel (default: true)") },
    }, { "filename", "layer_name", "source_frame", "target_frame" }),
    true,
    [](const Json& args) -> std::string {
      std::string script = kSpritePreludeLua;
      script += "local src_idx = " + std::to_string(argInt(args, "source_frame")) + "\n"
                "local dst_idx = " + std::to_string(argInt(args, "target_frame")) + "\n"
                "if src_idx < 1 or src_idx > #spr.frames then\n"
                "  print(\"ERROR:Source frame out of range\") return\n"
                "end\n"
                "if dst_idx < 1 or dst_idx > #spr.frames then\n"
                "  print(\"ERROR:Target frame out of range\") return\n"
                "end\n"
                "\n";
      script += layerLookup(argString(args, "layer_name"));
      script += "\n"
                "app.transaction(function()\n"
                "  local src = target:cel(spr.frames[src_idx])\n"
                "  if not src then return end\n"
                "  local dst = target:cel(spr.frames[dst_idx])\n"
                "  if dst and " + luaBool(argBool(args, "replace", true)) + " then\n"
                "    spr:deleteCel(dst)\n"
                "    dst = nil\n"
                "  end\n"
                "  if not dst then\n"
                "    spr:newCel(target, spr.frames[dst_idx], src.image:clone(), src.position)\n"
                "  end\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  tools.push_back(LuaTool(
    "set_cel_position",
    "Move a cel to (x, y). With create_if_missing a missing cel is created; it then starts "
    "as a copy of the cel at source_frame_index (default: the same frame), or as an empty "
    "canvas-sized image. The file is saved.",
    fileSchema({
      { "layer_name", stringProp("Layer name, or group/child path") },
      { "frame_index", intProp("Frame index starting at 1") },
      { "x", intProp("Cel X position in pixels") },
      { "y", intProp("Cel Y position in pixels") },
      { "create_if_missing", boolProp("Create the cel when it does not exist "
                                      "(default: false)") },
      { "source_frame_index", intProp("Frame to copy the cel image from when creating it "
                                      "(default: the cel's own frame)") },
    }, { "filename", "layer_name", "frame_index", "x", "y" }),
    true,
    [](const Json& args) -> std::string {
      std::string script = kSpritePreludeLua;
      script += frameIndexCheck(argInt(args, "frame_index"));
      script += layerLookup(argString(args, "layer_name"));
      script += kEnsureCelLua;
      script += sourceFrameLookup(args, "idx");
      script += "\n"
                "app.transaction(function()\n"
                "  local cel = target:cel(spr.frames[idx])\n"
                "  if not cel and " + luaBool(argBool(args, "create_if_missing")) + " then\n"
                "    cel = ensure_cel(spr, target, spr.frames[idx], src)\n"
                "  end\n"
                "  if not cel then return end\n"
                "  cel.position = Point(" + std::to_string(argInt(args, "x")) + ", " +
                std::to_string(argInt(args, "y")) + ")\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  tools.push_back(LuaTool(
    "offset_cel_positions",
    "Shift the position of every cel of a layer across a frame range by (dx, dy). Frames "
    "without a cel are skipped. The file is saved.",
    fileSchema({
      { "layer_name", stringProp("Layer name, or group/child path") },
      { "start_frame", intProp("First frame of the range (1-based)") },
      { "end_frame", intProp("Last frame of the range (1-based, inclusive)") },
      { "dx", intProp("X delta in pixels") },
      { "dy", intProp("Y delta in pixels") },
    }, { "filename", "layer_name", "start_frame", "end_frame", "dx", "dy" }),
    true,
    [](const Json& args) -> std::string {
      std::string script = kSpritePreludeLua;
      script += frameRangeCheck(argInt(args, "start_frame"), argInt(args, "end_frame"));
      script += layerLookup(argString(args, "layer_name"));
      script += "\n"
                "app.transaction(function()\n"
                "  for fi = start_idx, end_idx do\n"
                "    local cel = target:cel(spr.frames[fi])\n"
                "    if cel then\n"
                "      local pos = cel.position\n"
                "      cel.position = Point(pos.x + " + std::to_string(argInt(args, "dx")) +
                ", pos.y + " + std::to_string(argInt(args, "dy")) + ")\n"
                "    end\n"
                "  end\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  tools.push_back(LuaTool(
    "set_cel_opacity",
    "Set a single cel's opacity from 0 (transparent) to 255 (opaque). The file is saved.",
    fileSchema({
      { "layer_name", stringProp("Layer name, or group/child path") },
      { "frame_index", intProp("Frame index starting at 1") },
      { "opacity", intProp("Opacity 0-255") },
    }, { "filename", "layer_name", "frame_index", "opacity" }),
    true,
    [](const Json& args) -> std::string {
      const int opacity = argInt(args, "opacity");
      if (opacity < 0 || opacity > 255)
        return luaError("Opacity must be between 0 and 255");

      std::string script = kSpritePreludeLua;
      script += frameIndexCheck(argInt(args, "frame_index"));
      script += layerLookup(argString(args, "layer_name"));
      script += "local cel = target:cel(spr.frames[idx])\n"
                "if not cel then print(\"ERROR:No cel at that layer/frame\") return end\n"
                "\n"
                "app.transaction(function()\n"
                "  cel.opacity = " + std::to_string(opacity) + "\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  // Tweening.

  tools.push_back(LuaTool(
    "tween_cel_positions",
    "Move a layer's cels along a straight line from (start_x, start_y) to (end_x, end_y) "
    "across a frame range, with even steps and rounded coordinates. With "
    "create_missing_cels each frame gets a cel (copied from source_frame_index, default: "
    "the range start). The file is saved.",
    fileSchema({
      { "layer_name", stringProp("Layer name, or group/child path") },
      { "start_frame", intProp("First frame of the range (1-based)") },
      { "end_frame", intProp("Last frame of the range (1-based, inclusive)") },
      { "start_x", intProp("X position on the first frame") },
      { "start_y", intProp("Y position on the first frame") },
      { "end_x", intProp("X position on the last frame") },
      { "end_y", intProp("Y position on the last frame") },
      { "create_missing_cels", boolProp("Create cels missing on the destination frames "
                                        "(default: false)") },
      { "source_frame_index", intProp("Frame to copy newly created cels from (default: the "
                                      "range start)") },
    }, { "filename", "layer_name", "start_frame", "end_frame",
         "start_x", "start_y", "end_x", "end_y" }),
    true,
    [](const Json& args) -> std::string {
      std::string script = kSpritePreludeLua;
      script += frameRangeCheck(argInt(args, "start_frame"), argInt(args, "end_frame"));
      script += layerLookup(argString(args, "layer_name"));
      script += kEnsureCelLua;
      script += sourceFrameLookup(args, "start_idx");
      script += "local create_missing = " + luaBool(argBool(args, "create_missing_cels")) + "\n";
      script += "\n"
                "app.transaction(function()\n"
                "  local span = end_idx - start_idx\n"
                "  for fi = start_idx, end_idx do\n"
                "    local t = 0\n"
                "    if span > 0 then t = (fi - start_idx) / span end\n"
                "    local x = math.floor(" + std::to_string(argInt(args, "start_x")) + " + (" +
                std::to_string(argInt(args, "end_x")) + " - " +
                std::to_string(argInt(args, "start_x")) + ") * t + 0.5)\n"
                "    local y = math.floor(" + std::to_string(argInt(args, "start_y")) + " + (" +
                std::to_string(argInt(args, "end_y")) + " - " +
                std::to_string(argInt(args, "start_y")) + ") * t + 0.5)\n"
                "    local cel = target:cel(spr.frames[fi])\n"
                "    if not cel and create_missing then\n"
                "      cel = ensure_cel(spr, target, spr.frames[fi], src)\n"
                "    end\n"
                "    if cel then cel.position = Point(x, y) end\n"
                "  end\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  tools.push_back(LuaTool(
    "tween_cel_positions_eased",
    "Like tween_cel_positions, but the interpolation is shaped by easing: linear, ease_in, "
    "ease_out, ease_in_out, or smoothstep (default). The file is saved.",
    fileSchema({
      { "layer_name", stringProp("Layer name, or group/child path") },
      { "start_frame", intProp("First frame of the range (1-based)") },
      { "end_frame", intProp("Last frame of the range (1-based, inclusive)") },
      { "start_x", intProp("X position on the first frame") },
      { "start_y", intProp("Y position on the first frame") },
      { "end_x", intProp("X position on the last frame") },
      { "end_y", intProp("Y position on the last frame") },
      { "easing", stringProp("linear, ease_in, ease_out, ease_in_out, or smoothstep "
                             "(default: smoothstep)") },
      { "create_missing_cels", boolProp("Create cels missing on the destination frames "
                                        "(default: false)") },
      { "source_frame_index", intProp("Frame to copy newly created cels from (default: the "
                                      "range start)") },
    }, { "filename", "layer_name", "start_frame", "end_frame",
         "start_x", "start_y", "end_x", "end_y" }),
    true,
    [](const Json& args) -> std::string {
      const std::string easing = toLowerTrimmed(argString(args, "easing", "smoothstep"));
      if (!isEasing(easing))
        return luaError(kEasingError);

      std::string script = kSpritePreludeLua;
      script += frameRangeCheck(argInt(args, "start_frame"), argInt(args, "end_frame"));
      script += layerLookup(argString(args, "layer_name"));
      script += kEnsureCelLua;
      script += easeFunction(easing);
      script += sourceFrameLookup(args, "start_idx");
      script += "local create_missing = " + luaBool(argBool(args, "create_missing_cels")) + "\n";
      script += "\n"
                "app.transaction(function()\n"
                "  local span = end_idx - start_idx\n"
                "  for fi = start_idx, end_idx do\n"
                "    local t = 0\n"
                "    if span > 0 then t = (fi - start_idx) / span end\n"
                "    local e = ease(t)\n"
                "    local x = math.floor(" + std::to_string(argInt(args, "start_x")) + " + (" +
                std::to_string(argInt(args, "end_x")) + " - " +
                std::to_string(argInt(args, "start_x")) + ") * e + 0.5)\n"
                "    local y = math.floor(" + std::to_string(argInt(args, "start_y")) + " + (" +
                std::to_string(argInt(args, "end_y")) + " - " +
                std::to_string(argInt(args, "start_y")) + ") * e + 0.5)\n"
                "    local cel = target:cel(spr.frames[fi])\n"
                "    if not cel and create_missing then\n"
                "      cel = ensure_cel(spr, target, spr.frames[fi], src)\n"
                "    end\n"
                "    if cel then cel.position = Point(x, y) end\n"
                "  end\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  tools.push_back(LuaTool(
    "oscillate_cel_positions",
    "Add a sine/cosine wobble to a layer's cels across a frame range: X moves with "
    "amplitude_x * sin and Y with amplitude_y * cos, cycles cycles over the range, offset "
    "by phase_deg degrees. The file is saved.",
    fileSchema({
      { "layer_name", stringProp("Layer name, or group/child path") },
      { "start_frame", intProp("First frame of the range (1-based)") },
      { "end_frame", intProp("Last frame of the range (1-based, inclusive)") },
      { "amplitude_x", intProp("Horizontal amplitude in pixels (default: 0)") },
      { "amplitude_y", intProp("Vertical amplitude in pixels (default: 0)") },
      { "cycles", numberProp("Number of wobble cycles over the range (default: 1.0)") },
      { "phase_deg", numberProp("Phase offset in degrees (default: 0.0)") },
      { "create_missing_cels", boolProp("Create cels missing on the destination frames "
                                        "(default: false)") },
      { "source_frame_index", intProp("Frame to copy newly created cels from (default: the "
                                      "range start)") },
    }, { "filename", "layer_name", "start_frame", "end_frame" }),
    true,
    [](const Json& args) -> std::string {
      std::string script = kSpritePreludeLua;
      script += frameRangeCheck(argInt(args, "start_frame"), argInt(args, "end_frame"));
      script += layerLookup(argString(args, "layer_name"));
      script += kEnsureCelLua;
      script += sourceFrameLookup(args, "start_idx");
      script += "local amplitude_x = " + std::to_string(argInt(args, "amplitude_x")) + "\n"
                "local amplitude_y = " + std::to_string(argInt(args, "amplitude_y")) + "\n"
                "local cycles = " + luaNumber(argDouble(args, "cycles", 1.0)) + "\n"
                "local phase = (" + luaNumber(argDouble(args, "phase_deg", 0.0)) +
                ") * math.pi / 180\n"
                "local create_missing = " + luaBool(argBool(args, "create_missing_cels")) + "\n";
      script += "\n"
                "app.transaction(function()\n"
                "  local span = end_idx - start_idx\n"
                "  for fi = start_idx, end_idx do\n"
                "    local t = 0\n"
                "    if span > 0 then t = (fi - start_idx) / span end\n"
                "    local angle = 2 * math.pi * cycles * t + phase\n"
                "    local dx = math.floor(amplitude_x * math.sin(angle) + 0.5)\n"
                "    local dy = math.floor(amplitude_y * math.cos(angle) + 0.5)\n"
                "    local cel = target:cel(spr.frames[fi])\n"
                "    if not cel and create_missing then\n"
                "      cel = ensure_cel(spr, target, spr.frames[fi], src)\n"
                "    end\n"
                "    if cel then\n"
                "      local pos = cel.position\n"
                "      cel.position = Point(pos.x + dx, pos.y + dy)\n"
                "    end\n"
                "  end\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  tools.push_back(LuaTool(
    "tween_cel_opacity_eased",
    "Fade a layer's cels from start_opacity to end_opacity across a frame range, shaped by "
    "easing (linear, ease_in, ease_out, ease_in_out, smoothstep; default smoothstep). With "
    "create_missing_cels each frame gets a cel. The file is saved.",
    fileSchema({
      { "layer_name", stringProp("Layer name, or group/child path") },
      { "start_frame", intProp("First frame of the range (1-based)") },
      { "end_frame", intProp("Last frame of the range (1-based, inclusive)") },
      { "start_opacity", intProp("Opacity on the first frame, 0-255") },
      { "end_opacity", intProp("Opacity on the last frame, 0-255") },
      { "easing", stringProp("linear, ease_in, ease_out, ease_in_out, or smoothstep "
                             "(default: smoothstep)") },
      { "create_missing_cels", boolProp("Create cels missing on the destination frames "
                                        "(default: false)") },
      { "source_frame_index", intProp("Frame to copy newly created cels from (default: the "
                                      "range start)") },
    }, { "filename", "layer_name", "start_frame", "end_frame",
         "start_opacity", "end_opacity" }),
    true,
    [](const Json& args) -> std::string {
      const int startOpacity = argInt(args, "start_opacity");
      const int endOpacity = argInt(args, "end_opacity");
      if (startOpacity < 0 || startOpacity > 255 || endOpacity < 0 || endOpacity > 255)
        return luaError("Opacity must be between 0 and 255");

      const std::string easing = toLowerTrimmed(argString(args, "easing", "smoothstep"));
      if (!isEasing(easing))
        return luaError(kEasingError);

      std::string script = kSpritePreludeLua;
      script += frameRangeCheck(argInt(args, "start_frame"), argInt(args, "end_frame"));
      script += layerLookup(argString(args, "layer_name"));
      script += kEnsureCelLua;
      script += easeFunction(easing);
      script += sourceFrameLookup(args, "start_idx");
      script += "local create_missing = " + luaBool(argBool(args, "create_missing_cels")) + "\n";
      script += "\n"
                "app.transaction(function()\n"
                "  local span = end_idx - start_idx\n"
                "  for fi = start_idx, end_idx do\n"
                "    local t = 0\n"
                "    if span > 0 then t = (fi - start_idx) / span end\n"
                "    local e = ease(t)\n"
                "    local opacity = math.floor(" + std::to_string(startOpacity) + " + (" +
                std::to_string(endOpacity) + " - " + std::to_string(startOpacity) +
                ") * e + 0.5)\n"
                "    if opacity < 0 then opacity = 0 end\n"
                "    if opacity > 255 then opacity = 255 end\n"
                "    local cel = target:cel(spr.frames[fi])\n"
                "    if not cel and create_missing then\n"
                "      cel = ensure_cel(spr, target, spr.frames[fi], src)\n"
                "    end\n"
                "    if cel then cel.opacity = opacity end\n"
                "  end\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  tools.push_back(LuaTool(
    "tween_cel_scale_eased",
    "Scale a layer's cels from start_scale to end_scale across a frame range, shaped by "
    "easing (linear, ease_in, ease_out, ease_in_out, smoothstep; default smoothstep). The "
    "image is resized from source_frame_index (default: the range start); center keeps the "
    "source center, topleft keeps the source corner. The file is saved.",
    fileSchema({
      { "layer_name", stringProp("Layer name, or group/child path") },
      { "start_frame", intProp("First frame of the range (1-based)") },
      { "end_frame", intProp("Last frame of the range (1-based, inclusive)") },
      { "start_scale", numberProp("Scale factor on the first frame (must be > 0)") },
      { "end_scale", numberProp("Scale factor on the last frame (must be > 0)") },
      { "easing", stringProp("linear, ease_in, ease_out, ease_in_out, or smoothstep "
                             "(default: smoothstep)") },
      { "anchor", stringProp("center or topleft (default: center)") },
      { "replace", boolProp("Replace the existing destination cels (default: true)") },
      { "create_missing_cels", boolProp("Create cels missing on the destination frames "
                                        "(default: true)") },
      { "source_frame_index", intProp("Frame whose cel image is resized (default: the "
                                      "range start)") },
    }, { "filename", "layer_name", "start_frame", "end_frame",
         "start_scale", "end_scale" }),
    true,
    [](const Json& args) -> std::string {
      const double startScale = argDouble(args, "start_scale");
      const double endScale = argDouble(args, "end_scale");
      if (startScale <= 0 || endScale <= 0)
        return luaError("Scale must be > 0");

      const std::string easing = toLowerTrimmed(argString(args, "easing", "smoothstep"));
      if (!isEasing(easing))
        return luaError(kEasingError);

      const std::string anchor = toLowerTrimmed(argString(args, "anchor", "center"));
      if (anchor != "center" && anchor != "topleft")
        return luaError("Unsupported anchor (center, topleft)");

      std::string script = kSpritePreludeLua;
      script += frameRangeCheck(argInt(args, "start_frame"), argInt(args, "end_frame"));
      script += layerLookup(argString(args, "layer_name"));
      script += easeFunction(easing);
      script += sourceFrameLookup(args, "start_idx");
      script += "local base_cel = target:cel(spr.frames[src])\n"
                "if not base_cel then print(\"ERROR:Source cel not found\") return end\n"
                "\n"
                "local base_img = base_cel.image:clone()\n"
                "local base_w, base_h = base_img.width, base_img.height\n"
                "local base_pos = base_cel.position\n"
                "local anchor = \"" + luaString(anchor) + "\"\n"
                "local replace = " + luaBool(argBool(args, "replace", true)) + "\n"
                "local create_missing = " + luaBool(argBool(args, "create_missing_cels", true)) + "\n"
                "\n"
                "app.transaction(function()\n"
                "  local span = end_idx - start_idx\n"
                "  for fi = start_idx, end_idx do\n"
                "    local t = 0\n"
                "    if span > 0 then t = (fi - start_idx) / span end\n"
                "    local scale = " + luaNumber(startScale) + " + (" + luaNumber(endScale) +
                " - " + luaNumber(startScale) + ") * ease(t)\n"
                "    local w = math.max(1, math.floor(base_w * scale + 0.5))\n"
                "    local h = math.max(1, math.floor(base_h * scale + 0.5))\n"
                "    local cel = target:cel(spr.frames[fi])\n"
                "    if cel and replace then\n"
                "      spr:deleteCel(cel)\n"
                "      cel = nil\n"
                "    end\n"
                "    if not cel and create_missing then\n"
                "      local img = base_img:clone()\n"
                "      img:resize(w, h)\n"
                "      local px, py = base_pos.x, base_pos.y\n"
                "      if anchor == \"center\" then\n"
                "        local cx = base_pos.x + base_w / 2\n"
                "        local cy = base_pos.y + base_h / 2\n"
                "        px = math.floor(cx - w / 2 + 0.5)\n"
                "        py = math.floor(cy - h / 2 + 0.5)\n"
                "      end\n"
                "      cel = spr:newCel(target, spr.frames[fi], img, Point(px, py))\n"
                "    end\n"
                "  end\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  // Quality.

  tools.push_back(LuaTool(
    "ensure_layers_present",
    "Create the missing cels of the named layers on every frame of a range (default: all "
    "frames), so each layer is present across the whole range. The file is saved.",
    fileSchema({
      { "layer_names", stringListProp("Layer names to cover (must not be empty)") },
      { "start_frame", intProp("First frame of the range (default: 1)") },
      { "end_frame", intProp("Last frame of the range, inclusive (default: the last frame)") },
    }, { "filename", "layer_names" }),
    true,
    [](const Json& args) -> std::string {
      const Json& layerNames = args["layer_names"];
      if (!layerNames.is_array() || layerNames.array_items().empty())
        return luaError("Layer names list cannot be empty");

      std::string script = kSpritePreludeLua;
      script += frameRangeOpenEndCheck(argInt(args, "start_frame", 1),
                                       optionalIntLiteral(args, "end_frame"));
      script += kFindLayerLua;
      script += "local names = " + stringListLua(layerNames) + "\n"
                "local targets = {}\n"
                "for _, name in ipairs(names) do\n"
                "  local layer = find_layer(spr, name)\n"
                "  if layer and not layer.isGroup then table.insert(targets, layer) end\n"
                "end\n"
                "if #targets == 0 then print(\"ERROR:No layers found\") return end\n"
                "\n"
                "app.transaction(function()\n"
                "  for fi = start_idx, end_idx do\n"
                "    local frame = spr.frames[fi]\n"
                "    for _, layer in ipairs(targets) do\n"
                "      if not layer:cel(frame) then\n"
                "        spr:newCel(layer, frame,\n"
                "                   Image(spr.width, spr.height, spr.colorMode),\n"
                "                   Point(0, 0))\n"
                "      end\n"
                "    end\n"
                "  end\n"
                "end)\n" + kSaveOk;
      return script;
    }));

  tools.push_back(LuaTool(
    "validate_scene",
    "Check that the required layers exist and have a cel on every frame of a range "
    "(default: all frames). Prints a JSON report with the sprite frame count, the checked "
    "range, the missing layer names, and the missing layer/frame cels.",
    fileSchema({
      { "required_layers", stringListProp("Layer names that must exist with cels "
                                          "(must not be empty)") },
      { "start_frame", intProp("First frame of the range (default: 1)") },
      { "end_frame", intProp("Last frame of the range, inclusive (default: the last frame)") },
    }, { "filename", "required_layers" }),
    true,
    [](const Json& args) -> std::string {
      const Json& requiredLayers = args["required_layers"];
      if (!requiredLayers.is_array() || requiredLayers.array_items().empty())
        return luaError("Required layers list cannot be empty");

      std::string script = kSpritePreludeLua;
      script += frameRangeOpenEndCheck(argInt(args, "start_frame", 1),
                                       optionalIntLiteral(args, "end_frame"));
      script += kFindLayerLua;
      script += "local names = " + stringListLua(requiredLayers) + "\n"
                "local missing_layers = {}\n"
                "local missing_cels = {}\n"
                "\n"
                "for _, name in ipairs(names) do\n"
                "  local layer = find_layer(spr, name)\n"
                "  if not layer then\n"
                "    table.insert(missing_layers, name)\n"
                "  else\n"
                "    for fi = start_idx, end_idx do\n"
                "      if not layer:cel(spr.frames[fi]) then\n"
                "        table.insert(missing_cels, { layer = name, frame = fi })\n"
                "      end\n"
                "    end\n"
                "  end\n"
                "end\n"
                "\n"
                "print(json.encode({\n"
                "  frames = #spr.frames,\n"
                "  range = { start = start_idx, [\"end\"] = end_idx },\n"
                "  missing_layers = missing_layers,\n"
                "  missing_cels = missing_cels,\n"
                "}))";
      return script;
    }));

  tools.push_back(LuaTool(
    "audit_animation",
    "Inspect an animation for cels outside their layer's declared frame ranges and for "
    "overlaps between the given layer pairs. layer_frame_ranges entries look like "
    "\"layer:1-8,17-24\"; overlap_pairs entries look like \"layerA,layerB\" or "
    "\"layerA:layerB\". Prints a JSON report (frames, summary, overlaps, out_of_range, and "
    "optionally the cels of every frame).",
    fileSchema({
      { "start_frame", intProp("First frame of the range (default: 1)") },
      { "end_frame", intProp("Last frame of the range, inclusive (default: the last frame)") },
      { "layer_names", stringListProp("Layers to inspect (default: every non-group layer)") },
      { "overlap_pairs", stringListProp("Layer pairs whose cels must not overlap, each "
                                        "\"A,B\" or \"A:B\"") },
      { "layer_frame_ranges", stringListProp("Allowed frames per layer, each "
                                             "\"layer:1-8,17-24\"") },
      { "report_cels", boolProp("Include the cels of every frame in the report "
                                "(default: false)") },
      { "report_bounds", boolProp("Include cel bounds in overlaps and cels "
                                  "(default: false)") },
      { "max_overlaps", intProp("Maximum overlap samples to report (default: 200)") },
      { "max_out_of_range", intProp("Maximum out-of-range samples to report "
                                    "(default: 200)") },
    }, { "filename" }),
    true,
    [](const Json& args) -> std::string {
      const int startFrame = argInt(args, "start_frame", 1);
      if (startFrame < 1)
        return luaError("Start frame must be >= 1");
      if (args["end_frame"].is_number() && argInt(args, "end_frame") < startFrame)
        return luaError("End frame must be >= start frame");

      const int maxOverlaps = argInt(args, "max_overlaps", 200);
      const int maxOutOfRange = argInt(args, "max_out_of_range", 200);
      if (maxOverlaps < 0 || maxOutOfRange < 0)
        return luaError("Max limits must be >= 0");

      const bool reportCels = argBool(args, "report_cels", false);
      const bool reportBounds = argBool(args, "report_bounds", false);

      std::string script = kSpritePreludeLua;
      script += frameRangeOpenEndCheck(startFrame, optionalIntLiteral(args, "end_frame"));
      script += kFindLayerLua;
      script += "local target_names = " + optionalStringListLua(args["layer_names"]) + "\n";
      script += kTargetLayersLua;
      script += "local overlap_pairs_list = " + overlapPairsLua(args["overlap_pairs"]) + "\n"
                "local ranges_by_layer = " + layerFrameRangesLua(args["layer_frame_ranges"]) + "\n";
      script += kInRangesLua;
      script += "\n"
                "local overlaps = {}\n"
                "local overlaps_total = 0\n"
                "local overlaps_truncated = false\n"
                "local out_of_range = {}\n"
                "local cel_entries = {}\n"
                "local total_cels = 0\n"
                "\n"
                "for fi = start_idx, end_idx do\n"
                "  local frame = spr.frames[fi]\n"
                "  local frame_cels = {}\n"
                "  for _, layer in ipairs(target_layers) do\n"
                "    local cel = layer:cel(frame)\n"
                "    if cel then\n"
                "      total_cels = total_cels + 1\n"
                "      if " + luaBool(reportCels) + " then\n"
                "        if " + luaBool(reportBounds) + " then\n"
                "          table.insert(frame_cels, {\n"
                "            layer = layer.name,\n"
                "            x = cel.position.x, y = cel.position.y,\n"
                "            w = cel.image.width, h = cel.image.height,\n"
                "          })\n"
                "        else\n"
                "          table.insert(frame_cels, { layer = layer.name })\n"
                "        end\n"
                "      end\n"
                "      if not in_ranges(layer.name, fi) then\n"
                "        if #out_of_range < " + std::to_string(maxOutOfRange) + " then\n"
                "          table.insert(out_of_range, { frame = fi, layer = layer.name })\n"
                "        end\n"
                "      end\n"
                "    end\n"
                "  end\n"
                "  if " + luaBool(reportCels) + " then\n"
                "    table.insert(cel_entries, { frame = fi, cels = frame_cels })\n"
                "  end\n"
                "\n"
                "  for _, pair in ipairs(overlap_pairs_list) do\n"
                "    local layer_a = layer_map[pair[1]]\n"
                "    local layer_b = layer_map[pair[2]]\n"
                "    if layer_a and layer_b then\n"
                "      local cel_a = layer_a:cel(frame)\n"
                "      local cel_b = layer_b:cel(frame)\n"
                "      if cel_a and cel_b then\n"
                "        local a_pos = cel_a.position\n"
                "        local b_pos = cel_b.position\n"
                "        local a_w, a_h = cel_a.image.width, cel_a.image.height\n"
                "        local b_w, b_h = cel_b.image.width, cel_b.image.height\n"
                "        local hit = a_pos.x < b_pos.x + b_w and\n"
                "                    a_pos.x + a_w > b_pos.x and\n"
                "                    a_pos.y < b_pos.y + b_h and\n"
                "                    a_pos.y + a_h > b_pos.y\n"
                "        if hit then\n"
                "          overlaps_total = overlaps_total + 1\n"
                "          if #overlaps < " + std::to_string(maxOverlaps) + " then\n"
                "            local entry = { frame = fi, a = layer_a.name, b = layer_b.name }\n"
                "            if " + luaBool(reportBounds) + " then\n"
                "              entry.a_bounds = { a_pos.x, a_pos.y, a_w, a_h }\n"
                "              entry.b_bounds = { b_pos.x, b_pos.y, b_w, b_h }\n"
                "            end\n"
                "            table.insert(overlaps, entry)\n"
                "          else\n"
                "            overlaps_truncated = true\n"
                "          end\n"
                "        end\n"
                "      end\n"
                "    end\n"
                "  end\n"
                "end\n"
                "\n"
                "local result = {\n"
                "  frames = { start = start_idx, [\"end\"] = end_idx },\n"
                "  summary = {\n"
                "    total_layers = #spr.layers,\n"
                "    layers_checked = #target_layers,\n"
                "    total_cels = total_cels,\n"
                "    overlaps = #overlaps,\n"
                "    overlaps_total = overlaps_total,\n"
                "    overlaps_truncated = overlaps_truncated,\n"
                "    out_of_range = #out_of_range,\n"
                "  },\n"
                "  overlaps = overlaps,\n"
                "  out_of_range = out_of_range,\n"
                "}\n"
                "if " + luaBool(reportCels) + " then result.cels = cel_entries end\n"
                "print(json.encode(result))";
      return script;
    }));

  tools.push_back(LuaTool(
    "animation_sanitize",
    "Normalize an animation and optionally repair it: reorder layers to layer_order, "
    "create cels for ensure_layers, and act on cels outside their layer's declared frame "
    "ranges (set_opacity_zero, delete_cels, or none). overlapping layer pairs are counted, "
    "full-canvas cels optionally ignored. With report_only nothing is changed and the file "
    "is not saved. Prints a JSON report (frames, per-step counts, analysis, layer_stats, "
    "alerts, overlap samples).",
    fileSchema({
      { "start_frame", intProp("First frame of the range (default: 1)") },
      { "end_frame", intProp("Last frame of the range, inclusive (default: the last frame)") },
      { "layer_names", stringListProp("Layers to analyze (default: every non-group layer)") },
      { "layer_order", stringListProp("Desired top-to-bottom layer order, by name") },
      { "layer_frame_ranges", stringListProp("Allowed frames per layer, each "
                                             "\"layer:1-8,17-24\"") },
      { "ensure_layers", stringListProp("Layers whose missing cels are created") },
      { "overlap_pairs", stringListProp("Layer pairs whose cels must not overlap, each "
                                        "\"A,B\" or \"A:B\"") },
      { "report_bounds", boolProp("Include cel bounds in the overlap samples "
                                  "(default: false)") },
      { "max_overlaps", intProp("Maximum overlap samples to report (default: 200)") },
      { "ignore_full_canvas_overlaps", boolProp("Skip overlap checks when a cel is "
                                                "canvas-sized (default: true)") },
      { "out_of_range_action", stringProp("What to do with cels outside their ranges: "
                                          "set_opacity_zero, delete_cels, or none "
                                          "(default: set_opacity_zero)") },
      { "out_of_range_opacity", intProp("Opacity applied by set_opacity_zero, 0-255 "
                                        "(default: 0)") },
      { "report_only", boolProp("Report what would change without changing it "
                                "(default: false)") },
      { "include_stats", boolProp("Include per-layer stats in the report (default: true)") },
    }, { "filename" }),
    true,
    [](const Json& args) -> std::string {
      const int startFrame = argInt(args, "start_frame", 1);
      if (startFrame < 1)
        return luaError("Start frame must be >= 1");
      if (args["end_frame"].is_number() && argInt(args, "end_frame") < startFrame)
        return luaError("End frame must be >= start frame");

      const int maxOverlaps = argInt(args, "max_overlaps", 200);
      if (maxOverlaps < 0)
        return luaError("max_overlaps must be >= 0");

      const std::string action = argString(args, "out_of_range_action", "set_opacity_zero");
      if (action != "set_opacity_zero" && action != "delete_cels" && action != "none")
        return luaError("Unsupported out_of_range_action");

      const int outOfRangeOpacity = argInt(args, "out_of_range_opacity", 0);
      if (outOfRangeOpacity < 0 || outOfRangeOpacity > 255)
        return luaError("out_of_range_opacity must be 0-255");

      const bool reportOnly = argBool(args, "report_only", false);
      const bool reportBounds = argBool(args, "report_bounds", false);
      const bool ignoreFullCanvas = argBool(args, "ignore_full_canvas_overlaps", true);
      const bool includeStats = argBool(args, "include_stats", true);

      std::string script = kSpritePreludeLua;
      script += frameRangeOpenEndCheck(startFrame, optionalIntLiteral(args, "end_frame"));
      script += kFindLayerLua;
      script += kJsonNullLua;
      script += "local target_names = " + optionalStringListLua(args["layer_names"]) + "\n";
      script += kTargetLayersLua;
      script += "local ranges_by_layer = " + layerFrameRangesLua(args["layer_frame_ranges"]) + "\n"
                "local order_names = " + optionalStringListLua(args["layer_order"]) + "\n"
                "local ensure_names = " + optionalStringListLua(args["ensure_layers"]) + "\n"
                "local overlap_pairs_list = " + overlapPairsLua(args["overlap_pairs"]) + "\n";
      script += kInRangesLua;
      script += "\n"
                "local sanitized = {\n"
                "  reordered = false,\n"
                "  ensured = 0,\n"
                "  out_of_range = 0,\n"
                "  opacity_set = 0,\n"
                "  deleted = 0,\n"
                "}\n"
                "local analysis = {\n"
                "  total_layers = #spr.layers,\n"
                "  layers_checked = #target_layers,\n"
                "  total_cels = 0,\n"
                "  empty_frames = 0,\n"
                "  inactive_layers = {},\n"
                "  overlaps = 0,\n"
                "}\n"
                "\n"
                "local layer_activity = {}\n"
                "local layer_stats = {}\n"
                "for _, layer in ipairs(target_layers) do\n"
                "  layer_activity[layer.name] = 0\n"
                "  layer_stats[layer.name] = {\n"
                "    frames_active = 0,\n"
                "    cel_count = 0,\n"
                "    full_canvas_cels = 0,\n"
                "    bounds = JSON_NULL,\n"
                "  }\n"
                "end\n"
                "\n"
                "local overlaps = {}\n"
                "local overlaps_total = 0\n"
                "local overlaps_truncated = false\n"
                "local alerts = {}\n"
                "local changed = false\n"
                "\n"
                "app.transaction(function()\n"
                "  if order_names ~= nil then\n"
                "    local has_groups = false\n"
                "    local parent_ref = nil\n"
                "    for _, layer in ipairs(spr.layers) do\n"
                "      if layer.isGroup then has_groups = true end\n"
                "    end\n"
                "    for _, layer in ipairs(target_layers) do\n"
                "      if parent_ref == nil then\n"
                "        parent_ref = layer.parent\n"
                "      elseif layer.parent ~= parent_ref then\n"
                "        has_groups = true\n"
                "        break\n"
                "      end\n"
                "    end\n"
                "\n"
                "    local ordered = {}\n"
                "    local seen = {}\n"
                "    for _, name in ipairs(order_names) do\n"
                "      local layer = find_layer(spr, name)\n"
                "      if layer and not layer.isGroup and not seen[name] then\n"
                "        table.insert(ordered, layer)\n"
                "        seen[name] = true\n"
                "      end\n"
                "    end\n"
                "    for _, layer in ipairs(spr.layers) do\n"
                "      if not layer.isGroup and not seen[layer.name] then\n"
                "        table.insert(ordered, layer)\n"
                "      end\n"
                "    end\n"
                "    if not has_groups and not " + luaBool(reportOnly) + " then\n"
                "      for idx, layer in ipairs(ordered) do\n"
                "        layer.stackIndex = idx\n"
                "      end\n"
                "      changed = true\n"
                "    end\n"
                "    sanitized.reordered = not has_groups\n"
                "  end\n"
                "\n"
                "  if ensure_names ~= nil then\n"
                "    for _, name in ipairs(ensure_names) do\n"
                "      local layer = find_layer(spr, name)\n"
                "      if layer and not layer.isGroup then\n"
                "        for fi = start_idx, end_idx do\n"
                "          local frame = spr.frames[fi]\n"
                "          if not layer:cel(frame) then\n"
                "            sanitized.ensured = sanitized.ensured + 1\n"
                "            if not " + luaBool(reportOnly) + " then\n"
                "              spr:newCel(layer, frame,\n"
                "                         Image(spr.width, spr.height, spr.colorMode),\n"
                "                         Point(0, 0))\n"
                "              changed = true\n"
                "            end\n"
                "          end\n"
                "        end\n"
                "      end\n"
                "    end\n"
                "  end\n"
                "\n"
                "  for fi = start_idx, end_idx do\n"
                "    local frame = spr.frames[fi]\n"
                "    local has_cel = false\n"
                "    for _, layer in ipairs(target_layers) do\n"
                "      local cel = layer:cel(frame)\n"
                "      if cel then\n"
                "        has_cel = true\n"
                "        analysis.total_cels = analysis.total_cels + 1\n"
                "        layer_activity[layer.name] = (layer_activity[layer.name] or 0) + 1\n"
                "        local stats = layer_stats[layer.name]\n"
                "        if stats then\n"
                "          stats.cel_count = stats.cel_count + 1\n"
                "          stats.frames_active = stats.frames_active + 1\n"
                "          local img = cel.image\n"
                "          local pos = cel.position\n"
                "          if img.width == spr.width and img.height == spr.height then\n"
                "            stats.full_canvas_cels = stats.full_canvas_cels + 1\n"
                "          end\n"
                "          local x1, y1 = pos.x, pos.y\n"
                "          local x2, y2 = pos.x + img.width, pos.y + img.height\n"
                "          if stats.bounds == JSON_NULL then\n"
                "            stats.bounds = { x1, y1, x2, y2 }\n"
                "          else\n"
                "            if x1 < stats.bounds[1] then stats.bounds[1] = x1 end\n"
                "            if y1 < stats.bounds[2] then stats.bounds[2] = y1 end\n"
                "            if x2 > stats.bounds[3] then stats.bounds[3] = x2 end\n"
                "            if y2 > stats.bounds[4] then stats.bounds[4] = y2 end\n"
                "          end\n"
                "        end\n"
                "        if not in_ranges(layer.name, fi) then\n"
                "          sanitized.out_of_range = sanitized.out_of_range + 1\n"
                "          if not " + luaBool(reportOnly) + " then\n"
                "            if \"" + action + "\" == \"delete_cels\" then\n"
                "              spr:deleteCel(cel)\n"
                "              sanitized.deleted = sanitized.deleted + 1\n"
                "              changed = true\n"
                "            elseif \"" + action + "\" == \"set_opacity_zero\" then\n"
                "              cel.opacity = " + std::to_string(outOfRangeOpacity) + "\n"
                "              sanitized.opacity_set = sanitized.opacity_set + 1\n"
                "              changed = true\n"
                "            end\n"
                "          end\n"
                "        end\n"
                "      end\n"
                "    end\n"
                "    if not has_cel then\n"
                "      analysis.empty_frames = analysis.empty_frames + 1\n"
                "    end\n"
                "\n"
                "    for _, pair in ipairs(overlap_pairs_list) do\n"
                "      local layer_a = layer_map[pair[1]]\n"
                "      local layer_b = layer_map[pair[2]]\n"
                "      if layer_a and layer_b then\n"
                "        local cel_a = layer_a:cel(frame)\n"
                "        local cel_b = layer_b:cel(frame)\n"
                "        if cel_a and cel_b then\n"
                "          local skip = false\n"
                "          if " + luaBool(ignoreFullCanvas) + " then\n"
                "            if cel_a.image.width == spr.width and\n"
                "               cel_a.image.height == spr.height then\n"
                "              skip = true\n"
                "            end\n"
                "            if cel_b.image.width == spr.width and\n"
                "               cel_b.image.height == spr.height then\n"
                "              skip = true\n"
                "            end\n"
                "          end\n"
                "          if not skip then\n"
                "            local a_pos = cel_a.position\n"
                "            local b_pos = cel_b.position\n"
                "            local a_w, a_h = cel_a.image.width, cel_a.image.height\n"
                "            local b_w, b_h = cel_b.image.width, cel_b.image.height\n"
                "            local hit = a_pos.x < b_pos.x + b_w and\n"
                "                        a_pos.x + a_w > b_pos.x and\n"
                "                        a_pos.y < b_pos.y + b_h and\n"
                "                        a_pos.y + a_h > b_pos.y\n"
                "            if hit then\n"
                "              analysis.overlaps = analysis.overlaps + 1\n"
                "              overlaps_total = overlaps_total + 1\n"
                "              if #overlaps < " + std::to_string(maxOverlaps) + " then\n"
                "                local entry = { frame = fi, a = layer_a.name, b = layer_b.name }\n"
                "                if " + luaBool(reportBounds) + " then\n"
                "                  entry.a_bounds = { a_pos.x, a_pos.y, a_w, a_h }\n"
                "                  entry.b_bounds = { b_pos.x, b_pos.y, b_w, b_h }\n"
                "                end\n"
                "                table.insert(overlaps, entry)\n"
                "              else\n"
                "                overlaps_truncated = true\n"
                "              end\n"
                "            end\n"
                "          end\n"
                "        end\n"
                "      end\n"
                "    end\n"
                "  end\n"
                "end)\n"
                "\n"
                "for _, layer in ipairs(target_layers) do\n"
                "  if (layer_activity[layer.name] or 0) == 0 then\n"
                "    table.insert(analysis.inactive_layers, layer.name)\n"
                "  end\n"
                "end\n"
                "\n"
                "if not " + luaBool(reportOnly) + " and changed then\n"
                "  spr:saveAs(spr.filename)\n"
                "end\n"
                "\n"
                "local result = {\n"
                "  frames = { start = start_idx, [\"end\"] = end_idx },\n"
                "  reordered = sanitized.reordered,\n"
                "  ensured = sanitized.ensured,\n"
                "  out_of_range = sanitized.out_of_range,\n"
                "  opacity_set = sanitized.opacity_set,\n"
                "  deleted = sanitized.deleted,\n"
                "  analysis = {\n"
                "    total_layers = analysis.total_layers,\n"
                "    layers_checked = analysis.layers_checked,\n"
                "    total_cels = analysis.total_cels,\n"
                "    empty_frames = analysis.empty_frames,\n"
                "    overlaps = analysis.overlaps,\n"
                "    overlaps_total = overlaps_total,\n"
                "    overlaps_truncated = overlaps_truncated,\n"
                "    inactive_layers = analysis.inactive_layers,\n"
                "  },\n"
                "}\n"
                "if " + luaBool(includeStats) + " then result.layer_stats = layer_stats end\n"
                "\n"
                "if analysis.empty_frames > 0 then\n"
                "  table.insert(alerts, \"empty_frames_detected\")\n"
                "end\n"
                "if sanitized.out_of_range > 0 then\n"
                "  table.insert(alerts, \"cels_out_of_range\")\n"
                "end\n"
                "for _, layer in ipairs(target_layers) do\n"
                "  local stats = layer_stats[layer.name]\n"
                "  if stats and stats.full_canvas_cels > 0 then\n"
                "    table.insert(alerts, \"full_canvas_cels:\" .. layer.name)\n"
                "  end\n"
                "end\n"
                "if #alerts > 0 then result.alerts = alerts end\n"
                "if #overlaps > 0 then result.overlap_samples = overlaps end\n"
                "print(json.encode(result))";
      return script;
    }));
}

} // namespace mcp
} // namespace app
