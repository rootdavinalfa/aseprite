// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_MCP_MCP_FONTS_H_INCLUDED
#define APP_MCP_MCP_FONTS_H_INCLUDED
#pragma once

#ifndef ENABLE_SCRIPTING
  #error ENABLE_SCRIPTING must be defined
#endif

#include "doc/color.h"

#include <string>
#include <vector>

namespace doc {
class Image;
}

namespace app {
namespace mcp {

// A font usable by measureText()/drawTextInImage(): either a bitmap
// font directory (holding a font.json descriptor plus its sheet PNGs)
// or a TrueType file.
struct FontInfo {
  std::string name;
  enum class Kind { Bitmap, TrueType } kind = Kind::Bitmap;
  std::string path;
};

struct TextRenderOptions {
  int size = 1;
  int letterSpacing = 0;
  bool bold = false;
  bool antialias = false;
};

// Ink extents of a text run: "width"/"height" are the ink box, and the
// baseline extents tell how far the ink reaches above and below the
// baseline (descenders count in belowBaseline). An empty run reports
// zeroes with the pen advance still set.
struct TextMetrics {
  int width = 0;
  int height = 0;
  int advanceWidth = 0;
  int aboveBaseline = 0;
  int belowBaseline = 0;
};

// Fonts under the ASEPRITE_MCP_FONTS directory (or ~/.aseprite-mcp/fonts
// by default) plus, on macOS, the system font directories. User fonts
// come first and the list is de-duplicated by name.
std::vector<FontInfo> listFonts();

// Resolves a font name from listFonts(), or a path to a .ttf/.otf file
// or to a bitmap font directory.
bool resolveFont(const std::string& nameOrPath, FontInfo& font, std::string& error);

// Measures a text run with the same layout drawTextInImage() uses.
bool measureText(const FontInfo& font,
                 const std::string& text,
                 const TextRenderOptions& options,
                 TextMetrics& metrics,
                 std::string& error);

// Draws one text run where (x, y) is the pen origin: x at the left
// edge of the ink box and y on the baseline. Glyphs are composited
// over the image with normal alpha blending. When "metrics" is not
// null it receives the same values measureText() reports.
bool drawTextInImage(const FontInfo& font,
                     const std::string& text,
                     doc::Image* image,
                     int x,
                     int y,
                     doc::color_t color,
                     const TextRenderOptions& options,
                     TextMetrics* metrics,
                     std::string& error);

} // namespace mcp
} // namespace app

#endif
