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

#include <string>

namespace app {
namespace mcp {

namespace {

// Saves the file and reports success; appended after a transaction's
// "end)" or as the tail of a script that manages its own undo step.
const char kSaveOk[] =
  "\n"
  "spr:saveAs(spr.filename)\n"
  "print(\"OK\")";

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
}

} // namespace mcp
} // namespace app
