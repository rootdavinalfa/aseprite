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

#include "app/mcp/mcp_fonts.h"
#include "app/mcp/mcp_tools_lua.h"
#include "app/mcp/mcp_tools_registry.h"

#include "app/app.h"
#include "app/commands/commands.h"
#include "app/commands/params.h"
#include "app/context.h"
#include "app/doc.h"
#include "app/doc_api.h"
#include "app/mcp/mcp_tool_context.h"
#include "app/site.h"
#include "app/tx.h"
#include "base/fs.h"
#include "doc/cel.h"
#include "doc/image.h"
#include "doc/image_ref.h"
#include "doc/layer.h"
#include "doc/primitives.h"
#include "doc/sprite.h"

#include <algorithm>
#include <cstdlib>
#include <string>
#include <vector>

namespace app {
namespace mcp {

namespace {

// Reads the "pixels" argument: an array of {x, y, color} objects.
// Returns false (and fills "error") when a color is malformed.
bool pixelLua(
  const json11::Json& pixels,
  const bool useCelOffset,
  const bool usePset,
  std::string& script,
  std::string& error)
{
  if (!pixels.is_array())
    return true;

  for (const auto& pixel : pixels.array_items()) {
    const int x = argInt(pixel, "x");
    const int y = argInt(pixel, "y");
    int r = 0, g = 0, b = 0, a = 255;
    if (!parseHexColor(argString(pixel, "color", "#000000"), r, g, b, a)) {
      error = argString(pixel, "color");
      return false;
    }

    const std::string px = std::to_string(x) + (useCelOffset ? " - cox" : "");
    const std::string py = std::to_string(y) + (useCelOffset ? " - coy" : "");
    script += (usePset ? "  pset(img, " : "  img:putPixel(") +
              px + ", " + py + ", Color(" + std::to_string(r) + ", " + std::to_string(g) + ", " +
              std::to_string(b) + ", " + std::to_string(a) + "))\n";
  }
  return true;
}

// Reads the "points" argument: an array of {x, y} objects.
std::string pointsLua(const json11::Json& points)
{
  std::string list;
  if (points.is_array()) {
    for (const auto& point : points.array_items()) {
      if (!list.empty())
        list += ", ";
      list += "{ x = " + std::to_string(argInt(point, "x")) +
              ", y = " + std::to_string(argInt(point, "y")) + " }";
    }
  }
  return list;
}

std::string frameIndexCheck(const int frameIndex)
{
  const std::string idx = std::to_string(frameIndex);
  return "local idx = " + idx + "\n"
         "if idx < 1 or idx > #spr.frames then\n"
         "  print(\"ERROR:Frame index out of range\") return\n"
         "end\n";
}

// Selects a layer/frame and normalizes (or creates) its cel, leaving
// "img" canvas-sized so the given body can use global coordinates.
std::string layerFrameCel(const std::string& layerName,
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
         "  app.activeLayer = target\n"
         "  app.activeFrame = spr.frames[idx]\n"
         "  local cel = normalize_cel(spr, target, spr.frames[idx], " +
         (createIfMissing ? "true" : "false") + ")\n"
         "  if not cel then return end\n"
         "  local img = cel.image\n";
}

// Closes the transaction opened by layerFrameCel(), saves, and reports OK.
std::string layerFrameCelEnd()
{
  return "end)\n"
         "\n"
         "spr:saveAs(spr.filename)\n"
         "print(\"OK\")";
}

// Bresenham line that stamps a size x size square around each point.
const char kDrawLineLua[] =
  "local function put_thick(img, x, y, color, size)\n"
  "  local rad = math.max(0, math.floor(size / 2))\n"
  "  for oy = -rad, rad do\n"
  "    for ox = -rad, rad do\n"
  "      pset(img, x + ox, y + oy, color)\n"
  "    end\n"
  "  end\n"
  "end\n"
  "\n"
  "local function draw_line(img, x0, y0, x1, y1, color, size)\n"
  "  local dx = math.abs(x1 - x0)\n"
  "  local sx = x0 < x1 and 1 or -1\n"
  "  local dy = -math.abs(y1 - y0)\n"
  "  local sy = y0 < y1 and 1 or -1\n"
  "  local err = dx + dy\n"
  "  while true do\n"
  "    if size > 1 then\n"
  "      put_thick(img, x0, y0, color, size)\n"
  "    else\n"
  "      pset(img, x0, y0, color)\n"
  "    end\n"
  "    if x0 == x1 and y0 == y1 then break end\n"
  "    local e2 = 2 * err\n"
  "    if e2 >= dy then err = err + dy; x0 = x0 + sx end\n"
  "    if e2 <= dx then err = err + dx; y0 = y0 + sy end\n"
  "  end\n"
  "end\n";

std::string shapeToolLua(const bool fill, const std::string& outline, const std::string& filled)
{
  return fill ? filled : outline;
}

// Active-cel script bodies shared by draw_pixels/draw_line/draw_rectangle/
// fill_area/draw_circle: they draw on app.activeCel, covering the first
// layer/frame when no cel is active.
const char kActiveCelStart[] =
  "app.transaction(function()\n"
  "  local cel = app.activeCel\n"
  "  if not cel then\n"
  "    app.activeLayer = spr.layers[1]\n"
  "    app.activeFrame = spr.frames[1]\n"
  "    cel = app.activeCel\n"
  "    if not cel then\n"
  "      print(\"ERROR:No active cel and couldn't create one\")\n"
  "      return\n"
  "    end\n"
  "  end\n"
  "  local img = cel.image\n"
  "  local cox = cel.position.x\n"
  "  local coy = cel.position.y\n";

// Same as layerFrameCel(), but for scripts drawing inside the
// transaction opened by kActiveCelStart.
std::string activeCelEnd()
{
  return "end)\n"
         "\n"
         "spr:saveAs(spr.filename)\n"
         "print(\"OK\")";
}

// ---------------------------------------------------------------------
// Text tools: glyphs are rasterized natively (mcp_fonts) and
// composited into the cel image, because the Lua API has no text.

const char* const kTextAnchors[] = {
  "topleft",      "top",       "topright",
  "left",         "center",    "right",
  "bottomleft",   "bottom",    "bottomright",
  "baselineleft", "baseline",  "baselineright",
};

ToolResult toolError(const std::string& text)
{
  ToolResult result;
  result.isError = true;
  result.text = text;
  return result;
}

bool isValidAnchor(const std::string& anchor)
{
  for (const char* candidate : kTextAnchors) {
    if (anchor == candidate)
      return true;
  }
  return false;
}

std::string joinSegments(const std::vector<std::string>& segments,
                         const size_t from,
                         const size_t to)
{
  std::string out;
  for (size_t i = from; i <= to; ++i) {
    if (i > from)
      out += '/';
    out += segments[i];
  }
  return out;
}

// The trailing "group/child" half of findLayerByName(): segments are
// matched longest-first with backtracking, like find_layer() in
// mcp_tools_lua.h.
doc::Layer* resolveLayerPath(const doc::LayerList& layers,
                             const std::vector<std::string>& segments,
                             const size_t from)
{
  for (size_t j = segments.size(); j > from; --j) {
    const std::string candidate = joinSegments(segments, from, j - 1);
    for (doc::Layer* layer : layers) {
      if (layer->name() != candidate)
        continue;
      if (j == segments.size())
        return layer;
      if (layer->isGroup()) {
        if (doc::Layer* found = resolveLayerPath(layer->layers(), segments, j))
          return found;
      }
    }
  }
  return nullptr;
}

// Finds a layer by name, searching inside groups: an exact full-name
// match is tried first, breadth-first, like the Lua tools.
doc::Layer* findLayerByName(doc::Sprite* sprite, const std::string& name)
{
  doc::Layer* root = sprite->root();
  std::vector<doc::Layer*> queue(root->layers().begin(), root->layers().end());
  for (size_t head = 0; head < queue.size(); ++head) {
    doc::Layer* layer = queue[head];
    if (layer->name() == name)
      return layer;
    if (layer->isGroup()) {
      for (doc::Layer* child : layer->layers())
        queue.push_back(child);
    }
  }

  if (name.find('/') == std::string::npos)
    return nullptr;

  std::vector<std::string> segments;
  size_t start = 0;
  while (start < name.size()) {
    size_t end = name.find('/', start);
    if (end == std::string::npos)
      end = name.size();
    if (end > start)
      segments.push_back(name.substr(start, end - start));
    start = end + 1;
  }
  return resolveLayerPath(root->layers(), segments, 0);
}

// Maps an anchor point to the pen origin the glyph run starts from.
// Ink runs from the pen origin on x and from the baseline on y, so the
// baseline anchors need no vertical correction while the box anchors
// are offset by the ink extents.
gfx::Point textPenOrigin(const std::string& anchor,
                         const int x,
                         const int y,
                         const TextMetrics& metrics)
{
  gfx::Point pen(x, y);
  if (anchor == "topright" || anchor == "right" || anchor == "bottomright" ||
      anchor == "baselineright")
    pen.x = x - metrics.width;
  else if (anchor == "top" || anchor == "center" || anchor == "bottom" ||
           anchor == "baseline")
    pen.x = x - metrics.width / 2;

  if (anchor.compare(0, 8, "baseline") == 0)
    ; // the baseline anchors use y as-is
  else if (anchor.compare(0, 6, "bottom") == 0)
    pen.y = y - metrics.belowBaseline;
  else if (anchor == "left" || anchor == "center" || anchor == "right")
    pen.y = y - metrics.height / 2 + metrics.aboveBaseline;
  else
    pen.y = y + metrics.aboveBaseline;
  return pen;
}

void register_text_tools(std::vector<McpTool>& tools)
{
  tools.push_back(NativeTool(
    "list_text_fonts",
    "List the fonts measure_text and draw_text can use, as JSON objects with name, kind "
    "(\"bitmap\" or \"truetype\") and path. Fonts are discovered under the "
    "ASEPRITE_MCP_FONTS directory (or ~/.aseprite-mcp/fonts by default): a directory "
    "holding font.json is a bitmap font, and .ttf/.otf files are TrueType; on macOS the "
    "system font directories are also listed. Bitmap fonts are the right choice for pixel "
    "art: their glyphs are already pixels and size scales them by whole numbers.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {} },
      { "required", json11::Json::array {} },
    },
    [](const json11::Json&) -> ToolResult {
      json11::Json::array out;
      for (const FontInfo& font : listFonts()) {
        out.push_back(json11::Json::object {
          { "name", font.name },
          { "kind", font.kind == FontInfo::Kind::Bitmap ? "bitmap" : "truetype" },
          { "path", font.path },
        });
      }

      ToolResult result;
      result.text = json11::Json(out).dump();
      return result;
    }));

