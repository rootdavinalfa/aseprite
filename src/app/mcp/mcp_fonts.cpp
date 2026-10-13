// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/mcp/mcp_fonts.h"

#include "app/modules/palettes.h"
#include "base/fs.h"
#include "base/string.h"
#include "base/utf8_decode.h"
#include "doc/blend_image.h"
#include "doc/image.h"
#include "doc/image_ref.h"
#include "doc/primitives.h"
#include "ft/freetype_headers.h"
#include "ft/lib.h"
#include "json11.hpp"
#include "png.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>

namespace app {
namespace mcp {

namespace {

// Ink points of a run, in layout coordinates: x from the pen origin,
// y from the baseline (negative above it).
using Ink = std::set<std::pair<int, int>>;

struct Glyph {
  Ink ink;         // offsets from the glyph box's top-left corner
  int advance = 0; // pen advance for this glyph, before letter spacing
  int ascent = 0;  // rows from the glyph box top down to the baseline
};

struct InkBounds {
  int minX = 0;
  int minY = 0;
  int maxX = -1; // maxX < minX marks an empty run
  int maxY = -1;
};

bool hasExtension(const std::string& path, const char* ext)
{
  return base::utf8_icmp(base::get_file_extension(path), ext) == 0;
}

bool isTrueTypeFile(const std::string& path)
{
  return hasExtension(path, "ttf") || hasExtension(path, "otf") || hasExtension(path, "ttc");
}

std::string homeDir()
{
#if LAF_WINDOWS
  const char* home = std::getenv("USERPROFILE");
#else
  const char* home = std::getenv("HOME");
#endif
  return home ? home : std::string();
}

std::string expandTilde(const std::string& path)
{
  if (path.size() >= 2 && path[0] == '~' && base::is_path_separator(path[1]))
    return base::join_path(homeDir(), path.substr(2));
  return path;
}

// The directory searched for user fonts: ASEPRITE_MCP_FONTS when set,
// otherwise ~/.aseprite-mcp/fonts.
std::string userFontDir()
{
  const char* env = std::getenv("ASEPRITE_MCP_FONTS");
  if (env && *env)
    return expandTilde(env);
  return base::join_path(homeDir(), ".aseprite-mcp/fonts");
}

// Decodes a PNG into RGBA8 rows (libpng's simplified API).
bool loadSheetPng(const std::string& path,
                  std::vector<uint8_t>& rgba,
                  int& width,
                  int& height,
                  std::string& error)
{
  png_image image;
  std::memset(&image, 0, sizeof(image));
  image.version = PNG_IMAGE_VERSION;

  if (!png_image_begin_read_from_file(&image, path.c_str())) {
    error = "Could not read " + path + ": " + image.message;
    return false;
  }

  image.format = PNG_FORMAT_RGBA;
  rgba.resize(PNG_IMAGE_SIZE(image));
  if (!png_image_finish_read(&image, nullptr, rgba.data(), 0, nullptr)) {
    error = "Could not decode " + path + ": " + image.message;
    png_image_free(&image);
    return false;
  }

  width = image.width;
  height = image.height;
  png_image_free(&image);
  return true;
}

// A sprite-sheet font sheet: a fixed cell grid over one decoded PNG.
struct BitmapSheet {
  std::vector<uint8_t> rgba;
  int width = 0;
  int height = 0;
  int cellW = 0;
  int cellH = 0;
  int ascent = 0;
  int originX = 0;
  int originY = 0;
  bool dark = false;       // "dark" ink rule: white background, dark pixels are ink
  bool boxAdvance = false; // "box" advance rule: the cell box is the advance
};

class BitmapFont {
public:
  bool load(const std::string& dir, std::string& error)
  {
    const std::string descriptor = base::join_path(dir, "font.json");
    std::ifstream in(descriptor, std::ios::binary);
    if (!in) {
      error = "Could not read " + descriptor;
      return false;
    }
    const std::string text((std::istreambuf_iterator<char>(in)),
                           std::istreambuf_iterator<char>());

    std::string parseError;
    const json11::Json spec = json11::Json::parse(text, parseError);
    if (!parseError.empty()) {
      error = "Could not read " + descriptor + ": " + parseError;
      return false;
    }

    m_letterGap = spec["letter_gap"].is_number() ? spec["letter_gap"].int_value() : 1;
    m_spaceWidth = spec["space_width"].is_number() ? spec["space_width"].int_value() : 3;

    for (const json11::Json& sheet : spec["sheets"].array_items()) {
      BitmapSheet s;
      const std::string file = sheet["file"].string_value();
      if (file.empty() || !sheet["cell_w"].is_number() || !sheet["cell_h"].is_number()) {
        error = "Bad sheet in " + descriptor;
        return false;
      }
      if (!loadSheetPng(base::join_path(dir, file), s.rgba, s.width, s.height, error))
        return false;

      s.cellW = sheet["cell_w"].int_value();
      s.cellH = sheet["cell_h"].int_value();
      s.ascent = sheet["ascent"].is_number() ? sheet["ascent"].int_value() : s.cellH;
      s.dark = sheet["ink_rule"].string_value() == "dark";
      s.boxAdvance = sheet["advance"].string_value() == "box";
      const json11::Json& origin = sheet["origin"];
      if (origin.is_array() && origin.array_items().size() == 2) {
        s.originX = origin.array_items()[0].int_value();
        s.originY = origin.array_items()[1].int_value();
      }

      // Sheets are consulted in order and the first one to claim a
      // codepoint wins.
      const int sheetIndex = int(m_sheets.size());
      int row = 0;
      for (const json11::Json& line : sheet["chars"].array_items()) {
        int col = 0;
        for (base::utf8_decode chars(line.string_value()); !chars.is_end();) {
          const base::codepoint_t cp = chars.next();
          if (cp && m_index.find(cp) == m_index.end())
            m_index[cp] = Location { sheetIndex, row, col };
          ++col;
        }
        ++row;
      }

      m_sheets.push_back(std::move(s));
    }

    if (m_sheets.empty()) {
      error = descriptor + " declares no sheets";
      return false;
    }

    for (const auto& it : spec["overrides"].object_items()) {
      const int codepoint = std::atoi(it.first.c_str());
      const json11::Json& entry = it.second;
      Glyph glyph;
      int width = 0;
      int row = 0;
      for (const json11::Json& line : entry["rows"].array_items()) {
        int col = 0;
        for (base::utf8_decode chars(line.string_value()); !chars.is_end();) {
          const base::codepoint_t ch = chars.next();
          if (ch != '.' && ch != ' ') {
            glyph.ink.insert(std::make_pair(col, row));
            width = std::max(width, col + 1);
          }
          ++col;
        }
        ++row;
      }
      glyph.advance = width + m_letterGap;
      glyph.ascent = entry["ascent"].is_number() ? entry["ascent"].int_value() : row;
      m_overrides[codepoint] = glyph;
    }

    return true;
  }

