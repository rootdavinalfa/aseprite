// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/mcp/mcp_tools_lua.h"
#include "app/mcp/mcp_tools_registry.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

namespace app {
namespace mcp {

namespace {

using json11::Json;

// Saves the file and reports success; appended after a transaction's
// "end)" or as the tail of a script that manages its own undo step.
const char kSaveOk[] =
  "\n"
  "spr:saveAs(spr.filename)\n"
  "print(\"OK\")";

// A snippet that reports a validation failure; the "ERROR:" prefix
// turns the tool result into an error.
std::string luaError(const std::string& message)
{
  return "print(\"ERROR:" + luaString(message) + "\")";
}

// A named retro palette preset.
struct PalettePreset {
  const char* name;
  std::vector<const char*> colors;
};

const std::vector<PalettePreset>& palettePresets()
{
  static const std::vector<PalettePreset> presets = {
    { "gameboy", { "#0F380F", "#306230", "#8BAC0F", "#9BBC0F" } },
    { "monochrome", { "#000000", "#FFFFFF" } },
    { "grayscale_4", { "#000000", "#555555", "#AAAAAA", "#FFFFFF" } },
    { "cga", { "#000000", "#55FFFF", "#FF55FF", "#FFFFFF" } },
    { "pico8", { "#000000", "#1D2B53", "#7E2553", "#008751",
                 "#AB5236", "#5F574F", "#C2C3C7", "#FFF1E8",
                 "#FF004D", "#FFA300", "#FFEC27", "#00E436",
                 "#29ADFF", "#83769C", "#FF77A8", "#FFCCAA" } },
    { "c64", { "#000000", "#FFFFFF", "#880000", "#AAFFEE",
               "#CC44CC", "#00CC55", "#0000AA", "#EEEE77",
               "#DD8855", "#664400", "#FF7777", "#333333",
               "#777777", "#AAFF66", "#0088FF", "#BBBBBB" } },
    { "dawnbringer16", { "#140C1C", "#442434", "#30346D", "#4E4A4E",
                         "#854C30", "#346524", "#D04648", "#757161",
                         "#597DCE", "#D27D2C", "#8595A1", "#6DAA2C",
                         "#D2AA99", "#6DC2CA", "#DAD45E", "#DEEED6" } },
    { "dawnbringer32", { "#000000", "#222034", "#45283C", "#663931",
                         "#8F563B", "#DF7126", "#D9A066", "#EEC39A",
                         "#FBF236", "#99E550", "#6ABE30", "#37946E",
                         "#4B692F", "#524B24", "#323C39", "#3F3F74",
                         "#306082", "#5B6EE1", "#639BFF", "#5FCDE4",
                         "#CBDBFC", "#FFFFFF", "#9BADB7", "#847E87",
                         "#696A6A", "#595652", "#76428A", "#AC3232",
                         "#D95763", "#D77BBA", "#8F974A", "#8A6F30" } },
  };
  return presets;
}

// Case-insensitive lookup of a palette preset.
const PalettePreset* findPalettePreset(const std::string& name)
{
  std::string lower = name;
  for (char& c : lower)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

  for (const PalettePreset& preset : palettePresets()) {
    if (lower == preset.name)
      return &preset;
  }
  return nullptr;
}

// The comma-separated preset names, sorted, for error messages.
std::string palettePresetNames()
{
  std::vector<std::string> names;
  for (const PalettePreset& preset : palettePresets())
    names.push_back(preset.name);
  std::sort(names.begin(), names.end());

  std::string out;
  for (const std::string& name : names) {
    if (!out.empty())
      out += ", ";
    out += name;
  }
  return out;
}

// A snippet that replaces the sprite palette with the given "#RRGGBB"
// colors and saves the file.
std::string setPaletteScript(const std::vector<std::string>& colors)
{
  std::string script = std::string(kSpritePreludeLua) +
                       "app.transaction(function()\n"
                       "  local pal = Palette(" + std::to_string(colors.size()) + ")\n";
  for (std::size_t i = 0; i < colors.size(); ++i) {
    int r = 0, g = 0, b = 0, a = 0;
    parseHexColor(colors[i], r, g, b, a);
    script += "  pal:setColor(" + std::to_string(i) + ", Color(" + std::to_string(r) + ", " +
              std::to_string(g) + ", " + std::to_string(b) + "))\n";
  }
  script += "  spr:setPalette(pal)\n"
            "end)\n" + std::string(kSaveOk);
  return script;
}

// Resolves a slice by name; requires a local "spr".
const char kFindSliceLua[] =
  "local function find_slice(spr, name)\n"
  "  for _, slice in ipairs(spr.slices) do\n"
  "    if slice.name == name then return slice end\n"
  "  end\n"
  "  return nil\n"
  "end\n";

// Resolves the tilemap layer named by "layer_name" into the local
// "target", checking that it is a tilemap.
std::string tilemapLayerLookup(const std::string& layerName)
{
  return std::string(kFindLayerLua) +
         "local target = find_layer(spr, \"" + luaString(layerName) + "\")\n"
         "if not target then print(\"ERROR:Layer not found\") return end\n"
         "if not target.isTilemap then print(\"ERROR:Layer is not a tilemap layer\") return end\n";
}

// Maps a normalized color channel back to 0-255, like Python's
// colorsys helpers.
int channelToByte(const double value)
{
  const int byte = static_cast<int>(std::lround(value * 255));
  return std::min(255, std::max(0, byte));
}

// Python colorsys rgb_to_hls: hue in [0,1), lightness and saturation in
// [0,1].
void rgbToHls(const double r, const double g, const double b,
              double& h, double& l, double& s)
{
  const double maxc = std::max({ r, g, b });
  const double minc = std::min({ r, g, b });
  l = (maxc + minc) / 2;
  if (maxc == minc) {
    h = 0;
    s = 0;
    return;
  }

  const double d = maxc - minc;
  s = (l <= 0.5) ? d / (maxc + minc) : d / (2 - maxc - minc);
  const double rc = (maxc - r) / d;
  const double gc = (maxc - g) / d;
  const double bc = (maxc - b) / d;
  if (r == maxc)
    h = bc - gc;
  else if (g == maxc)
    h = 2 + rc - bc;
  else
    h = 4 + gc - rc;
  h = std::fmod(h / 6, 1.0);
  if (h < 0)
    h += 1;
}

double hueChannel(const double m1, const double m2, double hue)
{
  hue = std::fmod(hue, 1.0);
  if (hue < 0)
    hue += 1;
  if (hue < 1.0 / 6)
    return m1 + (m2 - m1) * hue * 6;
  if (hue < 0.5)
    return m2;
  if (hue < 2.0 / 3)
    return m1 + (m2 - m1) * (2.0 / 3 - hue) * 6;
  return m1;
}

// Python colorsys hls_to_rgb.
void hlsToRgb(const double h, const double l, const double s,
              int& r, int& g, int& b)
{
  if (s <= 0) {
    const int v = channelToByte(l);
    r = g = b = v;
    return;
  }

  const double m2 = (l <= 0.5) ? l * (1 + s) : l + s - l * s;
  const double m1 = 2 * l - m2;
  r = channelToByte(hueChannel(m1, m2, h + 1.0 / 3));
  g = channelToByte(hueChannel(m1, m2, h));
  b = channelToByte(hueChannel(m1, m2, h - 1.0 / 3));
}

std::string rgbHex(const int r, const int g, const int b)
{
  char buf[8];
  std::snprintf(buf, sizeof(buf), "#%02X%02X%02X", r, g, b);
  return buf;
}

// Starts a script that opens a transaction on the cel of the named
// layer and 1-based frame, in cel-local coordinates: the body uses
// "img" bound to cel.image.
std::string celEditStart(const std::string& layerName, const int frameIndex)
{
  return "local idx = " + std::to_string(frameIndex) + "\n"
         "if idx < 1 or idx > #spr.frames then print(\"ERROR:Frame index out of range\") return end\n" +
         kFindLayerLua +
         "local target = find_layer(spr, \"" + luaString(layerName) + "\")\n"
         "if not target then print(\"ERROR:Layer not found\") return end\n"
         "\n"
         "local cel = target:cel(spr.frames[idx])\n"
         "if not cel then print(\"ERROR:No cel at that layer/frame\") return end\n"
         "\n"
         "app.transaction(function()\n"
         "  local img = cel.image\n";
}

// Swaps the pixels of "img" in place using the snapshot in "pixels".
// "read" indexes the snapshot with the source of the destination
// pixel (px, py).
std::string flipBody(const std::string& read)
{
  return "  local w = img.width\n"
         "  local h = img.height\n"
         "\n"
         "  local pixels = {}\n"
         "  for py = 0, h - 1 do\n"
         "    pixels[py] = {}\n"
         "    for px = 0, w - 1 do\n"
         "      pixels[py][px] = img:getPixel(px, py)\n"
         "    end\n"
         "  end\n"
         "\n"
         "  for py = 0, h - 1 do\n"
         "    for px = 0, w - 1 do\n"
         "      img:putPixel(px, py, " + read + ")\n"
         "    end\n"
         "  end\n";
}

} // anonymous namespace

void register_palette_transform_tools(std::vector<McpTool>& tools)
{
  tools.push_back(LuaTool(
    "flip_layer",
    "Flip the image of one cel horizontally or vertically, in place. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to flip, by name or group/child path" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "direction", json11::Json::object {
            { "type", "string" },
            { "description", "\"horizontal\" or \"vertical\" (default: \"horizontal\")" } } },
      } },
      { "required", json11::Json::array { "filename", "layer_name", "frame_index" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const std::string direction = argString(args, "direction", "horizontal");
      if (direction != "horizontal" && direction != "vertical")
        return std::string("print(\"ERROR:direction must be 'horizontal' or 'vertical'\")");

      const std::string read = direction == "horizontal" ? "pixels[py][w - 1 - px]"
                                                         : "pixels[h - 1 - py][px]";
      return std::string(kSpritePreludeLua) +
             celEditStart(argString(args, "layer_name"), argInt(args, "frame_index")) +
             flipBody(read) + "end)\n" + kSaveOk;
    }));

