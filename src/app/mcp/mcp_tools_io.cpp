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

#include "base/base64.h"
#include "base/fs.h"
#include "base/string.h"

#include <fstream>
#include <iterator>
#include <string>

namespace app {
namespace mcp {

namespace {

// A snippet that reports a validation failure; the "ERROR:" prefix
// turns the tool result into an error.
std::string luaError(const std::string& message)
{
  return "print(\"ERROR:" + luaString(message) + "\")";
}

// True when path ends with the given extension, ignoring case. Used to
// decide whether an output name needs its extension appended, and
// whether the produced file is a PNG.
bool hasExtension(const std::string& path, const std::string& ext)
{
  return (path.size() >= ext.size() &&
          base::utf8_icmp(path.substr(path.size() - ext.size()), ext) == 0);
}

std::string outputWithExtension(const std::string& path, const std::string& ext)
{
  return hasExtension(path, ext) ? path : path + ext;
}

// Reads a PNG file written by a tool snippet and attaches its contents
// to the result as base64 image content. Missing or unreadable files
// are ignored silently (the text result already reports the failure).
bool attachPng(const std::string& path, ToolResult& result)
{
  std::ifstream in(path, std::ios::binary);
  if (!in)
    return false;

  const std::string data((std::istreambuf_iterator<char>(in)),
                         std::istreambuf_iterator<char>());
  if (data.empty())
    return false;

  result.imageMime = "image/png";
  result.imageBase64 = base::encode_base64(data);
  return true;
}

// Defines wrote_output(path): true when the file exists, accepting the
// frame-numbered siblings Aseprite writes when a multi-frame sprite is
// saved as a still image (e.g. out1.png, out2.png for out.png).
const char kWroteOutputLua[] =
  "local function wrote_output(path)\n"
  "  if app.fs.isFile(path) then return true end\n"
  "  local dir = app.fs.filePath(path)\n"
  "  if dir == \"\" then dir = \".\" end\n"
  "  local title = app.fs.fileTitle(path)\n"
  "  local ext = \".\" .. app.fs.fileExtension(path)\n"
  "  for _, name in ipairs(app.fs.listFiles(dir)) do\n"
  "    if string.sub(name, 1, #title) == title and\n"
  "       string.lower(string.sub(name, -#ext)) == string.lower(ext) then\n"
  "      return true\n"
  "    end\n"
  "  end\n"
  "  return false\n"
  "end\n";

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
    script += std::string(kFindLayerLua) +
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