  // Fills "glyph" for the codepoint; false when the font doesn't
  // declare it (the run then skips it without advancing).
  bool glyphFor(const int codepoint, Glyph& glyph) const
  {
    const auto override = m_overrides.find(codepoint);
    if (override != m_overrides.end()) {
      glyph = override->second;
      return true;
    }

    const auto location = m_index.find(codepoint);
    if (location == m_index.end())
      return false;

    readCell(m_sheets[location->second.sheet],
             location->second.row,
             location->second.col,
             glyph);
    return true;
  }

private:
  struct Location {
    int sheet = 0;
    int row = 0;
    int col = 0;
  };

  bool isBackground(const BitmapSheet& sheet, const int x, const int y) const
  {
    const uint8_t* px = &sheet.rgba[(y * sheet.width + x) * 4];
    return px[3] == 255 && px[0] == 255 && px[1] == 255 && px[2] == 255;
  }

  void readCell(const BitmapSheet& sheet, const int row, const int col, Glyph& glyph) const
  {
    const int ox = sheet.originX + col * sheet.cellW;
    const int oy = sheet.originY + row * sheet.cellH;
    int boxW = std::max(0, std::min(sheet.cellW, sheet.width - ox));
    int boxH = std::max(0, std::min(sheet.cellH, sheet.height - oy));

    if (sheet.dark) {
      // The box runs from the cell origin until the white background.
      boxW = 0;
      while (boxW < sheet.cellW && ox + boxW < sheet.width && !isBackground(sheet, ox + boxW, oy))
        ++boxW;
      boxH = 0;
      while (boxH < sheet.cellH && oy + boxH < sheet.height && !isBackground(sheet, ox, oy + boxH))
        ++boxH;
    }

    glyph.ink.clear();
    glyph.ascent = sheet.ascent;
    int inkWidth = 0;
    for (int y = 0; y < boxH; ++y) {
      for (int x = 0; x < boxW; ++x) {
        const uint8_t* px = &sheet.rgba[((oy + y) * sheet.width + (ox + x)) * 4];
        const bool lit = sheet.dark ? (px[3] > 0 && std::max({ px[0], px[1], px[2] }) < 128)
                                    : (px[3] > 0);
        if (lit) {
          glyph.ink.insert(std::make_pair(x, y));
          inkWidth = std::max(inkWidth, x + 1);
        }
      }
    }

    glyph.advance = sheet.boxAdvance
                      ? (boxW ? boxW : m_spaceWidth)
                      : (inkWidth ? inkWidth : m_spaceWidth) + m_letterGap;
  }