  tools.push_back(NativeTool(
    "measure_text",
    "Measure text without drawing it, reporting the ink box (width and height), the pen "
    "advance, and the ink extents above and below the baseline. Both bitmap fonts "
    "(sprite-sheet) and TrueType fonts are supported; bitmap fonts scale by whole numbers. "
    "Use this to size a panel or center a label in one shot instead of guessing and "
    "re-rendering.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "text", json11::Json::object {
            { "type", "string" },
            { "description", "The string to measure" } } },
        { "font", json11::Json::object {
            { "type", "string" },
            { "description", "Font name from list_text_fonts, or a path to a .ttf/.otf "
                             "file or a bitmap font directory" } } },
        { "size", json11::Json::object {
            { "type", "integer" },
            { "description", "Pixel size for TrueType fonts; integer scale factor for "
                             "bitmap fonts (default: 1)" } } },
        { "letter_spacing", json11::Json::object {
            { "type", "integer" },
            { "description", "Extra pixels between glyphs (default: 0)" } } },
        { "bold", json11::Json::object {
            { "type", "integer" },
            { "description", "Faux-bold passes; each grows strokes by 1px (default: 0)" } } },
        { "antialias", json11::Json::object {
            { "type", "boolean" },
            { "description", "Keep greyscale edges instead of hard pixels (default: false)" } } },
      } },
      { "required", json11::Json::array { "text", "font" } },
    },
    [](const json11::Json& args) -> ToolResult {
      FontInfo font;
      std::string error;
      if (!resolveFont(argString(args, "font"), font, error))
        return toolError(error);

      TextRenderOptions options;
      options.size = argInt(args, "size", 1);
      options.letterSpacing = argInt(args, "letter_spacing", 0);
      options.bold = argInt(args, "bold", 0);
      options.antialias = argBool(args, "antialias", false);

      TextMetrics metrics;
      if (!measureText(font, argString(args, "text"), options, metrics, error))
        return toolError(error);

      ToolResult result;
      result.text = "width=" + std::to_string(metrics.width) +
                    " height=" + std::to_string(metrics.height) +
                    " advance_width=" + std::to_string(metrics.advanceWidth) +
                    " above_baseline=" + std::to_string(metrics.aboveBaseline) +
                    " below_baseline=" + std::to_string(metrics.belowBaseline);
      return result;
    }));

  tools.push_back(NativeTool(
    "draw_text",
    "Draw text onto a sprite. Glyphs are rasterized natively and composited into the "
    "layer's cel image, because the Lua API has no text drawing of its own. The anchor "
    "selects which point of the text (x, y) refers to; outline and shadow are drawn "
    "behind the letters in the given colors. Optional layer_name defaults to the active "
    "layer. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "text", json11::Json::object {
            { "type", "string" },
            { "description", "String to draw" } } },
        { "x", json11::Json::object {
            { "type", "integer" },
            { "description", "X coordinate of the anchor point" } } },
        { "y", json11::Json::object {
            { "type", "integer" },
            { "description", "Y coordinate of the anchor point" } } },
        { "font", json11::Json::object {
            { "type", "string" },
            { "description", "Font name from list_text_fonts, or a path to a .ttf/.otf "
                             "file or a bitmap font directory" } } },
        { "size", json11::Json::object {
            { "type", "integer" },
            { "description", "Pixel size for TrueType fonts; integer scale factor for "
                             "bitmap fonts (default: 1)" } } },
        { "color", json11::Json::object {
            { "type", "string" },
            { "description", "Text color #RRGGBB or #RRGGBBAA (default: #FFFFFF)" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to draw on (default: active layer)" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1 (default: 1)" } } },
        { "anchor", json11::Json::object {
            { "type", "string" },
            { "description", "Which point of the text (x, y) refers to: topleft, top, "
                             "topright, left, center, right, bottomleft, bottom, "
                             "bottomright, baselineleft, baseline or baselineright "
                             "(default: topleft)" } } },
        { "letter_spacing", json11::Json::object {
            { "type", "integer" },
            { "description", "Extra pixels between glyphs (default: 0)" } } },
        { "bold", json11::Json::object {
            { "type", "integer" },
            { "description", "Faux-bold passes; each grows strokes by 1px (default: 0)" } } },
        { "outline_color", json11::Json::object {
            { "type", "string" },
            { "description", "If set, draw an outline in this color (default: no outline)" } } },
        { "outline_width", json11::Json::object {
            { "type", "integer" },
            { "description", "Outline thickness in pixels (default: 1)" } } },
        { "outline_diagonal", json11::Json::object {
            { "type", "boolean" },
            { "description", "Include diagonal neighbors in the outline, giving rounded "
                             "corners (default: true)" } } },
        { "shadow_color", json11::Json::object {
            { "type", "string" },
            { "description", "If set, draw a drop shadow in this color "
                             "(default: no shadow)" } } },
        { "shadow_dx", json11::Json::object {
            { "type", "integer" },
            { "description", "Shadow X offset (default: 1)" } } },
        { "shadow_dy", json11::Json::object {
            { "type", "integer" },
            { "description", "Shadow Y offset (default: 1)" } } },
        { "antialias", json11::Json::object {
            { "type", "boolean" },
            { "description", "Keep greyscale edges instead of hard pixels (default: false)" } } },
        { "create_if_missing", json11::Json::object {
            { "type", "boolean" },
            { "description", "Create the layer when it does not exist (default: true)" } } },
      } },
      { "required", json11::Json::array { "filename", "text", "x", "y", "font" } },
    },
    [](const json11::Json& args) -> ToolResult {
      const std::string anchor = argString(args, "anchor", "topleft");
      if (!isValidAnchor(anchor)) {
        return toolError("Invalid anchor '" + anchor + "'. Expected one of: baseline, "
                         "baselineleft, baselineright, bottom, bottomleft, bottomright, "
                         "center, left, right, top, topleft, topright");
      }

      int r = 255, g = 255, b = 255, a = 255;
      if (!parseHexColor(argString(args, "color", "#FFFFFF"), r, g, b, a))
        return toolError("Invalid color value: " + argString(args, "color"));

      const std::string outlineColor = argString(args, "outline_color");
      int or_ = 0, og = 0, ob = 0, oa = 255;
      const bool hasOutline = !outlineColor.empty();
      if (hasOutline && !parseHexColor(outlineColor, or_, og, ob, oa))
        return toolError("Invalid outline_color value: " + outlineColor);

      const std::string shadowColor = argString(args, "shadow_color");
      int sr = 0, sg = 0, sb = 0, sa = 255;
      const bool hasShadow = !shadowColor.empty();
      if (hasShadow && !parseHexColor(shadowColor, sr, sg, sb, sa))
        return toolError("Invalid shadow_color value: " + shadowColor);

      FontInfo font;
      std::string error;
      if (!resolveFont(argString(args, "font"), font, error))
        return toolError(error);

      TextRenderOptions options;
      options.size = argInt(args, "size", 1);
      options.letterSpacing = argInt(args, "letter_spacing", 0);
      options.bold = argInt(args, "bold", 0);
      options.antialias = argBool(args, "antialias", false);

      const std::string text = argString(args, "text");
      TextMetrics metrics;
      if (!measureText(font, text, options, metrics, error))
        return toolError(error);
      if (metrics.width == 0 && metrics.height == 0) {
        ToolResult result;
        result.text = "OK: nothing to draw (text has no visible glyphs)";
        return result;
      }

      if (!ToolContext::instance().openDocument(argString(args, "filename"), error))
        return toolError(error);

      app::Context* context = App::instance()->context();
      Doc* doc = context ? context->activeDocument() : nullptr;
      if (!doc)
        return toolError("No active sprite");
      doc::Sprite* sprite = doc->sprite();

      const int frameIndex = argInt(args, "frame_index", 1);
      if (frameIndex < 1 || frameIndex > sprite->totalFrames())
        return toolError("Frame index out of range");
      const doc::frame_t frame = doc::frame_t(frameIndex - 1);

      const std::string layerName = argString(args, "layer_name");
      const bool createIfMissing = argBool(args, "create_if_missing", true);
      const gfx::Point pen = textPenOrigin(anchor, argInt(args, "x"), argInt(args, "y"),
                                           metrics);

      try {
        Tx tx(doc);
        DocApi api = doc->getApi(tx);

        doc::Layer* layer = nullptr;
        if (!layerName.empty()) {
          layer = findLayerByName(sprite, layerName);
        }
        else {
          // The active layer, like app.activeLayer in Lua; only when
          // there is no active layer fall back to the bottom one.
          layer = context->activeSite().layer();
          if (layer && layer->sprite() != sprite)
            layer = nullptr;
          if (!layer)
            layer = sprite->firstLayer();
        }

        if (!layer) {
          if (layerName.empty())
            return toolError("No layer to draw on");
          if (!createIfMissing)
            return toolError("Layer not found");
          layer = api.newLayer(sprite->root(), layerName);
        }
        if (!layer->isImage())
          return toolError("Layer cannot hold image pixels");

        auto layerImage = static_cast<doc::LayerImage*>(layer);
        doc::Cel* cel = layerImage->cel(frame);
        if (!cel) {
          doc::ImageRef image(doc::Image::create(sprite->spec()));
          if (!image)
            return toolError("Could not allocate the cel image");
          doc::clear_image(image.get(), 0);
          cel = api.addCel(layerImage, frame, image);
        }
        else if (cel->position() != gfx::Point(0, 0) ||
                 cel->image()->size() != sprite->size()) {
          // Make the cel canvas-sized so sprite-global coordinates
          // can be used directly, like normalize_cel() in Lua.
          doc::ImageRef image(doc::Image::create(sprite->spec()));
          if (!image)
            return toolError("Could not allocate the cel image");
          doc::clear_image(image.get(), 0);
          image->copy(cel->image(),
                      gfx::Clip(gfx::Point(cel->x(), cel->y()), cel->image()->bounds()));
          api.replaceImage(sprite, cel->data()->imageRef(), image);
          api.setCelPosition(sprite, cel, 0, 0);
        }

        doc::Image* target = cel->image();
        if (hasShadow) {
          if (!drawTextInImage(font, text, target,
                               pen.x + argInt(args, "shadow_dx", 1),
                               pen.y + argInt(args, "shadow_dy", 1),
                               doc::rgba(sr, sg, sb, sa), options, nullptr, error))
            return toolError(error);
        }
        if (hasOutline) {
          const int width = argInt(args, "outline_width", 1);
          if (width < 1 || width > 16)
            return toolError("outline_width must be between 1 and 16");
          const bool diagonal = argBool(args, "outline_diagonal", true);
          for (int dy = -width; dy <= width; ++dy) {
            for (int dx = -width; dx <= width; ++dx) {
              if (dx == 0 && dy == 0)
                continue;
              // The 4-way outline grows as a diamond (Manhattan
              // distance), the diagonal one as a square.
              if (!diagonal && std::abs(dx) + std::abs(dy) > width)
                continue;
              if (!drawTextInImage(font, text, target, pen.x + dx, pen.y + dy,
                                   doc::rgba(or_, og, ob, oa), options, nullptr, error))
                return toolError(error);
            }
          }
        }
        if (!drawTextInImage(font, text, target, pen.x, pen.y, doc::rgba(r, g, b, a),
                             options, nullptr, error))
          return toolError(error);

        tx.commit();
      }
      catch (const std::exception& ex) {
        return toolError(ex.what());
      }
      catch (...) {
        return toolError("Failed to draw text");
      }

      const std::string absFn = base::get_absolute_path(doc->filename());
      context->setActiveDocument(doc);
      Command* saveCommand = Commands::instance()->byId(CommandId::SaveFileCopyAs());
      Params params;
      params.set("filename", absFn.c_str());
      params.set("useUI", "false");
      context->executeCommand(saveCommand, params);
      doc->setFilename(absFn);
      doc->markAsSaved();

      // The ink box, not the baseline pen, is what the caller sees.
      const gfx::Point inkTopLeft(pen.x, pen.y - metrics.aboveBaseline);
      ToolResult result;
      result.text = "Drew '" + text + "' at (" + std::to_string(inkTopLeft.x) + ", " +
                    std::to_string(inkTopLeft.y) + "), text box " +
                    std::to_string(metrics.width) + "x" + std::to_string(metrics.height) +
                    " (anchor " + anchor + ")";
      return result;
    }));
}

} // anonymous namespace

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
    [](const json11::Json& args) -> std::string {
      const int width = argInt(args, "width");
      const int height = argInt(args, "height");
      if (width <= 0 || height <= 0)
        return std::string("print(\"ERROR:Width and height must be > 0\")");

      const std::string filename = argString(args, "filename", "canvas.aseprite");
      return "local spr = Sprite(" + std::to_string(width) + ", " + std::to_string(height) + ")\n"
             "spr:saveAs(\"" + luaString(filename) + "\")\n"
             "print(\"" + luaString(filename) + "\")\n"
             // A freshly created file doesn't need to stay open in a
             // tab (repeated calls would pile up one document per
             // call); reopen it through the normal document path when
             // a tool needs it.
             "pcall(function() spr:close() end)";
    }));

  tools.push_back(LuaTool(
    "add_layer",
    "Add a new layer to the Aseprite file, optionally inside an existing group given by "
    "name or group/subgroup path. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Name of the new layer" } } },
        { "group", json11::Json::object {
            { "type", "string" },
            { "description", "Optional group to place the layer in, by name or path "
                             "(default: top level)" } } },
      } },
      { "required", json11::Json::array { "filename", "layer_name" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const std::string layerName = argString(args, "layer_name");
      const std::string group = argString(args, "group");
      if (layerName.empty())
        return std::string("print(\"ERROR:Layer name cannot be empty\")");

      std::string script = kSpritePreludeLua;
      script += kFindLayerLua;
      script += "local parent = nil\n"
                "if \"" + luaString(group) + "\" ~= \"\" then\n"
                "  parent = find_layer(spr, \"" + luaString(group) + "\")\n"
                "  if not parent then print(\"ERROR:Group not found\") return end\n"
                "  if not parent.isGroup then print(\"ERROR:Target is not a group\") return end\n"
                "end\n"
                "\n"
                "app.transaction(function()\n"
                "  local lyr = spr:newLayer()\n"
                "  lyr.name = \"" + luaString(layerName) + "\"\n"
                "  if parent then lyr.parent = parent end\n"
                "end)\n"
                "\n"
                "spr:saveAs(spr.filename)\n"
                "print(\"OK\")";
      return script;
    }));

  tools.push_back(LuaTool(
    "add_group",
    "Add a new empty group layer, optionally nested inside an existing group given by name "
    "or group/subgroup path. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "group_name", json11::Json::object {
            { "type", "string" },
            { "description", "Name of the new group" } } },
        { "parent_group", json11::Json::object {
            { "type", "string" },
            { "description", "Optional existing group to nest the new group in, by name "
                             "or path (default: top level)" } } },
      } },
      { "required", json11::Json::array { "filename", "group_name" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const std::string groupName = argString(args, "group_name");
      const std::string parentGroup = argString(args, "parent_group");
      if (groupName.empty())
        return std::string("print(\"ERROR:Group name cannot be empty\")");

      std::string script = kSpritePreludeLua;
      script += kFindLayerLua;
      script += "local parent = nil\n"
                "if \"" + luaString(parentGroup) + "\" ~= \"\" then\n"
                "  parent = find_layer(spr, \"" + luaString(parentGroup) + "\")\n"
                "  if not parent then print(\"ERROR:Parent group not found\") return end\n"
                "  if not parent.isGroup then print(\"ERROR:Target is not a group\") return end\n"
                "end\n"
                "\n"
                "app.transaction(function()\n"
                "  local grp = spr:newGroup()\n"
                "  grp.name = \"" + luaString(groupName) + "\"\n"
                "  if parent then grp.parent = parent end\n"
                "end)\n"
                "\n"
                "spr:saveAs(spr.filename)\n"
                "print(\"OK\")";
      return script;
    }));

  tools.push_back(LuaTool(
    "add_frame",
    "Add a new frame at the end of the animation, duplicating the last frame's content. "
    "The file is saved.",
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
             "  spr:newFrame()\n"
             "end)\n"
             "\n"
             "spr:saveAs(spr.filename)\n"
             "print(\"OK\")";
    }));

  tools.push_back(LuaTool(
    "set_frame",
    "Set the active frame by 1-based index. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
      } },
      { "required", json11::Json::array { "filename", "frame_index" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      return std::string(kSpritePreludeLua) +
             frameIndexCheck(argInt(args, "frame_index")) +
             "\n"
             "app.activeFrame = spr.frames[idx]\n"
             "\n"
             "spr:saveAs(spr.filename)\n"
             "print(\"OK\")";
    }));

  tools.push_back(LuaTool(
    "set_frame_duration",
    "Set the duration of a frame in milliseconds by 1-based index. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "duration_ms", json11::Json::object {
            { "type", "integer" },
            { "description", "Duration in milliseconds (must be > 0)" } } },
      } },
      { "required", json11::Json::array { "filename", "frame_index", "duration_ms" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int durationMs = argInt(args, "duration_ms");
      if (durationMs <= 0)
        return std::string("print(\"ERROR:Duration must be > 0\")");

      return std::string(kSpritePreludeLua) +
             frameIndexCheck(argInt(args, "frame_index")) +
             "\n"
             "app.transaction(function()\n"
             "  spr.frames[idx].duration = " + std::to_string(durationMs) + " / 1000.0\n"
             "end)\n"
             "\n"
             "spr:saveAs(spr.filename)\n"
             "print(\"OK\")";
    }));

  tools.push_back(LuaTool(
    "set_layer",
    "Set the active layer by name, searching inside groups too. With create_if_missing the "
    "layer is created when absent. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer name to activate" } } },
        { "create_if_missing", json11::Json::object {
            { "type", "boolean" },
            { "description", "Create the layer when it does not exist (default: false)" } } },
      } },
      { "required", json11::Json::array { "filename", "layer_name" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const std::string layerName = argString(args, "layer_name");
      if (layerName.empty())
        return std::string("print(\"ERROR:Layer name cannot be empty\")");

      const bool create = argBool(args, "create_if_missing", false);
      std::string script = kSpritePreludeLua;
      script += kFindLayerLua;
      script += "local target = find_layer(spr, \"" + luaString(layerName) + "\")\n"
                "if not target and " + (create ? "true" : "false") + " then\n"
                "  target = spr:newLayer()\n"
                "  target.name = \"" + luaString(layerName) + "\"\n"
                "end\n"
                "if not target then print(\"ERROR:Layer not found\") return end\n"
                "\n"
                "app.activeLayer = target\n"
                "\n"
                "spr:saveAs(spr.filename)\n"
                "print(\"OK\")";
      return script;
    }));

  tools.push_back(LuaTool(
    "draw_pixels",
    "Draw pixels with individual colors on the active layer/frame. Coordinates are "
    "sprite-global; each pixel needs x, y, and a #RRGGBB or #RRGGBBAA color. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "pixels", json11::Json::object {
            { "type", "array" },
            { "description", "Pixels to draw, each { x, y, color }" },
            { "items", json11::Json::object {
              { "type", "object" },
              { "properties", json11::Json::object {
                { "x", json11::Json::object { { "type", "integer" },
                                              { "description", "X coordinate" } } },
                { "y", json11::Json::object { { "type", "integer" },
                                              { "description", "Y coordinate" } } },
                { "color", json11::Json::object {
                    { "type", "string" },
                    { "description", "Hex color #RRGGBB or #RRGGBBAA (default: #000000)" } } },
              } } } } } },
      } },
      { "required", json11::Json::array { "filename", "pixels" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      std::string body, color;
      if (!pixelLua(args["pixels"], true, false, body, color))
        return std::string("print(\"ERROR:Invalid color value: " + luaString(color) + "\")");
      return std::string(kSpritePreludeLua) + kActiveCelStart + body + activeCelEnd();
    }));

  tools.push_back(LuaTool(
    "draw_line",
    "Draw a line on the active layer/frame with Bresenham, in sprite-global coordinates. "
    "The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "x1", json11::Json::object { { "type", "integer" },
                                       { "description", "Starting x coordinate" } } },
        { "y1", json11::Json::object { { "type", "integer" },
                                       { "description", "Starting y coordinate" } } },
        { "x2", json11::Json::object { { "type", "integer" },
                                       { "description", "Ending x coordinate" } } },
        { "y2", json11::Json::object { { "type", "integer" },
                                       { "description", "Ending y coordinate" } } },
        { "color", json11::Json::object {
            { "type", "string" },
            { "description", "Hex color #RRGGBB or #RRGGBBAA (default: #000000)" } } },
        { "thickness", json11::Json::object {
            { "type", "integer" },
            { "description", "Line thickness in pixels (default: 1)" } } },
      } },
      { "required", json11::Json::array { "filename", "x1", "y1", "x2", "y2" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      int r = 0, g = 0, b = 0, a = 255;
      if (!parseHexColor(argString(args, "color", "#000000"), r, g, b, a))
        return std::string("print(\"ERROR:Invalid color value: " +
                           luaString(argString(args, "color")) + "\")");

      return std::string(kSpritePreludeLua) +
             kPSetLua +
             kDrawLineLua +
             kActiveCelStart +
             "  draw_line(img, " + std::to_string(argInt(args, "x1")) + " - cox, " +
             std::to_string(argInt(args, "y1")) + " - coy, " +
             std::to_string(argInt(args, "x2")) + " - cox, " +
             std::to_string(argInt(args, "y2")) + " - coy, "
             "Color(" + std::to_string(r) + ", " + std::to_string(g) + ", " + std::to_string(b) +
             ", " + std::to_string(a) + "), " + std::to_string(argInt(args, "thickness", 1)) +
             ")\n" + activeCelEnd();
    }));

  tools.push_back(LuaTool(
    "draw_rectangle",
    "Draw a rectangle on the active layer/frame; x,y is the top-left corner and both sides "
    "must be > 0. With fill the rectangle is filled. Coordinates are sprite-global. The file "
    "is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "x", json11::Json::object { { "type", "integer" },
                                      { "description", "Top-left x coordinate" } } },
        { "y", json11::Json::object { { "type", "integer" },
                                      { "description", "Top-left y coordinate" } } },
        { "width", json11::Json::object {
            { "type", "integer" },
            { "description", "Width in pixels (must be > 0)" } } },
        { "height", json11::Json::object {
            { "type", "integer" },
            { "description", "Height in pixels (must be > 0)" } } },
        { "color", json11::Json::object {
            { "type", "string" },
            { "description", "Hex color #RRGGBB or #RRGGBBAA (default: #000000)" } } },
        { "fill", json11::Json::object {
            { "type", "boolean" },
            { "description", "Fill the rectangle instead of outlining it (default: false)" } } },
      } },
      { "required", json11::Json::array { "filename", "x", "y", "width", "height" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int x = argInt(args, "x");
      const int y = argInt(args, "y");
      const int width = argInt(args, "width");
      const int height = argInt(args, "height");
      if (width <= 0 || height <= 0)
        return std::string("print(\"ERROR:Width and height must be > 0\")");

      int r = 0, g = 0, b = 0, a = 255;
      if (!parseHexColor(argString(args, "color", "#000000"), r, g, b, a))
        return std::string("print(\"ERROR:Invalid color value: " +
                           luaString(argString(args, "color")) + "\")");

      // app.useTool treats both points as inclusive corners.
      const std::string tool = shapeToolLua(argBool(args, "fill"),
                                            "\"rectangle\"", "\"filled_rectangle\"");
      return std::string(kSpritePreludeLua) +
             kActiveCelStart +
             "  app.useTool({\n"
             "    tool = " + tool + ",\n"
             "    color = Color(" + std::to_string(r) + ", " + std::to_string(g) + ", " +
             std::to_string(b) + ", " + std::to_string(a) + "),\n"
             "    points = { Point(" + std::to_string(x) + ", " + std::to_string(y) +
             "), Point(" + std::to_string(x + width - 1) + ", " + std::to_string(y + height - 1) +
             ") }\n"
             "  })\n" + activeCelEnd();
    }));

  tools.push_back(LuaTool(
    "fill_area",
    "Flood-fill the region around a sprite-global point on the active layer/frame, using the "
    "paint bucket tool. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "x", json11::Json::object { { "type", "integer" },
                                      { "description", "X coordinate to fill from" } } },
        { "y", json11::Json::object { { "type", "integer" },
                                      { "description", "Y coordinate to fill from" } } },
        { "color", json11::Json::object {
            { "type", "string" },
            { "description", "Hex color #RRGGBB or #RRGGBBAA (default: #000000)" } } },
      } },
      { "required", json11::Json::array { "filename", "x", "y" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      int r = 0, g = 0, b = 0, a = 255;
      if (!parseHexColor(argString(args, "color", "#000000"), r, g, b, a))
        return std::string("print(\"ERROR:Invalid color value: " +
                           luaString(argString(args, "color")) + "\")");

      return std::string(kSpritePreludeLua) +
             kActiveCelStart +
             "  app.useTool({\n"
             "    tool = \"paint_bucket\",\n"
             "    color = Color(" + std::to_string(r) + ", " + std::to_string(g) + ", " +
             std::to_string(b) + ", " + std::to_string(a) + "),\n"
             "    points = { Point(" + std::to_string(argInt(args, "x")) + ", " +
             std::to_string(argInt(args, "y")) + ") }\n"
             "  })\n" + activeCelEnd();
    }));

  tools.push_back(LuaTool(
    "draw_circle",
    "Draw a circle (ellipse tool) on the active layer/frame, centered on a sprite-global "
    "point. With fill the circle is filled. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "center_x", json11::Json::object {
            { "type", "integer" },
            { "description", "X coordinate of the circle center" } } },
        { "center_y", json11::Json::object {
            { "type", "integer" },
            { "description", "Y coordinate of the circle center" } } },
        { "radius", json11::Json::object {
            { "type", "integer" },
            { "description", "Radius in pixels" } } },
        { "color", json11::Json::object {
            { "type", "string" },
            { "description", "Hex color #RRGGBB or #RRGGBBAA (default: #000000)" } } },
        { "fill", json11::Json::object {
            { "type", "boolean" },
            { "description", "Fill the circle instead of outlining it (default: false)" } } },
      } },
      { "required", json11::Json::array { "filename", "center_x", "center_y", "radius" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int centerX = argInt(args, "center_x");
      const int centerY = argInt(args, "center_y");
      const int radius = argInt(args, "radius");
      int r = 0, g = 0, b = 0, a = 255;
      if (!parseHexColor(argString(args, "color", "#000000"), r, g, b, a))
        return std::string("print(\"ERROR:Invalid color value: " +
                           luaString(argString(args, "color")) + "\")");

      const std::string tool = shapeToolLua(argBool(args, "fill"),
                                            "\"ellipse\"", "\"filled_ellipse\"");
      return std::string(kSpritePreludeLua) +
             kActiveCelStart +
             "  app.useTool({\n"
             "    tool = " + tool + ",\n"
             "    color = Color(" + std::to_string(r) + ", " + std::to_string(g) + ", " +
             std::to_string(b) + ", " + std::to_string(a) + "),\n"
             "    points = { Point(" + std::to_string(centerX - radius) + ", " +
             std::to_string(centerY - radius) + "), Point(" + std::to_string(centerX + radius) +
             ", " + std::to_string(centerY + radius) + ") }\n"
             "  })\n" + activeCelEnd();
    }));

  tools.push_back(LuaTool(
    "draw_pixels_at",
    "Draw pixels with individual colors on a named layer and 1-based frame. Coordinates are "
    "sprite-global. With create_if_missing a missing cel is created. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to draw on" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "pixels", json11::Json::object {
            { "type", "array" },
            { "description", "Pixels to draw, each { x, y, color }" },
            { "items", json11::Json::object {
              { "type", "object" },
              { "properties", json11::Json::object {
                { "x", json11::Json::object { { "type", "integer" },
                                              { "description", "X coordinate" } } },
                { "y", json11::Json::object { { "type", "integer" },
                                              { "description", "Y coordinate" } } },
                { "color", json11::Json::object {
                    { "type", "string" },
                    { "description", "Hex color #RRGGBB or #RRGGBBAA (default: #000000)" } } },
              } } } } } },
        { "create_if_missing", json11::Json::object {
            { "type", "boolean" },
            { "description", "Create the cel when it does not exist (default: true)" } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "frame_index", "pixels" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      std::string body, color;
      if (!pixelLua(args["pixels"], false, true, body, color))
        return std::string("print(\"ERROR:Invalid color value: " + luaString(color) + "\")");
      return std::string(kSpritePreludeLua) +
             kPSetLua +
             layerFrameCel(argString(args, "layer_name"),
                           argInt(args, "frame_index"),
                           argBool(args, "create_if_missing", true)) +
             body + layerFrameCelEnd();
    }));

  tools.push_back(LuaTool(
    "draw_line_at",
    "Draw a Bresenham line on a named layer and 1-based frame in sprite-global coordinates. "
    "With create_if_missing a missing cel is created. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to draw on" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "x1", json11::Json::object { { "type", "integer" },
                                       { "description", "Starting x coordinate" } } },
        { "y1", json11::Json::object { { "type", "integer" },
                                       { "description", "Starting y coordinate" } } },
        { "x2", json11::Json::object { { "type", "integer" },
                                       { "description", "Ending x coordinate" } } },
        { "y2", json11::Json::object { { "type", "integer" },
                                       { "description", "Ending y coordinate" } } },
        { "color", json11::Json::object {
            { "type", "string" },
            { "description", "Hex color #RRGGBB or #RRGGBBAA (default: #000000)" } } },
        { "thickness", json11::Json::object {
            { "type", "integer" },
            { "description", "Line thickness in pixels (default: 1)" } } },
        { "create_if_missing", json11::Json::object {
            { "type", "boolean" },
            { "description", "Create the cel when it does not exist (default: true)" } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "frame_index", "x1", "y1", "x2", "y2" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      int r = 0, g = 0, b = 0, a = 255;
      if (!parseHexColor(argString(args, "color", "#000000"), r, g, b, a))
        return std::string("print(\"ERROR:Invalid color value: " +
                           luaString(argString(args, "color")) + "\")");

      return std::string(kSpritePreludeLua) +
             kPSetLua +
             kDrawLineLua +
             layerFrameCel(argString(args, "layer_name"),
                           argInt(args, "frame_index"),
                           argBool(args, "create_if_missing", true)) +
             "  draw_line(img, " + std::to_string(argInt(args, "x1")) + ", " +
             std::to_string(argInt(args, "y1")) + ", " + std::to_string(argInt(args, "x2")) + ", " +
             std::to_string(argInt(args, "y2")) + ", Color(" + std::to_string(r) + ", " +
             std::to_string(g) + ", " + std::to_string(b) + ", " + std::to_string(a) + "), " +
             std::to_string(argInt(args, "thickness", 1)) + ")\n" + layerFrameCelEnd();
    }));

  tools.push_back(LuaTool(
    "draw_rectangle_at",
    "Draw a rectangle on a named layer and 1-based frame; x,y is the top-left corner and both "
    "sides must be > 0. With create_if_missing a missing cel is created. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to draw on" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "x", json11::Json::object { { "type", "integer" },
                                      { "description", "Top-left x coordinate" } } },
        { "y", json11::Json::object { { "type", "integer" },
                                      { "description", "Top-left y coordinate" } } },
        { "width", json11::Json::object {
            { "type", "integer" },
            { "description", "Width in pixels (must be > 0)" } } },
        { "height", json11::Json::object {
            { "type", "integer" },
            { "description", "Height in pixels (must be > 0)" } } },
        { "color", json11::Json::object {
            { "type", "string" },
            { "description", "Hex color #RRGGBB or #RRGGBBAA (default: #000000)" } } },
        { "fill", json11::Json::object {
            { "type", "boolean" },
            { "description", "Fill the rectangle instead of outlining it (default: false)" } } },
        { "create_if_missing", json11::Json::object {
            { "type", "boolean" },
            { "description", "Create the cel when it does not exist (default: true)" } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "frame_index", "x", "y", "width", "height" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int x = argInt(args, "x");
      const int y = argInt(args, "y");
      const int width = argInt(args, "width");
      const int height = argInt(args, "height");
      if (width <= 0 || height <= 0)
        return std::string("print(\"ERROR:Width and height must be > 0\")");

      int r = 0, g = 0, b = 0, a = 255;
      if (!parseHexColor(argString(args, "color", "#000000"), r, g, b, a))
        return std::string("print(\"ERROR:Invalid color value: " +
                           luaString(argString(args, "color")) + "\")");

      const std::string tool = shapeToolLua(argBool(args, "fill"),
                                            "\"rectangle\"", "\"filled_rectangle\"");
      return std::string(kSpritePreludeLua) +
             layerFrameCel(argString(args, "layer_name"),
                           argInt(args, "frame_index"),
                           argBool(args, "create_if_missing", true)) +
             "  app.useTool({\n"
             "    tool = " + tool + ",\n"
             "    color = Color(" + std::to_string(r) + ", " + std::to_string(g) + ", " +
             std::to_string(b) + ", " + std::to_string(a) + "),\n"
             "    points = { Point(" + std::to_string(x) + ", " + std::to_string(y) +
             "), Point(" + std::to_string(x + width - 1) + ", " + std::to_string(y + height - 1) +
             ") }\n"
             "  })\n" + layerFrameCelEnd();
    }));

  tools.push_back(LuaTool(
    "draw_circle_at",
    "Draw a circle on a named layer and 1-based frame, centered on a sprite-global point; the "
    "radius is in pixels. With create_if_missing a missing cel is created. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to draw on" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "center_x", json11::Json::object {
            { "type", "integer" },
            { "description", "X coordinate of the circle center" } } },
        { "center_y", json11::Json::object {
            { "type", "integer" },
            { "description", "Y coordinate of the circle center" } } },
        { "radius", json11::Json::object {
            { "type", "integer" },
            { "description", "Radius in pixels" } } },
        { "color", json11::Json::object {
            { "type", "string" },
            { "description", "Hex color #RRGGBB or #RRGGBBAA (default: #000000)" } } },
        { "fill", json11::Json::object {
            { "type", "boolean" },
            { "description", "Fill the circle instead of outlining it (default: false)" } } },
        { "create_if_missing", json11::Json::object {
            { "type", "boolean" },
            { "description", "Create the cel when it does not exist (default: true)" } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "frame_index", "center_x", "center_y", "radius" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int centerX = argInt(args, "center_x");
      const int centerY = argInt(args, "center_y");
      const int radius = argInt(args, "radius");
      int r = 0, g = 0, b = 0, a = 255;
      if (!parseHexColor(argString(args, "color", "#000000"), r, g, b, a))
        return std::string("print(\"ERROR:Invalid color value: " +
                           luaString(argString(args, "color")) + "\")");

      const std::string tool = shapeToolLua(argBool(args, "fill"),
                                            "\"ellipse\"", "\"filled_ellipse\"");
      return std::string(kSpritePreludeLua) +
             layerFrameCel(argString(args, "layer_name"),
                           argInt(args, "frame_index"),
                           argBool(args, "create_if_missing", true)) +
             "  app.useTool({\n"
             "    tool = " + tool + ",\n"
             "    color = Color(" + std::to_string(r) + ", " + std::to_string(g) + ", " +
             std::to_string(b) + ", " + std::to_string(a) + "),\n"
             "    points = { Point(" + std::to_string(centerX - radius) + ", " +
             std::to_string(centerY - radius) + "), Point(" + std::to_string(centerX + radius) +
             ", " + std::to_string(centerY + radius) + ") }\n"
             "  })\n" + layerFrameCelEnd();
    }));

  tools.push_back(LuaTool(
    "fill_area_at",
    "Flood-fill the region around a sprite-global point on a named layer and 1-based frame "
    "using the paint bucket tool. With create_if_missing a missing cel is created. The file "
    "is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to draw on" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "x", json11::Json::object { { "type", "integer" },
                                      { "description", "X coordinate to fill from" } } },
        { "y", json11::Json::object { { "type", "integer" },
                                      { "description", "Y coordinate to fill from" } } },
        { "color", json11::Json::object {
            { "type", "string" },
            { "description", "Hex color #RRGGBB or #RRGGBBAA (default: #000000)" } } },
        { "create_if_missing", json11::Json::object {
            { "type", "boolean" },
            { "description", "Create the cel when it does not exist (default: true)" } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "frame_index", "x", "y" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      int r = 0, g = 0, b = 0, a = 255;
      if (!parseHexColor(argString(args, "color", "#000000"), r, g, b, a))
        return std::string("print(\"ERROR:Invalid color value: " +
                           luaString(argString(args, "color")) + "\")");

      return std::string(kSpritePreludeLua) +
             layerFrameCel(argString(args, "layer_name"),
                           argInt(args, "frame_index"),
                           argBool(args, "create_if_missing", true)) +
             "  app.useTool({\n"
             "    tool = \"paint_bucket\",\n"
             "    color = Color(" + std::to_string(r) + ", " + std::to_string(g) + ", " +
             std::to_string(b) + ", " + std::to_string(a) + "),\n"
             "    points = { Point(" + std::to_string(argInt(args, "x")) + ", " +
             std::to_string(argInt(args, "y")) + ") }\n"
             "  })\n" + layerFrameCelEnd();
    }));

  tools.push_back(LuaTool(
    "draw_polygon",
    "Draw a closed polygon (at least 3 points) on a named layer and 1-based frame. Points are "
    "sprite-global { x, y }; with fill the interior is filled too. With create_if_missing a "
    "missing cel is created. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to draw on" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "points", json11::Json::object {
            { "type", "array" },
            { "description", "Polygon vertices, at least 3 { x, y } points" },
            { "items", json11::Json::object {
              { "type", "object" },
              { "properties", json11::Json::object {
                { "x", json11::Json::object { { "type", "integer" },
                                              { "description", "X coordinate" } } },
                { "y", json11::Json::object { { "type", "integer" },
                                              { "description", "Y coordinate" } } },
              } } } } } },
        { "color", json11::Json::object {
            { "type", "string" },
            { "description", "Hex color #RRGGBB or #RRGGBBAA (default: #000000)" } } },
        { "fill", json11::Json::object {
            { "type", "boolean" },
            { "description", "Fill the polygon instead of only outlining it (default: false)" } } },
        { "create_if_missing", json11::Json::object {
            { "type", "boolean" },
            { "description", "Create the cel when it does not exist (default: true)" } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "frame_index", "points" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const json11::Json& points = args["points"];
      if (!points.is_array() || points.array_items().size() < 3)
        return std::string("print(\"ERROR:Polygon requires at least 3 points\")");

      int r = 0, g = 0, b = 0, a = 255;
      if (!parseHexColor(argString(args, "color", "#000000"), r, g, b, a))
        return std::string("print(\"ERROR:Invalid color value: " +
                           luaString(argString(args, "color")) + "\")");

      return std::string(kSpritePreludeLua) +
             kPSetLua +
             kDrawLineLua +
             // Scanline fill for the polygon interior.
             "local function fill_polygon(img, pts, color)\n"
             "  local minY, maxY = pts[1].y, pts[1].y\n"
             "  for i = 2, #pts do\n"
             "    if pts[i].y < minY then minY = pts[i].y end\n"
             "    if pts[i].y > maxY then maxY = pts[i].y end\n"
             "  end\n"
             "  for y = minY, maxY do\n"
             "    local nodes = {}\n"
             "    local j = #pts\n"
             "    for i = 1, #pts do\n"
             "      local xi, yi = pts[i].x, pts[i].y\n"
             "      local xj, yj = pts[j].x, pts[j].y\n"
             "      if (yi < y and yj >= y) or (yj < y and yi >= y) then\n"
             "        table.insert(nodes, xi + (y - yi) * (xj - xi) / (yj - yi))\n"
             "      end\n"
             "      j = i\n"
             "    end\n"
             "    table.sort(nodes)\n"
             "    for k = 1, #nodes, 2 do\n"
             "      if nodes[k + 1] ~= nil then\n"
             "        for x = math.floor(nodes[k] + 0.5), math.floor(nodes[k + 1] + 0.5) do\n"
             "          pset(img, x, y, color)\n"
             "        end\n"
             "      end\n"
             "    end\n"
             "  end\n"
             "end\n" +
             layerFrameCel(argString(args, "layer_name"),
                           argInt(args, "frame_index"),
                           argBool(args, "create_if_missing", true)) +
             "  local color = Color(" + std::to_string(r) + ", " + std::to_string(g) + ", " +
             std::to_string(b) + ", " + std::to_string(a) + ")\n"
             "  local pts = { " + pointsLua(points) + " }\n" +
             (argBool(args, "fill") ? "  fill_polygon(img, pts, color)\n" : "") +
             "  for i = 1, #pts do\n"
             "    local n = i + 1\n"
             "    if n > #pts then n = 1 end\n"
             "    draw_line(img, pts[i].x, pts[i].y, pts[n].x, pts[n].y, color, 1)\n"
             "  end\n" + layerFrameCelEnd();
    }));

  tools.push_back(LuaTool(
    "draw_path",
    "Draw a polyline path (at least 2 points) on a named layer and 1-based frame. Points are "
    "sprite-global { x, y }. With create_if_missing a missing cel is created. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to draw on" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "points", json11::Json::object {
            { "type", "array" },
            { "description", "Path vertices, at least 2 { x, y } points" },
            { "items", json11::Json::object {
              { "type", "object" },
              { "properties", json11::Json::object {
                { "x", json11::Json::object { { "type", "integer" },
                                              { "description", "X coordinate" } } },
                { "y", json11::Json::object { { "type", "integer" },
                                              { "description", "Y coordinate" } } },
              } } } } } },
        { "color", json11::Json::object {
            { "type", "string" },
            { "description", "Hex color #RRGGBB or #RRGGBBAA (default: #000000)" } } },
        { "thickness", json11::Json::object {
            { "type", "integer" },
            { "description", "Path thickness in pixels (default: 1)" } } },
        { "create_if_missing", json11::Json::object {
            { "type", "boolean" },
            { "description", "Create the cel when it does not exist (default: true)" } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "frame_index", "points" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const json11::Json& points = args["points"];
      if (!points.is_array() || points.array_items().size() < 2)
        return std::string("print(\"ERROR:Path requires at least 2 points\")");

      int r = 0, g = 0, b = 0, a = 255;
      if (!parseHexColor(argString(args, "color", "#000000"), r, g, b, a))
        return std::string("print(\"ERROR:Invalid color value: " +
                           luaString(argString(args, "color")) + "\")");

      return std::string(kSpritePreludeLua) +
             kPSetLua +
             kDrawLineLua +
             layerFrameCel(argString(args, "layer_name"),
                           argInt(args, "frame_index"),
                           argBool(args, "create_if_missing", true)) +
             "  local color = Color(" + std::to_string(r) + ", " + std::to_string(g) + ", " +
             std::to_string(b) + ", " + std::to_string(a) + ")\n"
             "  local pts = { " + pointsLua(points) + " }\n"
             "  for i = 1, #pts - 1 do\n"
             "    draw_line(img, pts[i].x, pts[i].y, pts[i + 1].x, pts[i + 1].y, color, " +
             std::to_string(argInt(args, "thickness", 1)) + ")\n"
             "  end\n" + layerFrameCelEnd();
    }));

  tools.push_back(LuaTool(
    "apply_gradient_rect",
    "Fill a rectangle on a named layer and 1-based frame with a linear gradient between "
    "color_start and color_end. horizontal runs the gradient left to right, otherwise top to "
    "bottom. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to draw on" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "x", json11::Json::object { { "type", "integer" },
                                      { "description", "Top-left x coordinate" } } },
        { "y", json11::Json::object { { "type", "integer" },
                                      { "description", "Top-left y coordinate" } } },
        { "width", json11::Json::object {
            { "type", "integer" },
            { "description", "Width in pixels (must be > 0)" } } },
        { "height", json11::Json::object {
            { "type", "integer" },
            { "description", "Height in pixels (must be > 0)" } } },
        { "color_start", json11::Json::object {
            { "type", "string" },
            { "description", "Hex color at the start of the gradient" } } },
        { "color_end", json11::Json::object {
            { "type", "string" },
            { "description", "Hex color at the end of the gradient" } } },
        { "horizontal", json11::Json::object {
            { "type", "boolean" },
            { "description", "Gradient direction, left to right when true (default: true)" } } },
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

      int sr = 0, sg = 0, sb = 0, sa = 255;
      if (!parseHexColor(argString(args, "color_start"), sr, sg, sb, sa))
        return std::string("print(\"ERROR:Invalid color_start value: " +
                           luaString(argString(args, "color_start")) + "\")");
      int er = 0, eg = 0, eb = 0, ea = 255;
      if (!parseHexColor(argString(args, "color_end"), er, eg, eb, ea))
        return std::string("print(\"ERROR:Invalid color_end value: " +
                           luaString(argString(args, "color_end")) + "\")");

      return std::string(kSpritePreludeLua) +
             kPSetLua +
             layerFrameCel(argString(args, "layer_name"),
                           argInt(args, "frame_index"),
                           argBool(args, "create_if_missing", true)) +
             "  local w, h = " + std::to_string(width) + ", " + std::to_string(height) + "\n"
             "  for iy = 0, h - 1 do\n"
             "    for ix = 0, w - 1 do\n"
             "      local t\n"
             "      if " + (argBool(args, "horizontal", true) ? "true" : "false") + " then\n"
             "        t = (w > 1) and (ix / (w - 1)) or 0\n"
             "      else\n"
             "        t = (h > 1) and (iy / (h - 1)) or 0\n"
             "      end\n"
             "      pset(img, " + std::to_string(argInt(args, "x")) + " + ix, " +
             std::to_string(argInt(args, "y")) + " + iy, Color(\n"
             "        math.floor(" + std::to_string(sr) + " + (" + std::to_string(er) + " - " +
             std::to_string(sr) + ") * t + 0.5),\n"
             "        math.floor(" + std::to_string(sg) + " + (" + std::to_string(eg) + " - " +
             std::to_string(sg) + ") * t + 0.5),\n"
             "        math.floor(" + std::to_string(sb) + " + (" + std::to_string(eb) + " - " +
             std::to_string(sb) + ") * t + 0.5),\n"
             "        math.floor(" + std::to_string(sa) + " + (" + std::to_string(ea) + " - " +
             std::to_string(sa) + ") * t + 0.5)))\n"
             "    end\n"
             "  end\n" + layerFrameCelEnd();
    }));

  tools.push_back(LuaTool(
    "draw_ellipse_at",
    "Draw an ellipse on a named layer and 1-based frame, centered on a sprite-global point "
    "with separate horizontal and vertical radii. With create_if_missing a missing cel is "
    "created. The file is saved.",
    json11::Json::object {
      { "type", "object" },
      { "properties", json11::Json::object {
        { "filename", json11::Json::object {
            { "type", "string" },
            { "description", "Aseprite file to modify" } } },
        { "layer_name", json11::Json::object {
            { "type", "string" },
            { "description", "Layer to draw on" } } },
        { "frame_index", json11::Json::object {
            { "type", "integer" },
            { "description", "Frame index starting at 1" } } },
        { "center_x", json11::Json::object {
            { "type", "integer" },
            { "description", "X coordinate of the ellipse center" } } },
        { "center_y", json11::Json::object {
            { "type", "integer" },
            { "description", "Y coordinate of the ellipse center" } } },
        { "radius_x", json11::Json::object {
            { "type", "integer" },
            { "description", "Horizontal radius in pixels (must be > 0)" } } },
        { "radius_y", json11::Json::object {
            { "type", "integer" },
            { "description", "Vertical radius in pixels (must be > 0)" } } },
        { "color", json11::Json::object {
            { "type", "string" },
            { "description", "Hex color #RRGGBB or #RRGGBBAA (default: #000000)" } } },
        { "fill", json11::Json::object {
            { "type", "boolean" },
            { "description", "Fill the ellipse instead of outlining it (default: false)" } } },
        { "create_if_missing", json11::Json::object {
            { "type", "boolean" },
            { "description", "Create the cel when it does not exist (default: true)" } } },
      } },
      { "required", json11::Json::array {
        "filename", "layer_name", "frame_index", "center_x", "center_y", "radius_x", "radius_y" } },
    },
    true,
    [](const json11::Json& args) -> std::string {
      const int centerX = argInt(args, "center_x");
      const int centerY = argInt(args, "center_y");
      const int radiusX = argInt(args, "radius_x");
      const int radiusY = argInt(args, "radius_y");
      if (radiusX <= 0 || radiusY <= 0)
        return std::string("print(\"ERROR:radius_x and radius_y must be > 0\")");

      int r = 0, g = 0, b = 0, a = 255;
      if (!parseHexColor(argString(args, "color", "#000000"), r, g, b, a))
        return std::string("print(\"ERROR:Invalid color value: " +
                           luaString(argString(args, "color")) + "\")");

      const std::string tool = shapeToolLua(argBool(args, "fill"),
                                            "\"ellipse\"", "\"filled_ellipse\"");
      return std::string(kSpritePreludeLua) +
             layerFrameCel(argString(args, "layer_name"),
                           argInt(args, "frame_index"),
                           argBool(args, "create_if_missing", true)) +
             "  app.useTool({\n"
             "    tool = " + tool + ",\n"
             "    color = Color(" + std::to_string(r) + ", " + std::to_string(g) + ", " +
             std::to_string(b) + ", " + std::to_string(a) + "),\n"
             "    points = { Point(" + std::to_string(centerX - radiusX) + ", " +
             std::to_string(centerY - radiusY) + "), Point(" + std::to_string(centerX + radiusX) +
             ", " + std::to_string(centerY + radiusY) + ") }\n"
             "  })\n" + layerFrameCelEnd();
    }));

  register_text_tools(tools);
}

} // namespace mcp
} // namespace app
