// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_MCP_MCP_TOOL_CONTEXT_H_INCLUDED
#define APP_MCP_MCP_TOOL_CONTEXT_H_INCLUDED
#pragma once

#ifndef ENABLE_SCRIPTING
  #error ENABLE_SCRIPTING must be defined
#endif

#include "app/mcp/mcp_tools_registry.h"
#include "base/disable_copying.h"
#include "base/time.h"

#include <cstddef>
#include <mutex>
#include <optional>
#include <string>

namespace app {

class Context;

namespace script {
class Engine;
}

namespace mcp {

// Shared state for tool execution: the script engine that runs Lua
// snippets, the app context used to open documents, and the file name,
// modification time, and size of the last document opened (used to
// detect external changes).
class ToolContext {
public:
  static ToolContext& instance();

  void setEngine(script::Engine* engine) { m_engine = engine; }
  void setContext(Context* context) { m_context = context; }

  // Makes the given file the active document, opening it when needed.
  // Returns false and fills "error" when the file doesn't exist or
  // cannot be loaded.
  bool openDocument(const std::string& filename, std::string& error);

  // Runs a Lua snippet capturing print()/error output. A Lua failure
  // or text starting with "ERROR:" yields an error ToolResult.
  ToolResult runLua(const std::string& code);

private:
  ToolContext() = default;
  DISABLE_COPYING(ToolContext);

  script::Engine* m_engine = nullptr;
  Context* m_context = nullptr;
  std::mutex m_mutex;

  // File name, modification time, and size of the last document
  // opened by openDocument(), used to detect external changes.
  std::string m_cachedFilename;
  std::optional<base::Time> m_cachedTime;
  std::size_t m_cachedSize = 0;
};

} // namespace mcp
} // namespace app

#endif
