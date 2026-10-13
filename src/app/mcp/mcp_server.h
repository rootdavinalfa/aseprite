// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_MCP_MCP_SERVER_H_INCLUDED
#define APP_MCP_MCP_SERVER_H_INCLUDED
#pragma once

#ifndef ENABLE_SCRIPTING
  #error ENABLE_SCRIPTING must be defined
#endif

#include "json11.hpp"

namespace app {

class Context;

namespace script {
class Engine;
}

namespace mcp {

class McpServer {
public:
  static McpServer* instance();

  // Serves newline-delimited JSON-RPC messages from stdin/stdout
  // until stdin reaches EOF.
  void runStdio(script::Engine& engine, Context& context);

private:
  json11::Json handleMessage(const json11::Json& request);
};

} // namespace mcp
} // namespace app

#endif