  tools.push_back(LuaTool(
    "rotate_layer",
    "Rotate the image of one cel 90, 180, or 270 degrees clockwise. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to rotate, by name or group/child path" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "angle", json11::Json::object {
            { "type", "integer" },
            { "description", "Rotation in degrees clockwise: 90, 180, or 270 (default: 90)" } } },
      } },
      { "required", json11::Json::array { "filename", "layer_name", "frame_index" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int angle = argInt(args, "angle", 90);
      if (angle != 90 && angle != 180 && angle != 270)
        return std::string("print(\"ERROR:angle must be 90, 180, or 270\")");

      std::string body;
      if (angle == 180) {
        body = flipBody("pixels[h - 1 - py][w - 1 - px]");
      }
      else if (angle == 90) {
        body = "  local new_img = Image(img.height, img.width, img.colorMode)\n"
               "  for py = 0, img.height - 1 do\n"
               "    for px = 0, img.width - 1 do\n"
               "      new_img:putPixel(img.height - 1 - py, px, img:getPixel(px, py))\n"
               "    end\n"
               "  end\n"
               "  cel.image = new_img\n";
      }
      else {
        body = "  local new_img = Image(img.height, img.width, img.colorMode)\n"
               "  for py = 0, img.height - 1 do\n"
               "    for px = 0, img.width - 1 do\n"
               "      new_img:putPixel(py, img.width - 1 - px, img:getPixel(px, py))\n"
               "    end\n"
               "  end\n"
               "  cel.image = new_img\n";
      }

      return std::string(kSpritePreludeLua) +
             celEditStart(argString(args, "layer_name"), argInt(args, "frame_index")) +
             body + "end)\n" + kSaveOk;
    }));

  tools.push_back(LuaTool(
    "resize_canvas",
    "Resize the canvas and scale all its content to new dimensions. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "width", json11::Json::object {
            { "type", "integer" },
            { "description", "New canvas width in pixels (must be > 0)" } } },
        { "height", json11::Json::object {
            { "type", "integer" },
            { "description", "New canvas height in pixels (must be > 0)" } } },
      } },
      { "required", json11::Json::array { "filename", "width", "height" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int width = argInt(args, "width");
      const int height = argInt(args, "height");
      if (width <= 0 || height <= 0)
        return std::string("print(\"ERROR:Width and height must be > 0\")");

      return std::string(kSpritePreludeLua) +
             "app.transaction(function()\n"
             "  spr:resize(" + std::to_string(width) + ", " + std::to_string(height) + ")\n"
             "end)\n" + kSaveOk;
    }));

  tools.push_back(LuaTool(
    "crop_canvas",
    "Crop the canvas to the given rectangle, discarding content outside it. The file is "
    "saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "x", json11::Json::object {
            { "type", "integer" },
            { "description", "Left edge of the crop area" } } },
        { "y", json11::Json::object {
            { "type", "integer" },
            { "description", "Top edge of the crop area" } } },
        { "width", json11::Json::object {
            { "type", "integer" },
            { "description", "Crop width in pixels (must be > 0)" } } },
        { "height", json11::Json::object {
            { "type", "integer" },
            { "description", "Crop height in pixels (must be > 0)" } } },
      } },
      { "required", json11::Json::array { "filename", "x", "y", "width", "height" } },
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
             "if " + std::to_string(x) + " >= spr.width or " + std::to_string(y) +
             " >= spr.height or " + std::to_string(x + width) + " <= 0 or " +
             std::to_string(y + height) + " <= 0 then\n"
             "  print(\"ERROR:Crop rect is fully outside the canvas\")\n"
             "  return\n"
             "end\n"
             "\n"
             "app.transaction(function()\n"
             "  spr:crop(" + std::to_string(x) + ", " + std::to_string(y) + ", " +
             std::to_string(width) + ", " + std::to_string(height) + ")\n"
             "end)\n" + kSaveOk;
    }));

  // Palette.

  tools.push_back(LuaTool(
    "get_palette",
    "Read the sprite's first palette as a JSON array of #RRGGBB hex strings.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to read" } } },
      } },
      { "required", json11::Json::array { "filename" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      return std::string(kSpritePreludeLua) +
             "local ok, pal = pcall(function() return spr.palettes[1] end)\n"
             "if not ok or not pal then print(\"ERROR:No palette\") return end\n"
             "\n"
             "local colors = {}\n"
             "for i = 0, #pal - 1 do\n"
             "  local c = pal:getColor(i)\n"
             "  colors[#colors + 1] = string.format(\"#%02X%02X%02X\", c.red, c.green, c.blue)\n"
             "end\n"
             "print(json.encode(colors))";
    }));

  tools.push_back(LuaTool(
    "set_palette",
    "Replace the sprite palette with the given hex colors (#RGB, #RGBA, #RRGGBB, or "
    "#RRGGBBAA). The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "colors", json11::Json::object {
            { "type", "array" },
            { "description", "Palette entries as hex colors, in order" },
            { "items", json11::Json::object { { "type", "string" } } } } },
      } },
      { "required", json11::Json::array { "filename", "colors" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const Json& list = args["colors"];
      if (!list.is_array() || list.array_items().empty())
        return luaError("Colors list cannot be empty");

      std::vector<std::string> colors;
      for (const Json& color : list.array_items()) {
        int r = 0, g = 0, b = 0, a = 0;
        if (!color.is_string() || !parseHexColor(color.string_value(), r, g, b, a))
          return luaError("Colors must use #RRGGBB values");
        colors.push_back(color.string_value());
      }
      return setPaletteScript(colors);
    }));

  tools.push_back(LuaTool(
    "remap_colors_in_cel_range",
    "Remap exact RGB colors on one layer across a frame range, keeping each pixel's "
    "alpha. mappings is an array of {\"from\", \"to\"} hex pairs; with "
    "create_missing_cels missing cels are cloned from source_frame_index (default: "
    "start_frame). The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to remap, by name or group/child path" } } },
        { "start_frame", json11::Json::object {
            { "type", "integer" },
            { "description", "First frame index to remap" } } },
        { "end_frame", json11::Json::object {
            { "type", "integer" },
            { "description", "Last frame index to remap" } } },
        { "mappings", json11::Json::object {
            { "type", "array" },
            { "description", "Color mappings, each {\"from\": \"#RRGGBB\", \"to\": "
                             "\"#RRGGBB\"}" },
            { "items", json11::Json::object { { "type", "object" } } } } },
        { "create_missing_cels", json11::Json::object {
            { "type", "boolean" },
            { "description", "Create cels missing in the range (default: false)" } } },
        { "source_frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame to clone when creating a cel (default: start_frame)" } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "start_frame", "end_frame", "mappings" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const Json& list = args["mappings"];
      if (!list.is_array() || list.array_items().empty())
        return luaError("Mappings list cannot be empty");

      std::string mappings;
      for (const Json& mapping : list.array_items()) {
        int sr = 0, sg = 0, sb = 0, sa = 0;
        int dr = 0, dg = 0, db = 0, da = 0;
        if (!parseHexColor(argString(mapping, "from"), sr, sg, sb, sa) ||
            !parseHexColor(argString(mapping, "to"), dr, dg, db, da))
          return luaError("Mappings must use #RRGGBB colors");

        if (!mappings.empty())
          mappings += ", ";
        mappings += "{" + std::to_string(sr) + "," + std::to_string(sg) + "," +
                    std::to_string(sb) + "," + std::to_string(dr) + "," + std::to_string(dg) +
                    "," + std::to_string(db) + "}";
      }

      const Json& sourceFrame = args["source_frame_index"];
      const std::string sourceFrameLiteral =
        sourceFrame.is_number() ? std::to_string(sourceFrame.int_value()) : "nil";
      const std::string layerName = luaString(argString(args, "layer_name"));

      return std::string(kSpritePreludeLua) +
             "local start_idx = " + std::to_string(argInt(args, "start_frame")) + "\n"
             "local end_idx = " + std::to_string(argInt(args, "end_frame")) + "\n"
             "if start_idx < 1 or end_idx > #spr.frames or start_idx > end_idx then\n"
             "  print(\"ERROR:Frame range out of bounds\") return\n"
             "end\n"
             "\n" + kFindLayerLua +
             "local target = find_layer(spr, \"" + layerName + "\")\n"
             "if not target then print(\"ERROR:Layer not found\") return end\n"
             "\n"
             "local source_frame = " + sourceFrameLiteral + "\n"
             "if source_frame == nil then source_frame = start_idx end\n"
             "if source_frame < 1 or source_frame > #spr.frames then\n"
             "  print(\"ERROR:Source frame out of range\") return\n"
             "end\n"
             "\n"
             "local map = { " + mappings + " }\n"
             "\n"
             "app.transaction(function()\n"
             "  for fi = start_idx, end_idx do\n"
             "    local frame = spr.frames[fi]\n"
             "    local cel = target:cel(frame)\n"
             "    if not cel and " + std::string(argBool(args, "create_missing_cels", false) ?
                                              "true" : "false") + " then\n"
             "      local source_cel = target:cel(spr.frames[source_frame])\n"
             "      if source_cel then\n"
             "        local img = source_cel.image:clone()\n"
             "        cel = spr:newCel(target, frame, img, source_cel.position)\n"
             "      else\n"
             "        local img = Image(spr.width, spr.height, spr.colorMode)\n"
             "        cel = spr:newCel(target, frame, img, Point(0, 0))\n"
             "      end\n"
             "    end\n"
             "    if cel then\n"
             "      local img = cel.image\n"
             "      for y = 0, img.height - 1 do\n"
             "        for x = 0, img.width - 1 do\n"
             "          local c = img:getPixel(x, y)\n"
             "          local r = app.pixelColor.rgbaR(c)\n"
             "          local g = app.pixelColor.rgbaG(c)\n"
             "          local b = app.pixelColor.rgbaB(c)\n"
             "          local a = app.pixelColor.rgbaA(c)\n"
             "          if a > 0 then\n"
             "            for _, m in ipairs(map) do\n"
             "              if r == m[1] and g == m[2] and b == m[3] then\n"
             "                img:putPixel(x, y, app.pixelColor.rgba(m[4], m[5], m[6], a))\n"
             "                break\n"
             "              end\n"
             "            end\n"
             "          end\n"
             "        end\n"
             "      end\n"
             "    end\n"
             "  end\n"
             "end)\n" + kSaveOk;
    }));

  tools.push_back(NativeTool(
    "list_palette_presets",
    "List the built-in retro palettes as a JSON object from preset name to its "
    "#RRGGBB colors: gameboy, monochrome, grayscale_4, cga, pico8, c64, "
    "dawnbringer16, dawnbringer32.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {} },
      { "required", json11::Json::array {} },
    },
    [](const json11::Json&) -> ToolResult {
      json11::Json::object out;
      for (const PalettePreset& preset : palettePresets()) {
        json11::Json::array colors;
        for (const char* color : preset.colors)
          colors.push_back(color);
        out[preset.name] = colors;
      }

      ToolResult result;
      result.text = json11::Json(out).dump();
      return result;
    }));

  tools.push_back(LuaTool(
    "apply_palette_preset",
    "Set the sprite palette to a built-in retro preset (gameboy, monochrome, "
    "grayscale_4, cga, pico8, c64, dawnbringer16, dawnbringer32). Existing pixels keep "
    "their colors; use quantize_to_palette to snap them. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "preset", json11::Json::object {
            { "type", "string" },
            { "description", "Preset name, case-insensitive" } } },
      } },
      { "required", json11::Json::array { "filename", "preset" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const std::string name = argString(args, "preset");
      const PalettePreset* preset = findPalettePreset(name);
      if (!preset)
        return luaError("Unknown preset '" + name + "'. Available: " + palettePresetNames());

      const std::vector<std::string> colors(preset->colors.begin(), preset->colors.end());
      return setPaletteScript(colors);
    }));

  tools.push_back(NativeTool(
    "generate_color_ramp",
    "Generate a hue-shifted shading ramp from a base color, ordered darkest to "
    "lightest, as a JSON array of #RRGGBB. Shadows lean cooler and highlights warmer, "
    "the standard pixel-art trick that reads better than plain darker/lighter shades.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "base_color", json11::Json::object {
            { "type", "string" },
            { "description", "Hex color the ramp is built around, e.g. #D04648" } } },
        { "steps", json11::Json::object {
            { "type", "integer" },
            { "description", "Number of colors in the ramp, 2-16 (default: 5)" } } },
        { "hue_shift_degrees", json11::Json::object {
            { "type", "number" },
            { "description", "Total hue rotation across the ramp in degrees "
                             "(default: 20)" } } },
        { "lightness_range", json11::Json::object {
            { "type", "number" },
            { "description", "Total lightness span across the ramp, 0-1 (default: 0.5)" } } },
      } },
      { "required", json11::Json::array { "base_color" } },
    },
    [](const json11::Json& args) -> ToolResult {
      const std::string baseColor = argString(args, "base_color");
      int br = 0, bg = 0, bb = 0, ba = 0;
      if (!parseHexColor(baseColor, br, bg, bb, ba)) {
        ToolResult result;
        result.isError = true;
        result.text = "Invalid color value: " + baseColor;
        return result;
      }

      const int steps = argInt(args, "steps", 5);
      const double hueShift = argDouble(args, "hue_shift_degrees", 20.0);
      const double lightnessRange = argDouble(args, "lightness_range", 0.5);

      ToolResult result;
      if (steps < 2 || steps > 16) {
        result.isError = true;
        result.text = "steps must be between 2 and 16";
        return result;
      }
      if (lightnessRange < 0 || lightnessRange > 1) {
        result.isError = true;
        result.text = "lightness_range must be between 0 and 1";
        return result;
      }

      double h = 0, l = 0, s = 0;
      rgbToHls(br / 255.0, bg / 255.0, bb / 255.0, h, l, s);

      json11::Json::array ramp;
      const double mid = (steps - 1) / 2.0;
      for (int i = 0; i < steps; ++i) {
        // t in [-0.5, 0.5]: negative = shadow side, positive = highlight side.
        const double t = (i - mid) / (steps - 1);
        double nh = std::fmod(h - t * (hueShift / 360.0), 1.0);
        if (nh < 0)
          nh += 1;
        const double nl = std::min(1.0, std::max(0.0, l + t * lightnessRange));
        // Shadows are slightly more saturated, highlights slightly less.
        const double ns = std::min(1.0, std::max(0.0, s - t * 0.15));

        int r = 0, g = 0, b = 0;
        hlsToRgb(nh, nl, ns, r, g, b);
        ramp.push_back(rgbHex(r, g, b));
      }

      result.text = json11::Json(ramp).dump();
      return result;
    }));

  tools.push_back(LuaTool(
    "quantize_to_palette",
    "Snap opaque pixels to the nearest color in the sprite palette by RGB distance. "
    "layer_name selects one layer (empty = every image layer) and the frame range "
    "defaults to the whole sprite. Run after set_palette or apply_palette_preset to "
    "make existing art conform. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to quantize (default: empty = all image layers)" } } },
        { "start_frame", json11::Json::object {
            { "type", "integer" },
            { "description", "First frame to process (default: 1)" } } },
        { "end_frame", json11::Json::object {
            { "type", "integer" },
            { "description", "Last frame to process (default: 0 = last frame)" } } },
      } },
      { "required", json11::Json::array { "filename" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const std::string layerName = luaString(argString(args, "layer_name"));
      return std::string(kSpritePreludeLua) + kFindLayerLua +
             "local ok, pal = pcall(function() return spr.palettes[1] end)\n"
             "if not ok or not pal or #pal == 0 then print(\"ERROR:No palette\") return end\n"
             "\n"
             "local start_idx = " + std::to_string(argInt(args, "start_frame", 1)) + "\n"
             "local end_idx = " + std::to_string(argInt(args, "end_frame", 0)) + "\n"
             "if end_idx < 1 then end_idx = #spr.frames end\n"
             "if start_idx < 1 or end_idx > #spr.frames or start_idx > end_idx then\n"
             "  print(\"ERROR:Frame range out of bounds\") return\n"
             "end\n"
             "\n"
             "local layers = {}\n"
             "if \"" + layerName + "\" ~= \"\" then\n"
             "  local target = find_layer(spr, \"" + layerName + "\")\n"
             "  if not target then print(\"ERROR:Layer not found\") return end\n"
             "  table.insert(layers, target)\n"
             "else\n"
             "  for _, layer in ipairs(spr.layers) do\n"
             "    if layer.isImage then table.insert(layers, layer) end\n"
             "  end\n"
             "end\n"
             "\n"
             "local colors = {}\n"
             "for i = 0, #pal - 1 do\n"
             "  local c = pal:getColor(i)\n"
             "  table.insert(colors, { c.red, c.green, c.blue })\n"
             "end\n"
             "\n"
             "local cache = {}\n"
             "local function nearest(r, g, b)\n"
             "  local key = r * 65536 + g * 256 + b\n"
             "  local hit = cache[key]\n"
             "  if hit then return hit end\n"
             "  local best, best_d = colors[1], math.huge\n"
             "  for _, c in ipairs(colors) do\n"
             "    local dr, dg, db = r - c[1], g - c[2], b - c[3]\n"
             "    local d = dr * dr + dg * dg + db * db\n"
             "    if d < best_d then best, best_d = c, d end\n"
             "  end\n"
             "  cache[key] = best\n"
             "  return best\n"
             "end\n"
             "\n"
             "local count = 0\n"
             "app.transaction(function()\n"
             "  for _, layer in ipairs(layers) do\n"
             "    for fi = start_idx, end_idx do\n"
             "      local cel = layer:cel(spr.frames[fi])\n"
             "      if cel then\n"
             "        local img = cel.image\n"
             "        for py = 0, img.height - 1 do\n"
             "          for px = 0, img.width - 1 do\n"
             "            local v = img:getPixel(px, py)\n"
             "            local a = app.pixelColor.rgbaA(v)\n"
             "            if a > 0 then\n"
             "              local r = app.pixelColor.rgbaR(v)\n"
             "              local g = app.pixelColor.rgbaG(v)\n"
             "              local b = app.pixelColor.rgbaB(v)\n"
             "              local c = nearest(r, g, b)\n"
             "              if c[1] ~= r or c[2] ~= g or c[3] ~= b then\n"
             "                img:putPixel(px, py, app.pixelColor.rgba(c[1], c[2], c[3], a))\n"
             "                count = count + 1\n"
             "              end\n"
             "            end\n"
             "          end\n"
             "        end\n"
             "      end\n"
             "    end\n"
             "  end\n"
             "end)\n"
             "\n"
             "spr:saveAs(spr.filename)\n"
             "print(\"COUNT:\" .. count)";
    }));

  tools.push_back(LuaTool(
    "set_color_mode",
    "Convert the sprite to rgb, grayscale, or indexed color mode. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "mode", json11::Json::object {
            { "type", "string" },
            { "description", "\"rgb\", \"grayscale\", or \"indexed\" (case-insensitive)" } } },
      } },
      { "required", json11::Json::array { "filename", "mode" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      std::string mode = argString(args, "mode");
      for (char& c : mode)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (mode != "rgb" && mode != "grayscale" && mode != "indexed")
        return luaError("mode must be 'rgb', 'grayscale', or 'indexed'");

      return std::string(kSpritePreludeLua) +
             "app.command.ChangePixelFormat { format = \"" + mode + "\" }\n" + kSaveOk;
    }));

  // Slices.

  tools.push_back(LuaTool(
    "create_slice",
    "Create a named slice, a rectangle game engines can read (buttons, 9-patches, "
    "attack hitboxes). Names must be unique. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "name", json11::Json::object {
            { "type", "string" },
            { "description", "Slice name (cannot be empty)" } } },
        { "x", json11::Json::object { { "type", "integer" },
                                      { "description", "Left edge of the slice" } } },
        { "y", json11::Json::object { { "type", "integer" },
                                      { "description", "Top edge of the slice" } } },
        { "width", json11::Json::object {
            { "type", "integer" },
            { "description", "Slice width in pixels (must be > 0)" } } },
        { "height", json11::Json::object {
            { "type", "integer" },
            { "description", "Slice height in pixels (must be > 0)" } } },
      } },
      { "required", json11::Json::array {
        "filename", "name", "x", "y", "width", "height" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const std::string name = argString(args, "name");
      if (name.empty())
        return luaError("Slice name cannot be empty");
      const int width = argInt(args, "width");
      const int height = argInt(args, "height");
      if (width <= 0 || height <= 0)
        return luaError("Width and height must be > 0");

      const std::string safeName = luaString(name);
      return std::string(kSpritePreludeLua) + kFindSliceLua +
             "if find_slice(spr, \"" + safeName + "\") then\n"
             "  print(\"ERROR:Slice with that name already exists\") return\n"
             "end\n"
             "\n"
             "app.transaction(function()\n"
             "  local slice = spr:newSlice(Rectangle(" + std::to_string(argInt(args, "x")) +
             ", " + std::to_string(argInt(args, "y")) + ", " + std::to_string(width) + ", " +
             std::to_string(height) + "))\n"
             "  slice.name = \"" + safeName + "\"\n"
             "end)\n" + kSaveOk;
    }));

  tools.push_back(LuaTool(
    "set_slice_center",
    "Set a slice's 9-patch center rectangle, relative to the slice origin: the "
    "stretchable region engines repeat when scaling. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "name", json11::Json::object {
            { "type", "string" },
            { "description", "Slice name" } } },
        { "x", json11::Json::object {
            { "type", "integer" },
            { "description", "Center-rect left edge, relative to the slice" } } },
        { "y", json11::Json::object {
            { "type", "integer" },
            { "description", "Center-rect top edge, relative to the slice" } } },
        { "width", json11::Json::object {
            { "type", "integer" },
            { "description", "Center-rect width in pixels (must be > 0)" } } },
        { "height", json11::Json::object {
            { "type", "integer" },
            { "description", "Center-rect height in pixels (must be > 0)" } } },
      } },
      { "required", json11::Json::array {
        "filename", "name", "x", "y", "width", "height" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int width = argInt(args, "width");
      const int height = argInt(args, "height");
      if (width <= 0 || height <= 0)
        return luaError("Width and height must be > 0");

      const std::string safeName = luaString(argString(args, "name"));
      return std::string(kSpritePreludeLua) + kFindSliceLua +
             "local slice = find_slice(spr, \"" + safeName + "\")\n"
             "if not slice then print(\"ERROR:Slice not found\") return end\n"
             "\n"
             "app.transaction(function()\n"
             "  slice.center = Rectangle(" + std::to_string(argInt(args, "x")) + ", " +
             std::to_string(argInt(args, "y")) + ", " + std::to_string(width) + ", " +
             std::to_string(height) + ")\n"
             "end)\n" + kSaveOk;
    }));

  tools.push_back(LuaTool(
    "set_slice_pivot",
    "Set a slice's pivot point, relative to the slice origin. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "name", json11::Json::object {
            { "type", "string" },
            { "description", "Slice name" } } },
        { "x", json11::Json::object {
            { "type", "integer" },
            { "description", "Pivot x, relative to the slice" } } },
        { "y", json11::Json::object {
            { "type", "integer" },
            { "description", "Pivot y, relative to the slice" } } },
      } },
      { "required", json11::Json::array { "filename", "name", "x", "y" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const std::string safeName = luaString(argString(args, "name"));
      return std::string(kSpritePreludeLua) + kFindSliceLua +
             "local slice = find_slice(spr, \"" + safeName + "\")\n"
             "if not slice then print(\"ERROR:Slice not found\") return end\n"
             "\n"
             "app.transaction(function()\n"
             "  slice.pivot = Point(" + std::to_string(argInt(args, "x")) + ", " +
             std::to_string(argInt(args, "y")) + ")\n"
             "end)\n" + kSaveOk;
    }));

  tools.push_back(LuaTool(
    "list_slices",
    "List every slice as a JSON array of {name, x, y, width, height}, with center and "
    "pivot added when the slice has them.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to read" } } },
      } },
      { "required", json11::Json::array { "filename" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      return std::string(kSpritePreludeLua) +
             "local out = {}\n"
             "for _, slice in ipairs(spr.slices) do\n"
             "  local b = slice.bounds\n"
             "  local item = { name = slice.name, x = b.x, y = b.y,\n"
             "                 width = b.width, height = b.height }\n"
             "  local c = slice.center\n"
             "  if c then\n"
             "    item.center = { x = c.x, y = c.y, width = c.width, height = c.height }\n"
             "  end\n"
             "  local p = slice.pivot\n"
             "  if p then\n"
             "    item.pivot = { x = p.x, y = p.y }\n"
             "  end\n"
             "  out[#out + 1] = item\n"
             "end\n"
             "print(json.encode(out))";
    }));

  tools.push_back(LuaTool(
    "delete_slice",
    "Delete a slice by name. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "name", json11::Json::object {
            { "type", "string" },
            { "description", "Slice name to delete" } } },
      } },
      { "required", json11::Json::array { "filename", "name" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const std::string safeName = luaString(argString(args, "name"));
      return std::string(kSpritePreludeLua) + kFindSliceLua +
             "local slice = find_slice(spr, \"" + safeName + "\")\n"
             "if not slice then print(\"ERROR:Slice not found\") return end\n"
             "\n"
             "app.transaction(function()\n"
             "  spr:deleteSlice(slice)\n"
             "end)\n" + kSaveOk;
    }));

  // Tilemap.

  tools.push_back(LuaTool(
    "create_tilemap_layer",
    "Create a tilemap layer with its own tileset, setting the sprite grid to the tile "
    "size. Tile index 0 is always the empty tile; add real tiles with draw_on_tile. "
    "The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Name for the new tilemap layer" } } },
        { "tile_width", json11::Json::object {
            { "type", "integer" },
            { "description", "Tile width in pixels (must be > 0)" } } },
        { "tile_height", json11::Json::object {
            { "type", "integer" },
            { "description", "Tile height in pixels (must be > 0)" } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "tile_width", "tile_height" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int tileWidth = argInt(args, "tile_width");
      const int tileHeight = argInt(args, "tile_height");
      if (tileWidth <= 0 || tileHeight <= 0)
        return luaError("Tile dimensions must be > 0");

      const std::string layerName = luaString(argString(args, "layer_name"));
      // The duplicate-name guard is top-level only: the layer is created
      // at the root, and Aseprite allows the same name inside a group.
      return std::string(kSpritePreludeLua) +
             "local name_taken = false\n"
             "for _, layer in ipairs(spr.layers) do\n"
             "  if layer.name == \"" + layerName + "\" then name_taken = true break end\n"
             "end\n"
             "if name_taken then print(\"ERROR:Layer with that name already exists\") return end\n"
             "\n"
             "app.transaction(function()\n"
             "  spr.gridBounds = Rectangle(0, 0, " + std::to_string(tileWidth) + ", " +
             std::to_string(tileHeight) + ")\n"
             "  app.command.NewLayer { tilemap = true }\n"
             "  app.activeLayer.name = \"" + layerName + "\"\n"
             "end)\n" + kSaveOk;
    }));

  tools.push_back(LuaTool(
    "draw_on_tile",
    "Paint pixels onto one tile of a tilemap layer's tileset, in tile-local "
    "coordinates (0,0 = tile top-left). Passing tile_index equal to the current tile "
    "count appends a new tile; tile 0 is the reserved empty tile. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Tilemap layer whose tileset to edit" } } },
        { "tile_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Tile to draw on (1 = first real tile; 0 is reserved)" } } },
        { "pixels", json11::Json::object {
            { "type", "array" },
            { "description", "Pixels to draw, each {\"x\", \"y\", \"color\": \"#RRGGBB\"}" },
            { "items", json11::Json::object { { "type", "object" } } } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "tile_index", "pixels" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int tileIndex = argInt(args, "tile_index");
      if (tileIndex < 1)
        return luaError("tile_index must be >= 1 (tile 0 is the reserved empty tile)");

      const Json& pixels = args["pixels"];
      if (!pixels.is_array() || pixels.array_items().empty())
        return luaError("Pixels list cannot be empty");

      std::string puts;
      for (const Json& pixel : pixels.array_items()) {
        int r = 0, g = 0, b = 0, a = 0;
        if (!parseHexColor(argString(pixel, "color"), r, g, b, a))
          return luaError("Invalid color value: " + argString(pixel, "color"));

        puts += "  pset(img, " + std::to_string(argInt(pixel, "x")) + ", " +
                std::to_string(argInt(pixel, "y")) + ", app.pixelColor.rgba(" +
                std::to_string(r) + ", " + std::to_string(g) + ", " + std::to_string(b) +
                ", 255))\n";
      }

      return std::string(kSpritePreludeLua) +
             tilemapLayerLookup(argString(args, "layer_name")) + kPSetLua +
             "\n"
             "local ts = target.tileset\n"
             "if not ts then print(\"ERROR:Layer has no tileset\") return end\n"
             "\n"
             "local idx = " + std::to_string(tileIndex) + "\n"
             "if idx > #ts then\n"
             "  print(\"ERROR:tile_index out of range (tileset has \" .. (#ts - 1) ..\n"
             "        \" real tiles; pass \" .. #ts .. \" to append)\")\n"
             "  return\n"
             "end\n"
             "\n"
             "app.transaction(function()\n"
             "  if idx == #ts then\n"
             "    spr:newTile(ts)\n"
             "  end\n"
             "  local tile = ts:tile(idx)\n"
             "  local img = tile.image:clone()\n" + puts + "  tile.image = img\n"
             "end)\n" + kSaveOk;
    }));

  tools.push_back(LuaTool(
    "set_tiles",
    "Place tiles on a tilemap layer by grid cell: each entry is {\"col\", \"row\", "
    "\"tile_index\"} and tile_index 0 clears the cell. The cel is created or resized "
    "to the grid when needed. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Tilemap layer to edit" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "tiles", json11::Json::object {
            { "type", "array" },
            { "description", "Tiles to place, each {\"col\", \"row\", \"tile_index\"}" },
            { "items", json11::Json::object { { "type", "object" } } } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "frame_index", "tiles" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const Json& tiles = args["tiles"];
      if (!tiles.is_array() || tiles.array_items().empty())
        return luaError("Tiles list cannot be empty");

      std::string entries;
      for (const Json& tile : tiles.array_items()) {
        if (!entries.empty())
          entries += ", ";
        entries += "{" + std::to_string(argInt(tile, "col")) + "," +
                   std::to_string(argInt(tile, "row")) + "," +
                   std::to_string(argInt(tile, "tile_index")) + "}";
      }

      return std::string(kSpritePreludeLua) +
             "local idx = " + std::to_string(argInt(args, "frame_index")) + "\n"
             "if idx < 1 or idx > #spr.frames then\n"
             "  print(\"ERROR:Frame index out of range\") return\n"
             "end\n"
             "\n" +
             tilemapLayerLookup(argString(args, "layer_name")) +
             "\n"
             "local ts = target.tileset\n"
             "local grid = spr.gridBounds\n"
             "local tw, th = grid.width, grid.height\n"
             "local cols = math.ceil(spr.width / tw)\n"
             "local rows = math.ceil(spr.height / th)\n"
             "\n"
             "local tiles = { " + entries + " }\n"
             "for _, t in ipairs(tiles) do\n"
             "  if t[1] < 0 or t[1] >= cols or t[2] < 0 or t[2] >= rows then\n"
             "    print(\"ERROR:Tile position (\" .. t[1] .. \",\" .. t[2] .. \") outside the \" ..\n"
             "          cols .. \"x\" .. rows .. \" map\")\n"
             "    return\n"
             "  end\n"
             "  if t[3] < 0 or t[3] >= #ts then\n"
             "    print(\"ERROR:tile_index \" .. t[3] .. \" out of range (tileset has \" ..\n"
             "          (#ts - 1) .. \" real tiles)\")\n"
             "    return\n"
             "  end\n"
             "end\n"
             "\n"
             "app.transaction(function()\n"
             "  local frame = spr.frames[idx]\n"
             "  local cel = target:cel(frame)\n"
             "  local img\n"
             "  if cel and cel.image.width == cols and cel.image.height == rows and\n"
             "     cel.position.x == 0 and cel.position.y == 0 then\n"
             "    img = cel.image\n"
             "  else\n"
             "    img = Image(cols, rows, ColorMode.TILEMAP)\n"
             "    if cel then\n"
             "      local old = cel.image\n"
             "      local ox = cel.position.x // tw\n"
             "      local oy = cel.position.y // th\n"
             "      for y = 0, old.height - 1 do\n"
             "        for x = 0, old.width - 1 do\n"
             "          local nx, ny = x + ox, y + oy\n"
             "          if nx >= 0 and ny >= 0 and nx < cols and ny < rows then\n"
             "            img:putPixel(nx, ny, old:getPixel(x, y))\n"
             "          end\n"
             "        end\n"
             "      end\n"
             "    end\n"
             "    cel = spr:newCel(target, frame, img, Point(0, 0))\n"
             "  end\n"
             "  for _, t in ipairs(tiles) do\n"
             "    img:putPixel(t[1], t[2], t[3])\n"
             "  end\n"
             "  cel.image = img\n"
             "end)\n" + kSaveOk;
    }));

  tools.push_back(LuaTool(
    "get_tile_at",
    "Read the tile placed at a grid cell of a tilemap layer as JSON {col, row, "
    "tile_index}; 0 means the cell is empty.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to read" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Tilemap layer to read" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "col", json11::Json::object {
            { "type", "integer" },
            { "description", "Grid column, 0-based" } } },
        { "row", json11::Json::object {
            { "type", "integer" },
            { "description", "Grid row, 0-based" } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "frame_index", "col", "row" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const std::string col = std::to_string(argInt(args, "col"));
      const std::string row = std::to_string(argInt(args, "row"));
      return std::string(kSpritePreludeLua) +
             "local idx = " + std::to_string(argInt(args, "frame_index")) + "\n"
             "if idx < 1 or idx > #spr.frames then\n"
             "  print(\"ERROR:Frame index out of range\") return\n"
             "end\n"
             "\n" +
             tilemapLayerLookup(argString(args, "layer_name")) +
             "\n"
             "local grid = spr.gridBounds\n"
             "local cel = target:cel(spr.frames[idx])\n"
             "local tile = 0\n"
             "if cel then\n"
             "  local cx = " + col + " - cel.position.x // grid.width\n"
             "  local cy = " + row + " - cel.position.y // grid.height\n"
             "  if cx >= 0 and cy >= 0 and cx < cel.image.width and cy < cel.image.height then\n"
             "    tile = app.pixelColor.tileI(cel.image:getPixel(cx, cy))\n"
             "  end\n"
             "end\n"
             "print(json.encode({ col = " + col + ", row = " + row +
             ", tile_index = tile }))";
    }));

  tools.push_back(LuaTool(
    "get_tilemap_info",
    "Read a tilemap layer's tile size, real tile count (excluding the empty tile 0), "
    "and map size as JSON {tile_width, tile_height, tile_count, map_cols, map_rows}.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to read" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Tilemap layer to inspect" } } },
      } },
      { "required", json11::Json::array { "filename", "layer_name" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      return std::string(kSpritePreludeLua) +
             tilemapLayerLookup(argString(args, "layer_name")) +
             "\n"
             "local ts = target.tileset\n"
             "local grid = spr.gridBounds\n"
             "print(json.encode({\n"
             "  tile_width = grid.width,\n"
             "  tile_height = grid.height,\n"
             "  tile_count = #ts - 1,\n"
             "  map_cols = math.ceil(spr.width / grid.width),\n"
             "  map_rows = math.ceil(spr.height / grid.height),\n"
             "}))";
    }));
}

} // namespace mcp
} // namespace app
