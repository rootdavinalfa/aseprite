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
#include <vector>

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

  // The error from the last failed startHttp() (empty after a
  // successful start); the setup dialog shows it while stopped.
  std::string lastHttpError() const;

  // Starts the HTTP server and blocks until the process is
  // terminated (used by the headless --mcp-http mode).
  void runHttpOnly(int port);
#endif

private:
  json11::Json handleMessage(const json11::Json& request);

#ifdef ENABLE_WEBSOCKET
  // State shared between the ix worker thread serving a request and
  // the main thread that executes it in GUI mode; every request is
  // registered in m_pendingHttp while it is pending so stopHttp() can
  // cancel it instead of waiting for the marshaling timeout.
  struct PendingHttpRequest;

  std::shared_ptr<ix::HttpResponse> handleHttpRequest(
    const std::shared_ptr<ix::HttpRequest>& request);
  json11::Json dispatchHttp(const json11::Json& request, bool modern);
  void removePendingHttpRequest(const std::shared_ptr<PendingHttpRequest>& pending);

  std::unique_ptr<ix::HttpServer> m_httpServer;
  std::mutex m_httpDispatchMutex;
  std::string m_lastHttpError;

  std::vector<std::shared_ptr<PendingHttpRequest>> m_pendingHttp;
  std::mutex m_pendingHttpMutex;
  bool m_httpStopping = false; // guarded by m_pendingHttpMutex
#endif
};

} // namespace mcp
} // namespace app

#endif