  tools.push_back(LuaTool(
    "export_sprite",
    "Export the sprite to another format as a copy; the open document keeps its file. The "
    "format name is lower-cased and appended to output_filename when the name doesn't use it "
    "already (png, gif, etc.). Saving a multi-frame sprite as a still image writes "
    "frame-numbered siblings next to the requested name. PNG output is returned as image content.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to export" } } },
        { "output_filename", json11::Json::object {
            { "type", "string" },
            { "description", "Output file path" } } },
        { "format", json11::Json::object {
            { "type", "string" },
            { "description", "Output format, e.g. \"png\" or \"gif\" (default: \"png\")" } } },
      } },
      { "required", json11::Json::array { "filename", "output_filename" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const std::string format = base::string_to_lower(argString(args, "format", "png"));
      const std::string output = outputWithExtension(argString(args, "output_filename"),
                                                     "." + format);
      const std::string absOutput = base::get_absolute_path(output);
      return std::string(kWroteOutputLua) +
             "local spr = app.activeSprite\n"
             "if not spr then print(\"ERROR:No active sprite\") return end\n"
             "\n"
             "spr:saveCopyAs(\"" + luaString(absOutput) + "\")\n"
             "if not wrote_output(\"" + luaString(absOutput) + "\") then\n"
             "  print(\"ERROR:Failed to export sprite: no file was written\")\n"
             "  return\n"
             "end\n"
             "print(\"Sprite exported successfully to " + luaString(output) + "\")";
    },
    [](const json11::Json& args, ToolResult& result) {
      const std::string format = base::string_to_lower(argString(args, "format", "png"));
      const std::string output = outputWithExtension(argString(args, "output_filename"),
                                                     "." + format);
      if (hasExtension(output, ".png"))
        attachPng(base::get_absolute_path(output), result);
    }));

  tools.push_back(LuaTool(
    "copy_sprite",
    "Copy the sprite to a new .aseprite file (the .aseprite extension is appended when "
    "missing). Fails when the output exists unless overwrite is true.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to copy" } } },
        { "output_filename", json11::Json::object {
            { "type", "string" },
            { "description", "Output .aseprite file path" } } },
        { "overwrite", json11::Json::object {
            { "type", "boolean" },
            { "description", "Replace the output when it already exists (default: false)" } } },
      } },
      { "required", json11::Json::array { "filename", "output_filename" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const std::string output = outputWithExtension(argString(args, "output_filename"),
                                                     ".aseprite");
      const std::string absOutput = base::get_absolute_path(output);
      if (base::is_file(absOutput) && !argBool(args, "overwrite"))
        return luaError("Output file " + output + " already exists");

      return "local spr = app.activeSprite\n"
             "if not spr then print(\"ERROR:No active sprite\") return end\n"
             "\n"
             "spr:saveCopyAs(\"" + luaString(absOutput) + "\")\n"
             "if not app.fs.isFile(\"" + luaString(absOutput) + "\") then\n"
             "  print(\"ERROR:Failed to copy sprite: no file was written\")\n"
             "  return\n"
             "end\n"
             "print(\"Sprite copied to " + luaString(output) + "\")";
    }));

  tools.push_back(LuaTool(
    "export_frame",
    "Export one frame as a PNG, optionally scaled up by an integer nearest-neighbor factor. "
    "Useful for visual feedback while drawing: export at scale 8-10 and inspect the returned "
    "image content, then keep iterating.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to export" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "output_filename", json11::Json::object {
            { "type", "string" },
            { "description", "Output PNG path (.png is appended when missing)" } } },
        { "scale", json11::Json::object {
            { "type", "integer" },
            { "description", "Integer nearest-neighbor scale factor, 1-64 (default: 1)" } } },
      } },
      { "required", json11::Json::array { "filename", "frame_index", "output_filename" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int scale = argInt(args, "scale", 1);
      if (scale < 1 || scale > 64)
        return luaError("scale must be between 1 and 64");

      const int frameIndex = argInt(args, "frame_index");
      const std::string output = outputWithExtension(argString(args, "output_filename"), ".png");
      const std::string absOutput = base::get_absolute_path(output);
      const std::string scaleInt = std::to_string(scale);
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
             "if " + scaleInt + " > 1 then\n"
             "  img:resize{ width = img.width * " + scaleInt + ", height = img.height * " +
             scaleInt + ", method = \"nearest-neighbor\" }\n"
             "end\n"
             "img:saveAs(\"" + luaString(absOutput) + "\")\n"
             "if not app.fs.isFile(\"" + luaString(absOutput) + "\") then\n"
             "  print(\"ERROR:Failed to export frame: no file was written\")\n"
             "  return\n"
             "end\n"
             "print(\"Frame " + std::to_string(frameIndex) + " exported to " +
             luaString(output) + " at " + scaleInt + "x\")";
    },
    [](const json11::Json& args, ToolResult& result) {
      const std::string output = outputWithExtension(argString(args, "output_filename"), ".png");
      attachPng(base::get_absolute_path(output), result);
    }));

  tools.push_back(LuaTool(
    "export_spritesheet",
    "Export the frames as a sprite sheet PNG, optionally with a JSON metadata file and "
    "optionally limited to one animation tag. Layouts: horizontal, vertical, rows, columns, "
    "packed; scale scales the frames (nearest neighbor) before packing and padding separates "
    "them. The sheet is returned as image content.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to export" } } },
        { "output_filename", json11::Json::object {
            { "type", "string" },
            { "description", "Output sheet PNG path (.png is appended when missing)" } } },
        { "sheet_type", json11::Json::object {
            { "type", "string" },
            { "description", "Layout: \"horizontal\", \"vertical\", \"rows\", \"columns\" or "
                             "\"packed\" (default: \"horizontal\")" } } },
        { "data_filename", json11::Json::object {
            { "type", "string" },
            { "description", "Optional JSON metadata output path (default: no data file)" } } },
        { "scale", json11::Json::object {
            { "type", "integer" },
            { "description", "Integer nearest-neighbor scale applied before packing, 1-64 "
                             "(default: 1)" } } },
        { "padding", json11::Json::object {
            { "type", "integer" },
            { "description", "Pixels between frames (default: 0)" } } },
        { "tag_name", json11::Json::object {
            { "type", "string" },
            { "description", "Only include the frames of this animation tag (default: all)" } } },
        { "data_format", json11::Json::object {
            { "type", "string" },
            { "description", "JSON metadata layout: \"json-array\" or \"json-hash\" "
                             "(default: \"json-array\")" } } },
        { "list_tags", json11::Json::object {
            { "type", "boolean" },
            { "description", "Include animation tags in the JSON metadata (default: false)" } } },
      } },
      { "required", json11::Json::array { "filename", "output_filename" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const std::string sheetType = argString(args, "sheet_type", "horizontal");
      if (sheetType != "horizontal" && sheetType != "vertical" && sheetType != "rows" &&
          sheetType != "columns" && sheetType != "packed")
        return luaError("sheet_type must be one of: horizontal, vertical, rows, columns, packed");

      const int scale = argInt(args, "scale", 1);
      if (scale < 1 || scale > 64)
        return luaError("scale must be between 1 and 64");

      const int padding = argInt(args, "padding", 0);
      if (padding < 0)
        return luaError("padding must be >= 0");

      const std::string dataFormat = argString(args, "data_format", "json-array");
      if (dataFormat != "json-array" && dataFormat != "json-hash")
        return luaError("data_format must be 'json-array' or 'json-hash'");

      const std::string output = outputWithExtension(argString(args, "output_filename"), ".png");
      const std::string absOutput = base::get_absolute_path(output);
      const std::string data = argString(args, "data_filename");
      const std::string absData = data.empty() ? std::string() : base::get_absolute_path(data);
      const std::string tagName = argString(args, "tag_name");
      const bool listTags = argBool(args, "list_tags");

      std::string script =
        "local spr = app.activeSprite\n"
        "if not spr then print(\"ERROR:No active sprite\") return end\n"
        "\n";
      if (!tagName.empty()) {
        script +=
          "local found = false\n"
          "for _, t in ipairs(spr.tags) do\n"
          "  if t.name == \"" + luaString(tagName) + "\" then found = true break end\n"
          "end\n"
          "if not found then print(\"ERROR:Failed to resolve tag: Tag not found\") return end\n"
          "\n";
      }
      script +=
        "local params = {\n"
        "  ui = false,\n"
        "  type = \"" + sheetType + "\",\n"
        "  textureFilename = \"" + luaString(absOutput) + "\",\n"
        "  dataFormat = \"" + dataFormat + "\",\n"
        "  shapePadding = " + std::to_string(padding) + ",\n"
        "  listTags = " + (listTags ? "true" : "false") + ",\n"
        "  listLayers = false,\n"
        "  listSlices = false,\n"
        "}\n";
      if (!absData.empty())
        script += "params.dataFilename = \"" + luaString(absData) + "\"\n";
      if (!tagName.empty())
        script += "params.tag = \"" + luaString(tagName) + "\"\n";
      if (scale > 1) {
        script +=
          "local resized = app.command.SpriteSize{ scale = " + std::to_string(scale) +
          ", method = \"nearest-neighbor\", ui = false }\n"
          "if not resized then print(\"ERROR:failed to resize sprite for export\") return end\n";
      }
      else {
        script += "local resized = false\n";
      }
      script +=
        "local ok, result = pcall(function() return app.command.ExportSpriteSheet(params) end)\n"
        // Undo the temporary resize only when it actually happened:
        // SpriteSize reports failure by returning false (it can be
        // disabled), and an unconditional undo would then pop an
        // unrelated previous undo state.
        "if resized then app.undo() end\n";
      script +=
        "if not ok then print(\"ERROR:Failed to export sprite sheet: \" .. tostring(result)) return end\n"
        "if not app.fs.isFile(\"" + luaString(absOutput) + "\") then\n"
        "  print(\"ERROR:Failed to export sprite sheet: no sheet file was written\")\n"
        "  return\n"
        "end\n";
      if (!absData.empty()) {
        script +=
          "if not app.fs.isFile(\"" + luaString(absData) + "\") then\n"
          "  print(\"ERROR:Failed to export sprite sheet: no data file was written\")\n"
          "  return\n"
          "end\n";
      }
      script += "print(\"Sprite sheet exported to " + luaString(output) + " (" + sheetType + ")\"";
      if (!absData.empty())
        script += " .. \" with data file " + luaString(data) + "\"";
      script += ")";
      return script;
    },
    [](const json11::Json& args, ToolResult& result) {
      attachPng(base::get_absolute_path(outputWithExtension(argString(args, "output_filename"), ".png")),
                result);
    }));

  tools.push_back(LuaTool(
    "export_layers",
    "Export every (visible, or with include_hidden every) layer as its own PNG, named after "
    "the layer; multi-frame sprites get a frame-numbered PNG per layer. Canvases are composited "
    "at the sprite size, so trimmed cels and layer offsets don't shift the result.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to export" } } },
        { "output_directory", json11::Json::object {
            { "type", "string" },
            { "description", "Directory for the per-layer PNGs (created when missing)" } } },
        { "include_hidden", json11::Json::object {
            { "type", "boolean" },
            { "description", "Also export hidden layers (default: false)" } } },
      } },
      { "required", json11::Json::array { "filename", "output_directory" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const std::string dir = base::get_absolute_path(argString(args, "output_directory"));
      const std::string includeHidden = argBool(args, "include_hidden") ? "true" : "false";
      return
        "local spr = app.activeSprite\n"
        "if not spr then print(\"ERROR:No active sprite\") return end\n"
        "\n"
        "local outdir = \"" + luaString(dir) + "\"\n"
        "if not app.fs.isDirectory(outdir) then app.fs.makeAllDirectories(outdir) end\n"
        "\n"
        "local paths = {}\n"
        "local function walk(layers, path)\n"
        "  for i, layer in ipairs(layers) do\n"
        "    path[#path + 1] = i\n"
        "    if layer.isGroup then\n"
        "      walk(layer.layers, path)\n"
        "    else\n"
        "      local copy = {}\n"
        "      for j = 1, #path do copy[j] = path[j] end\n"
        "      table.insert(paths, { path = copy, layer = layer })\n"
        "    end\n"
        "    path[#path] = nil\n"
        "  end\n"
        "end\n"
        "walk(spr.layers, {})\n"
        "\n"
        "local frames = #spr.frames\n"
        "for _, item in ipairs(paths) do\n"
        "  if " + includeHidden + " or item.layer.isVisible then\n"
        "    local clone = Sprite(spr)\n"
        "    local function prune(layers, depth)\n"
        "      for i = #layers, 1, -1 do\n"
        "        local layer = layers[i]\n"
        "        if i == item.path[depth] then\n"
        "          if layer.isGroup then prune(layer.layers, depth + 1) end\n"
        "        else\n"
        "          clone:deleteLayer(layer)\n"
        "        end\n"
        "      end\n"
        "    end\n"
        "    prune(clone.layers, 1)\n"
        "    clone:flatten()\n"
        "    local flat = clone.layers[#clone.layers]\n"
        "    for i = 1, frames do\n"
        "      local img = Image(clone.width, clone.height, ColorMode.RGB)\n"
        "      local cel = flat:cel(clone.frames[i])\n"
        "      if cel then img:drawImage(cel.image, cel.position) end\n"
        "      img:saveAs(outdir .. \"/\" .. item.layer.name ..\n"
        "                 (frames > 1 and tostring(i) or \"\") .. \".png\")\n"
        "    end\n"
        "  end\n"
        "end\n"
        "\n"
        "local produced = {}\n"
        "for _, name in ipairs(app.fs.listFiles(outdir)) do\n"
        "  if string.lower(string.sub(name, -4)) == \".png\" then table.insert(produced, name) end\n"
        "end\n"
        "table.sort(produced)\n"
        "if #produced == 0 then\n"
        "  print(\"ERROR:Failed to export layers: no PNG files were written\")\n"
        "  return\n"
        "end\n"
        "print(\"Layers exported to " + luaString(dir) + ": \" .. table.concat(produced, \", \"))";
    }));

  tools.push_back(LuaTool(
    "export_tag",
    "Export the frames of one animation tag to a file, e.g. an animated GIF or a PNG "
    "sequence (the format follows the output extension), optionally scaled with an integer "
    "factor. The tag must exist; missing tags are reported instead of exporting everything.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to export" } } },
        { "tag_name", json11::Json::object {
            { "type", "string" },
            { "description", "Animation tag to export" } } },
        { "output_filename", json11::Json::object {
            { "type", "string" },
            { "description", "Output path; .gif exports an animation, .png a sequence" } } },
        { "scale", json11::Json::object {
            { "type", "integer" },
            { "description", "Integer nearest-neighbor scale factor, 1-64 (default: 1)" } } },
      } },
      { "required", json11::Json::array { "filename", "tag_name", "output_filename" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int scale = argInt(args, "scale", 1);
      if (scale < 1 || scale > 64)
        return luaError("scale must be between 1 and 64");

      const std::string tagName = argString(args, "tag_name");
      const std::string output = argString(args, "output_filename");
      const std::string absOutput = base::get_absolute_path(output);
      return std::string(kWroteOutputLua) +
             "local spr = app.activeSprite\n"
             "if not spr then print(\"ERROR:No active sprite\") return end\n"
             "\n"
             "local found = false\n"
             "for _, t in ipairs(spr.tags) do\n"
             "  if t.name == \"" + luaString(tagName) + "\" then found = true break end\n"
             "end\n"
             "if not found then print(\"ERROR:Failed to export tag: Tag not found\") return end\n"
             "\n"
             "app.command.SaveFileCopyAs{ filename = \"" + luaString(absOutput) +
             "\", tag = \"" + luaString(tagName) + "\", scale = " + std::to_string(scale) +
             ", ui = false }\n"
             "if not wrote_output(\"" + luaString(absOutput) + "\") then\n"
             "  print(\"ERROR:Failed to export tag: no file was written\")\n"
             "  return\n"
             "end\n"
             "print(\"Tag '" + luaString(tagName) + "' exported to " + luaString(output) + "\")";
    }));

  tools.push_back(LuaTool(
    "import_image_as_layer",
    "Import an image file (PNG, etc.) into a layer of the sprite at the given frame and "
    "position, creating the layer when it doesn't exist yet. Useful for reference images or "
    "pre-made parts; works best in RGB color mode. The sprite is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "image_path", json11::Json::object {
            { "type", "string" },
            { "description", "Image file to import" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to place the image on (created when missing)" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1 (default: 1)" } } },
        { "x", json11::Json::object {
            { "type", "integer" },
            { "description", "X position of the image's top-left corner (default: 0)" } } },
        { "y", json11::Json::object {
            { "type", "integer" },
            { "description", "Y position of the image's top-left corner (default: 0)" } } },
      } },
      { "required", json11::Json::array { "filename", "image_path", "layer_name" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const std::string imagePath = argString(args, "image_path");
      const std::string absImage = base::get_absolute_path(imagePath);
      if (!base::is_file(absImage))
        return luaError("Image " + imagePath + " not found");

      const std::string layerName = argString(args, "layer_name");
      return std::string(kFindLayerLua) + std::string(kNormalizeCelLua) +
             "local spr = app.activeSprite\n"
             "if not spr then print(\"ERROR:No active sprite\") return end\n"
             "\n"
             "local idx = " + std::to_string(argInt(args, "frame_index", 1)) + "\n"
             "if idx < 1 or idx > #spr.frames then print(\"ERROR:Frame index out of range\") return end\n"
             "\n"
             "local src = Image{ fromFile = \"" + luaString(absImage) + "\" }\n"
             "if not src then print(\"ERROR:Could not load image\") return end\n"
             "\n"
             "local target = find_layer(spr, \"" + luaString(layerName) + "\")\n"
             "app.transaction(function()\n"
             "  if not target then\n"
             "    target = spr:newLayer()\n"
             "    target.name = \"" + luaString(layerName) + "\"\n"
             "  end\n"
             "  local cel = normalize_cel(spr, target, spr.frames[idx], true)\n"
             "  cel.image:drawImage(src, Point(" + std::to_string(argInt(args, "x")) + ", " +
             std::to_string(argInt(args, "y")) + "))\n"
             "end)\n"
             "\n"
             "spr:saveAs(spr.filename)\n"
             "print(\"OK\")";
    }));

  tools.push_back(LuaTool(
    "render_onion_skin",
    "Render one frame with its neighboring frames as translucent onion-skin ghosts over a "
    "white background, scaled up for inspection — a batch-mode preview of motion continuity.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to read" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame to render, starting at 1" } } },
        { "output_filename", json11::Json::object {
            { "type", "string" },
            { "description", "Output PNG path (.png is appended when missing)" } } },
        { "before", json11::Json::object {
            { "type", "integer" },
            { "description", "Number of previous frames to ghost (default: 1)" } } },
        { "after", json11::Json::object {
            { "type", "integer" },
            { "description", "Number of following frames to ghost (default: 1)" } } },
        { "scale", json11::Json::object {
            { "type", "integer" },
            { "description", "Integer nearest-neighbor scale factor, 1-64 (default: 4)" } } },
        { "ghost_opacity", json11::Json::object {
            { "type", "integer" },
            { "description", "Ghost opacity, 0-255 (default: 100)" } } },
      } },
      { "required", json11::Json::array { "filename", "frame_index", "output_filename" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int frameIndex = argInt(args, "frame_index");
      const int before = argInt(args, "before", 1);
      const int after = argInt(args, "after", 1);
      const int scale = argInt(args, "scale", 4);
      const int ghostOpacity = argInt(args, "ghost_opacity", 100);
      if (scale < 1 || scale > 64)
        return luaError("scale must be between 1 and 64");
      if (ghostOpacity < 0 || ghostOpacity > 255)
        return luaError("ghost_opacity must be between 0 and 255");
      if (before < 0 || after < 0)
        return luaError("before and after must be >= 0");

      const std::string output = outputWithExtension(argString(args, "output_filename"), ".png");
      const std::string absOutput = base::get_absolute_path(output);
      const std::string ghostOpacityInt = std::to_string(ghostOpacity);
      return std::string(kFlattenFrame) +
             "local spr = app.activeSprite\n"
             "if not spr then print(\"ERROR:No active sprite\") return end\n"
             "\n"
             "local idx = " + std::to_string(frameIndex) + "\n"
             "if idx < 1 or idx > #spr.frames then print(\"ERROR:Frame index out of range\") return end\n"
             "\n"
             "local clone = Sprite(spr)\n"
             "clone:flatten()\n"
             "\n"
             "local white = app.pixelColor.rgba(255, 255, 255, 255)\n"
             "local comp = Image(clone.width, clone.height, ColorMode.RGB)\n"
             "comp:clear(white)\n"
             "\n"
             "-- Ghosts: oldest first so closer frames draw on top\n"
             "for offset = " + std::to_string(before) + ", 1, -1 do\n"
             "  local fi = idx - offset\n"
             "  if fi >= 1 then\n"
             "    comp:drawImage(flatten_frame(clone, fi), Point(0, 0), " + ghostOpacityInt +
             ", BlendMode.NORMAL)\n"
             "  end\n"
             "end\n"
             "for offset = " + std::to_string(after) + ", 1, -1 do\n"
             "  local fi = idx + offset\n"
             "  if fi <= #clone.frames then\n"
             "    comp:drawImage(flatten_frame(clone, fi), Point(0, 0), " + ghostOpacityInt +
             ", BlendMode.NORMAL)\n"
             "  end\n"
             "end\n"
             "comp:drawImage(flatten_frame(clone, idx), Point(0, 0), 255, BlendMode.NORMAL)\n"
             "\n"
             "if " + std::to_string(scale) + " > 1 then\n"
             "  comp:resize{ width = comp.width * " + std::to_string(scale) +
             ", height = comp.height * " + std::to_string(scale) +
             ", method = \"nearest-neighbor\" }\n"
             "end\n"
             "comp:saveAs(\"" + luaString(absOutput) + "\")\n"
             "if not app.fs.isFile(\"" + luaString(absOutput) + "\") then\n"
             "  print(\"ERROR:Failed to render onion skin: no file was written\")\n"
             "  return\n"
             "end\n"
             "print(\"Onion-skin render of frame " + std::to_string(frameIndex) + " (-" +
             std::to_string(before) + "/+" + std::to_string(after) + " ghosts) saved to " +
             luaString(output) + " at " + std::to_string(scale) + "x\")";
    }));

  tools.push_back(LuaTool(
    "compare_frames",
    "Diff two frames of the flattened sprite pixel by pixel. Returns JSON with the changed "
    "pixel count, total pixels, percent changed, and (when something changed) the bounding box "
    "of the changed region. Fully transparent pixels are not counted as different.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to read" } } },
        { "frame_a", json11::Json::object {
            { "type", "integer" },
            { "description", "First frame index, starting at 1" } } },
        { "frame_b", json11::Json::object {
            { "type", "integer" },
            { "description", "Second frame index, starting at 1" } } },
      } },
      { "required", json11::Json::array { "filename", "frame_a", "frame_b" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int frameA = argInt(args, "frame_a");
      const int frameB = argInt(args, "frame_b");
      return std::string(kFlattenFrame) +
             "local spr = app.activeSprite\n"
             "if not spr then print(\"ERROR:No active sprite\") return end\n"
             "\n"
             "local a, b = " + std::to_string(frameA) + ", " + std::to_string(frameB) + "\n"
             "if a < 1 or a > #spr.frames or b < 1 or b > #spr.frames then\n"
             "  print(\"ERROR:Frame index out of range\")\n"
             "  return\n"
             "end\n"
             "\n"
             "local clone = Sprite(spr)\n"
             "clone:flatten()\n"
             "local img_a = flatten_frame(clone, a)\n"
             "local img_b = flatten_frame(clone, b)\n"
             "\n"
             "local changed = 0\n"
             "local min_x, min_y = math.huge, math.huge\n"
             "local max_x, max_y = -1, -1\n"
             "for py = 0, img_a.height - 1 do\n"
             "  for px = 0, img_a.width - 1 do\n"
             "    local va = img_a:getPixel(px, py)\n"
             "    local vb = img_b:getPixel(px, py)\n"
             "    local aa = app.pixelColor.rgbaA(va)\n"
             "    local ab = app.pixelColor.rgbaA(vb)\n"
             "    local diff = false\n"
             "    if aa > 0 or ab > 0 then diff = va ~= vb end\n"
             "    if diff then\n"
             "      changed = changed + 1\n"
             "      if px < min_x then min_x = px end\n"
             "      if py < min_y then min_y = py end\n"
             "      if px > max_x then max_x = px end\n"
             "      if py > max_y then max_y = py end\n"
             "    end\n"
             "  end\n"
             "end\n"
             "\n"
             "local total = img_a.width * img_a.height\n"
             "local percent = \"0.0\"\n"
             "if total > 0 then\n"
             "  percent = string.format(\"%.2f\", changed / total * 100)\n"
             "  percent = string.gsub(percent, \"0+$\", \"\")\n"
             "  if string.sub(percent, -1) == \".\" then percent = percent .. \"0\" end\n"
             "end\n"
             "local out = string.format(\n"
             "  \"{\\\"frame_a\\\":%d,\\\"frame_b\\\":%d,\\\"changed_pixels\\\":%d,\" ..\n"
             "  \"\\\"total_pixels\\\":%d,\\\"percent_changed\\\":%s\",\n"
             "  a, b, changed, total, percent)\n"
             "if changed > 0 then\n"
             "  out = out .. string.format(\n"
             "    \",\\\"changed_bounds\\\":{\\\"x\\\":%d,\\\"y\\\":%d,\\\"width\\\":%d,\\\"height\\\":%d}\",\n"
             "    min_x, min_y, max_x - min_x + 1, max_y - min_y + 1)\n"
             "end\n"
             "print(out .. \"}\")";
    }));

  tools.push_back(LuaTool(
    "get_color_stats",
    "Histogram the colors of one frame of the flattened sprite. Returns JSON with the number "
    "of unique colors, the opaque pixel count, and the most used colors (\"#RRGGBB\" plus "
    "count), handy to check palette discipline.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to read" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame to analyze, starting at 1 (default: 1)" } } },
        { "top", json11::Json::object {
            { "type", "integer" },
            { "description", "How many of the most-used colors to list (default: 16)" } } },
      } },
      { "required", json11::Json::array { "filename" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int top = argInt(args, "top", 16);
      if (top < 1)
        return luaError("top must be >= 1");

      return std::string(kFlattenFrame) +
             "local spr = app.activeSprite\n"
             "if not spr then print(\"ERROR:No active sprite\") return end\n"
             "\n"
             "local idx = " + std::to_string(argInt(args, "frame_index", 1)) + "\n"
             "if idx < 1 or idx > #spr.frames then print(\"ERROR:Frame index out of range\") return end\n"
             "\n"
             "local clone = Sprite(spr)\n"
             "clone:flatten()\n"
             "local img = flatten_frame(clone, idx)\n"
             "\n"
             "local counts = {}\n"
             "local opaque = 0\n"
             "for py = 0, img.height - 1 do\n"
             "  for px = 0, img.width - 1 do\n"
             "    local v = img:getPixel(px, py)\n"
             "    if app.pixelColor.rgbaA(v) > 0 then\n"
             "      opaque = opaque + 1\n"
             "      local hex = string.format(\"#%02X%02X%02X\",\n"
             "        app.pixelColor.rgbaR(v), app.pixelColor.rgbaG(v), app.pixelColor.rgbaB(v))\n"
             "      counts[hex] = (counts[hex] or 0) + 1\n"
             "    end\n"
             "  end\n"
             "end\n"
             "\n"
             "local colors = {}\n"
             "for hex, count in pairs(counts) do\n"
             "  table.insert(colors, { color = hex, count = count })\n"
             "end\n"
             "table.sort(colors, function(x, y)\n"
             "  if x.count ~= y.count then return x.count > y.count end\n"
             "  return x.color < y.color\n"
             "end)\n"
             "\n"
             "local top_colors = {}\n"
             "for i = 1, math.min(" + std::to_string(top) + ", #colors) do\n"
             "  top_colors[i] = colors[i]\n"
             "end\n"
             "print(json.encode({\n"
             "  frame = idx,\n"
             "  unique_colors = #colors,\n"
             "  opaque_pixels = opaque,\n"
             "  top_colors = top_colors,\n"
             "}))";
    }));
}

} // namespace mcp
} // namespace app