  int m_letterGap = 1;
  int m_spaceWidth = 3;
  std::vector<BitmapSheet> m_sheets;
  std::map<int, Location> m_index;
  std::map<int, Glyph> m_overrides;
};

// Loaded bitmap fonts are kept around because one draw_text call
// shapes the same run once per outline/shadow pass.
std::map<std::string, std::shared_ptr<BitmapFont>>& bitmapFontCache()
{
  static std::map<std::string, std::shared_ptr<BitmapFont>> cache;
  return cache;
}

void dilateInk(Ink& ink)
{
  Ink grown = ink;
  for (const auto& point : ink) {
    grown.insert(std::make_pair(point.first + 1, point.second));
    grown.insert(std::make_pair(point.first, point.second + 1));
  }
  ink.swap(grown);
}

InkBounds boundsOf(const Ink& ink)
{
  InkBounds bounds;
  if (ink.empty())
    return bounds;
  bounds.minX = ink.begin()->first;
  bounds.maxX = ink.begin()->first;
  for (const auto& point : ink) {
    bounds.minX = std::min(bounds.minX, point.first);
    bounds.maxX = std::max(bounds.maxX, point.first);
    bounds.minY = std::min(bounds.minY, point.second);
    bounds.maxY = std::max(bounds.maxY, point.second);
  }
  return bounds;
}

void measureInk(const Ink& ink, const int advance, TextMetrics& metrics)
{
  metrics = TextMetrics();
  metrics.advanceWidth = advance;
  const InkBounds bounds = boundsOf(ink);
  if (bounds.maxX < bounds.minX)
    return;
  metrics.width = bounds.maxX - bounds.minX + 1;
  metrics.height = bounds.maxY - bounds.minY + 1;
  metrics.aboveBaseline = -bounds.minY;
  metrics.belowBaseline = bounds.maxY + 1;
}

bool shapeBitmap(const FontInfo& font,
                 const std::string& text,
                 const TextRenderOptions& options,
                 Ink& ink,
                 int& advance,
                 std::string& error)
{
  if (options.size < 1) {
    error = "size is an integer scale factor for bitmap fonts; must be >= 1";
    return false;
  }

  std::shared_ptr<BitmapFont>& cached = bitmapFontCache()[font.path];
  if (!cached) {
    cached = std::make_shared<BitmapFont>();
    if (!cached->load(font.path, error)) {
      bitmapFontCache().erase(font.path);
      return false;
    }
  }

  const int scale = options.size;
  int pen = 0;
  bool any = false;
  for (base::utf8_decode chars(text); !chars.is_end();) {
    const base::codepoint_t cp = chars.next();
    Glyph glyph;
    if (!cached->glyphFor(cp, glyph))
      continue;

    for (const auto& point : glyph.ink) {
      const int baseX = pen + point.first * scale;
      const int baseY = (point.second - glyph.ascent) * scale;
      for (int dy = 0; dy < scale; ++dy) {
        for (int dx = 0; dx < scale; ++dx)
          ink.insert(std::make_pair(baseX + dx, baseY + dy));
      }
    }

    pen += glyph.advance * scale + options.letterSpacing;
    any = true;
  }

  advance = std::max(0, pen - (any ? options.letterSpacing : 0));
  return true;
}

bool shapeTrueType(const FontInfo& font,
                   const std::string& text,
                   const TextRenderOptions& options,
                   Ink& ink,
                   int& advance,
                   std::string& error)
{
  if (options.size < 1) {
    error = "size is a pixel height for TrueType fonts; must be >= 1";
    return false;
  }

  ft::Lib lib;
  FT_Face face = lib.open(font.path);
  if (!face) {
    error = "Could not load font " + font.path;
    return false;
  }

  FT_Set_Pixel_Sizes(face, options.size, options.size);
  const int loadFlags = FT_LOAD_RENDER |
                        (options.antialias ? (FT_LOAD_TARGET_NORMAL | FT_LOAD_NO_BITMAP)
                                           : FT_LOAD_TARGET_MONO);
  // Antialiased edges are kept as hard pixels above this coverage.
  const int cutoff = options.antialias ? 1 : 128;

  double pen = 0;
  bool any = false;
  for (base::utf8_decode chars(text); !chars.is_end();) {
    const base::codepoint_t cp = chars.next();
    if (FT_Load_Char(face, cp, loadFlags) != 0)
      continue;

    const FT_GlyphSlot slot = face->glyph;
    const int baseX = int(std::lround(pen)) + slot->bitmap_left;
    const int baseY = -slot->bitmap_top;
    const FT_Bitmap& bitmap = slot->bitmap;
    if (bitmap.pixel_mode == FT_PIXEL_MODE_MONO) {
      for (int y = 0; y < int(bitmap.rows); ++y) {
        const uint8_t* src = bitmap.buffer + y * bitmap.pitch;
        for (int x = 0; x < int(bitmap.width); ++x) {
          if (src[x >> 3] & (0x80 >> (x & 7)))
            ink.insert(std::make_pair(baseX + x, baseY + y));
        }
      }
    }
    else if (bitmap.pixel_mode == FT_PIXEL_MODE_GRAY) {
      for (int y = 0; y < int(bitmap.rows); ++y) {
        const uint8_t* src = bitmap.buffer + y * bitmap.pitch;
        for (int x = 0; x < int(bitmap.width); ++x) {
          if (src[x] >= cutoff)
            ink.insert(std::make_pair(baseX + x, baseY + y));
        }
      }
    }

    pen += slot->advance.x / 64.0;
    pen += options.letterSpacing;
    any = true;
  }

  advance = int(std::lround(pen - (any ? options.letterSpacing : 0)));
  return true;
}

// Lays out the run in pen coordinates. Both measureText() and
// drawTextInImage() go through here, so their metrics always agree.
bool shape(const FontInfo& font,
           const std::string& text,
           const TextRenderOptions& options,
           Ink& ink,
           int& advance,
           std::string& error)
{
  if (font.kind == FontInfo::Kind::Bitmap) {
    if (!shapeBitmap(font, text, options, ink, advance, error))
      return false;
  }
  else {
    if (!shapeTrueType(font, text, options, ink, advance, error))
      return false;
  }

  if (options.bold && !ink.empty()) {
    dilateInk(ink);
    advance += 1;
  }

  // Ink is positioned so its box starts at the pen origin on x; the
  // reference behavior subtracts the left bearing before anchoring.
  if (!ink.empty()) {
    const int left = ink.begin()->first;
    if (left != 0) {
      Ink shifted;
      for (const auto& point : ink)
        shifted.insert(std::make_pair(point.first - left, point.second));
      ink.swap(shifted);
    }
  }

  return true;
}

std::vector<std::string> sortedDirectory(const std::string& dir)
{
  std::vector<std::string> names = base::list_files(dir, base::ItemType::All);
  std::sort(names.begin(), names.end());
  return names;
}

} // anonymous namespace

std::vector<FontInfo> listFonts()
{
  std::vector<FontInfo> fonts;
  std::set<std::string> seen;

  auto add = [&](const std::string& name, const FontInfo::Kind kind, const std::string& path) {
    if (!seen.insert(base::string_to_lower(name)).second)
      return;
    FontInfo font;
    font.name = name;
    font.kind = kind;
    font.path = path;
    fonts.push_back(font);
  };

  const std::string dir = userFontDir();
  if (base::is_directory(dir)) {
    for (const std::string& entry : sortedDirectory(dir)) {
      const std::string full = base::join_path(dir, entry);
      if (base::is_directory(full) && base::is_file(base::join_path(full, "font.json")))
        add(entry, FontInfo::Kind::Bitmap, full);
      else if (base::is_file(full) && isTrueTypeFile(entry))
        add(base::get_file_title(entry), FontInfo::Kind::TrueType, full);
    }
  }

#if LAF_MACOS
  const std::string systemDirs[] = {
    "/System/Library/Fonts",
    "/Library/Fonts",
    base::join_path(homeDir(), "Library/Fonts"),
  };
  for (const std::string& systemDir : systemDirs) {
    if (!base::is_directory(systemDir))
      continue;
    for (const std::string& entry : sortedDirectory(systemDir)) {
      const std::string full = base::join_path(systemDir, entry);
      if (base::is_file(full) && isTrueTypeFile(entry))
        add(base::get_file_title(entry), FontInfo::Kind::TrueType, full);
    }
  }
#endif

  return fonts;
}

bool resolveFont(const std::string& nameOrPath, FontInfo& font, std::string& error)
{
  const bool pathLike = nameOrPath.find('/') != std::string::npos ||
                        nameOrPath.find('\\') != std::string::npos || isTrueTypeFile(nameOrPath);
  if (pathLike) {
    const std::string path = expandTilde(nameOrPath);
    if (base::is_directory(path) && base::is_file(base::join_path(path, "font.json"))) {
      font.name = base::get_file_title(path);
      font.kind = FontInfo::Kind::Bitmap;
      font.path = path;
      return true;
    }
    if (base::is_file(path)) {
      font.name = base::get_file_title(path);
      font.kind = FontInfo::Kind::TrueType;
      font.path = path;
      return true;
    }
  }

  for (const FontInfo& candidate : listFonts()) {
    if (base::utf8_icmp(candidate.name, nameOrPath) == 0) {
      font = candidate;
      return true;
    }
  }

  error = "Font '" + nameOrPath + "' not found. Call list_text_fonts to see what is "
          "available, or pass a path to a .ttf/.otf file or to a bitmap font directory.";
  return false;
}

bool measureText(const FontInfo& font,
                 const std::string& text,
                 const TextRenderOptions& options,
                 TextMetrics& metrics,
                 std::string& error)
{
  Ink ink;
  int advance = 0;
  if (!shape(font, text, options, ink, advance, error))
    return false;
  measureInk(ink, advance, metrics);
  return true;
}

bool drawTextInImage(const FontInfo& font,
                     const std::string& text,
                     doc::Image* image,
                     const int x,
                     const int y,
                     const doc::color_t color,
                     const TextRenderOptions& options,
                     TextMetrics* metrics,
                     std::string& error)
{
  if (!image) {
    error = "No image to draw on";
    return false;
  }

  Ink ink;
  int advance = 0;
  if (!shape(font, text, options, ink, advance, error))
    return false;

  TextMetrics computed;
  measureInk(ink, advance, metrics ? *metrics : computed);
  if (ink.empty())
    return true;

  const InkBounds bounds = boundsOf(ink);
  const int stampW = bounds.maxX - bounds.minX + 1;
  const int stampH = bounds.maxY - bounds.minY + 1;
  if (stampW > 4096 || stampH > 4096) {
    // A letter-spacing typo can ask for a gigapixel run; fail fast
    // instead of trying to allocate it.
    error = "Text is too large to render (" + std::to_string(stampW) + "x" +
            std::to_string(stampH) + ")";
    return false;
  }

  doc::ImageRef stamp(doc::Image::create(doc::IMAGE_RGB, stampW, stampH));
  doc::clear_image(stamp.get(), 0);
  for (const auto& point : ink)
    doc::put_pixel(stamp.get(), point.first - bounds.minX, point.second - bounds.minY, color);

  doc::blend_image(image,
                   stamp.get(),
                   gfx::Clip(gfx::Point(x + bounds.minX, y + bounds.minY), stamp->bounds()),
                   app::get_current_palette(),
                   255,
                   doc::BlendMode::NORMAL);
  return true;
}

} // namespace mcp
} // namespace app
