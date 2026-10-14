// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_MCP_MCP_TOOLS_REGISTRY_H_INCLUDED
#define APP_MCP_MCP_TOOLS_REGISTRY_H_INCLUDED
#pragma once

#ifndef ENABLE_SCRIPTING
  #error ENABLE_SCRIPTING must be defined
#endif

#include "json11.hpp"

#include <functional>
#include <string>
#include <vector>

namespace app {
namespace mcp {

struct ToolResult {
  bool isError = false;
  std::string text;
  std::string imageMime;
  std::string imageBase64;
};

struct McpTool {
  std::string name;
  std::string description;
  json11::Json inputSchema;
  std::function<ToolResult(const json11::Json& args)> run;
};

// Builds a tool whose body is a Lua snippet. "buildScript" receives the
// tool arguments and returns the Lua source; when "openFilename" is
// true the snippet runs with the file named by the "filename" argument
// already open as app.activeSprite. "after" can post-process a
// successful result (e.g. attach the PNG written by the snippet); it is
// not called for error results.
McpTool LuaTool(std::string name,
                std::string description,
                json11::Json inputSchema,
                bool openFilename,
                std::function<std::string(const json11::Json&)> buildScript,
                std::function<void(const json11::Json&, ToolResult&)> after = {});

// Builds a tool implemented directly in C++ (no Lua engine involved).
McpTool NativeTool(std::string name,
                   std::string description,
                   json11::Json inputSchema,
                   std::function<ToolResult(const json11::Json&)> fn);

std::string argString(const json11::Json& args, const char* name, const std::string& def = {});
int argInt(const json11::Json& args, const char* name, int def = 0);
double argDouble(const json11::Json& args, const char* name, double def = 0.0);
bool argBool(const json11::Json& args, const char* name, bool def = false);

// Accepts #RGB, #RGBA, #RRGGBB, or #RRGGBBAA (the leading # is
// optional) and fills r, g, b, a. Returns false on malformed input.
bool parseHexColor(const std::string& value, int& r, int& g, int& b, int& a);

// Escapes a string for use inside a Lua double-quoted literal.
std::string luaString(const std::string& value);

// Absolute path with symlinks/aliases resolved (base::get_canonical_path),
// falling back to the plain absolute path when the file does not exist.
std::string canonicalFilePath(const std::string& filename);

void register_canvas_draw_tools(std::vector<McpTool>& tools);
void register_layers_tools(std::vector<McpTool>& tools);
void register_animation_tools(std::vector<McpTool>& tools);
void register_palette_transform_tools(std::vector<McpTool>& tools);
void register_io_tools(std::vector<McpTool>& tools);
void register_misc_tools(std::vector<McpTool>& tools);

// Registered tools in deterministic order.
const std::vector<McpTool>& mcp_tools();

} // namespace mcp
} // namespace app

#endif
