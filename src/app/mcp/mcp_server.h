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

#include <memory>
#include <mutex>
#include <string>

namespace ix {
struct HttpRequest;
struct HttpResponse;
class HttpServer;
}

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

#ifdef ENABLE_WEBSOCKET
  // Serves MCP over HTTP on 127.0.0.1:<port> (single endpoint
  // POST /mcp). Returns false and fills "error" when the port can't
  // be bound.
  bool startHttp(int port, std::string& error);
  void stopHttp();
  bool isHttpRunning() const;

  // Starts the HTTP server and blocks until the process is
  // terminated (used by the headless --mcp-http mode).
  void runHttpOnly(int port);
#endif

private:
  json11::Json handleMessage(const json11::Json& request);

#ifdef ENABLE_WEBSOCKET
  std::shared_ptr<ix::HttpResponse> handleHttpRequest(
    const std::shared_ptr<ix::HttpRequest>& request);
  json11::Json dispatchHttp(const json11::Json& request);

  std::unique_ptr<ix::HttpServer> m_httpServer;
  std::mutex m_httpDispatchMutex;
#endif
};

} // namespace mcp
} // namespace app

#endif
