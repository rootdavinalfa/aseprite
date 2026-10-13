// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/mcp/mcp_tools_registry.h"

#include <string>

namespace app {
namespace mcp {

namespace {

// Renders a frame of the flattened clone into a canvas-sized RGB image,
// so layer offsets and trimmed cels don't skew the read.
const char kFlattenFrame[] =
  "local function flatten_frame(clone, frame_idx)\n"
  "  local layer = clone.layers[#clone.layers]\n"
  "  local img = Image(clone.width, clone.height, ColorMode.RGB)\n"
  "  local cel = layer:cel(clone.frames[frame_idx])\n"
  "  if cel then\n"
  "    img:drawImage(cel.image, cel.position)\n"
  "  end\n"
  "  return img\n"
  "end\n";

// Reads img:getPixel(cx, cy) into r, g, b, a locals, leaving zeroes
// (transparent black) when the cel-local coordinates fall outside.
std::string readPixelLua(const std::string& celX, const std::string& celY)
{
  return "  local r, g, b, a = 0, 0, 0, 0\n"
         "  if " + celX + " >= 0 and " + celY + " >= 0 and " +
         celX + " < iw and " + celY + " < ih then\n"
         "    local pv = img:getPixel(" + celX + ", " + celY + ")\n"
         "    r = app.pixelColor.rgbaR(pv)\n"
         "    g = app.pixelColor.rgbaG(pv)\n"
         "    b = app.pixelColor.rgbaB(pv)\n"
         "    a = app.pixelColor.rgbaA(pv)\n"
         "  end\n";
}

// Selects the cel to read from: an explicitly named layer when
// layer_name is non-empty, otherwise the active cel of the frame.
// Leaves the sprite in "spr", the frame index in "idx", the image in
// "img", its size in "iw"/"ih", and the cel offset in "ox"/"oy".
std::string readPixelScript(const std::string& layerName, const int frameIndex)
{
  std::string script =
    "local spr = app.activeSprite\n"
    "if not spr then print(\"ERROR:No active sprite\") return end\n"
    "\n"
    "local idx = " + std::to_string(frameIndex) + "\n"
    "if idx < 1 or idx > #spr.frames then print(\"ERROR:Frame index out of range\") return end\n"
    "\n";

  if (layerName.empty()) {
    script +=
      "app.activeFrame = spr.frames[idx]\n"
      "local cel = app.activeCel\n"
      "if not cel then print(\"ERROR:No active cel\") return end\n"
      "local img = cel.image\n"
      "local iw, ih = img.width, img.height\n"
      "local ox, oy = cel.position.x, cel.position.y\n";
  }
  else {
    script +=
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
      "  return nil\n"
      "end\n"
      "\n"
      "local target = find_layer(spr, \"" + luaString(layerName) + "\")\n"
      "if not target then print(\"ERROR:Layer not found\") return end\n"
      "local cel = target:cel(spr.frames[idx])\n"
      "if not cel then print(\"ERROR:No cel at that layer/frame\") return end\n"
      "local img = cel.image\n"
      "local iw, ih = img.width, img.height\n"
      "local ox, oy = cel.position.x, cel.position.y\n";
  }
  return script;
}

} // anonymous namespace

void register_io_tools(std::vector<McpTool>& tools)
{
  tools.push_back(LuaTool(
    "get_pixel_color",
    "Read the RGBA color of one pixel as \"#RRGGBB (r=..., g=..., b=..., a=...)\". "
    "Coordinates are sprite-global; layer_name defaults to the active layer, and reading "
    "outside the cel yields transparent black.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to read" } } },
        { "x", json11::Json::object { { "type", "integer" },
                                      { "description", "X coordinate" } } },
        { "y", json11::Json::object { { "type", "integer" },
                                      { "description", "Y coordinate" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to read from (default: active layer)" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1 (default: 1)" } } },
      } },
      { "required", json11::Json::array { "filename", "x", "y" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      return readPixelScript(argString(args, "layer_name"), argInt(args, "frame_index", 1)) +
             "local px, py = " + std::to_string(argInt(args, "x")) + " - ox, " +
             std::to_string(argInt(args, "y")) + " - oy\n" +
             readPixelLua("px", "py") +
             "print(string.format(\"#%02x%02x%02x (r=%d, g=%d, b=%d, a=%d)\", r, g, b, r, g, b, a))";
    }));

  tools.push_back(LuaTool(
    "get_pixels_rect",
    "Read a rectangle of pixels (sprite-global coordinates) as a JSON array of "
    "{x, y, hex, r, g, b, a} objects, one per pixel in row-major order.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to read" } } },
        { "x", json11::Json::object { { "type", "integer" },
                                      { "description", "Top-left x coordinate" } } },
        { "y", json11::Json::object { { "type", "integer" },
                                      { "description", "Top-left y coordinate" } } },
        { "width", json11::Json::object {
            { "type", "integer" },
            { "description", "Region width in pixels (must be > 0)" } } },
        { "height", json11::Json::object {
            { "type", "integer" },
            { "description", "Region height in pixels (must be > 0)" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to read from (default: active layer)" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1 (default: 1)" } } },
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
      return readPixelScript(argString(args, "layer_name"), argInt(args, "frame_index", 1)) +
             "local pixels = {}\n"
             "for py = " + std::to_string(y) + ", " + std::to_string(y + height - 1) + " do\n"
             "  for px = " + std::to_string(x) + ", " + std::to_string(x + width - 1) + " do\n" +
             readPixelLua("px - ox", "py - oy") +
             "    table.insert(pixels, {\n"
             "      x = px, y = py,\n"
             "      hex = string.format(\"#%02x%02x%02x\", r, g, b),\n"
             "      r = r, g = g, b = b, a = a,\n"
             "    })\n"
             "  end\n"
             "end\n"
             "print(json.encode(pixels))";
    }));

  tools.push_back(LuaTool(
    "get_composite_pixel",
    "Read the RGBA color visible at a pixel after compositing every visible layer (flattened "
    "clone), as \"#RRGGBB (r=..., g=..., b=..., a=...)\". Coordinates are sprite-global.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to read" } } },
        { "x", json11::Json::object { { "type", "integer" },
                                      { "description", "X coordinate" } } },
        { "y", json11::Json::object { { "type", "integer" },
                                      { "description", "Y coordinate" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1 (default: 1)" } } },
      } },
      { "required", json11::Json::array { "filename", "x", "y" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int frameIndex = argInt(args, "frame_index", 1);
      return std::string(kFlattenFrame) +
             "local spr = app.activeSprite\n"
             "if not spr then print(\"ERROR:No active sprite\") return end\n"
             "\n"
             "local idx = " + std::to_string(frameIndex) + "\n"
             "if idx < 1 or idx > #spr.frames then print(\"ERROR:Frame index out of range\") return end\n"
             "\n"
             "local clone = Sprite(spr)\n"
             "clone:flatten()\n"
             "local img = flatten_frame(clone, idx)\n"
             "local iw, ih = img.width, img.height\n"
             "local px, py = " + std::to_string(argInt(args, "x")) + ", " +
             std::to_string(argInt(args, "y")) + "\n" +
             readPixelLua("px", "py") +
             "print(string.format(\"#%02x%02x%02x (r=%d, g=%d, b=%d, a=%d)\", r, g, b, r, g, b, a))";
    }));

  tools.push_back(LuaTool(
    "get_composite_rect",
    "Read a rectangle of composed pixels (all visible layers flattened, sprite-global "
    "coordinates) as a JSON array of {x, y, hex, r, g, b, a} objects in row-major order.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to read" } } },
        { "x", json11::Json::object { { "type", "integer" },
                                      { "description", "Top-left x coordinate" } } },
        { "y", json11::Json::object { { "type", "integer" },
                                      { "description", "Top-left y coordinate" } } },
        { "width", json11::Json::object {
            { "type", "integer" },
            { "description", "Region width in pixels (must be > 0)" } } },
        { "height", json11::Json::object {
            { "type", "integer" },
            { "description", "Region height in pixels (must be > 0)" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1 (default: 1)" } } },
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
      const int frameIndex = argInt(args, "frame_index", 1);
      return std::string(kFlattenFrame) +
             "local spr = app.activeSprite\n"
             "if not spr then print(\"ERROR:No active sprite\") return end\n"
             "\n"
             "local idx = " + std::to_string(frameIndex) + "\n"
             "if idx < 1 or idx > #spr.frames then print(\"ERROR:Frame index out of range\") return end\n"
             "\n"
             "local clone = Sprite(spr)\n"
             "clone:flatten()\n"
             "local img = flatten_frame(clone, idx)\n"
             "local iw, ih = img.width, img.height\n"
             "local pixels = {}\n"
             "for py = " + std::to_string(y) + ", " + std::to_string(y + height - 1) + " do\n"
             "  for px = " + std::to_string(x) + ", " + std::to_string(x + width - 1) + " do\n" +
             readPixelLua("px", "py") +
             "    table.insert(pixels, {\n"
             "      x = px, y = py,\n"
             "      hex = string.format(\"#%02x%02x%02x\", r, g, b),\n"
             "      r = r, g = g, b = b, a = a,\n"
             "    })\n"
             "  end\n"
             "end\n"
             "print(json.encode(pixels))";
    }));
}

} // namespace mcp
} // namespace app
