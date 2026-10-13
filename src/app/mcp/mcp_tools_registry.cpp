// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/mcp/mcp_tools_registry.h"

namespace app {
namespace mcp {

const std::vector<McpTool>& mcp_tools()
{
  static const std::vector<McpTool> tools;
  return tools;
}

} // namespace mcp
} // namespace app
