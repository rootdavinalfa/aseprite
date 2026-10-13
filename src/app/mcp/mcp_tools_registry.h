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

// Registered tools in deterministic order.
const std::vector<McpTool>& mcp_tools();

} // namespace mcp
} // namespace app

#endif
