// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#include "app/mcp/mcp_server.h"

#include "app/mcp/mcp_protocol.h"
#include "app/mcp/mcp_tool_context.h"

#include <cstdio>
#include <iostream>
#include <string>

#ifdef ENABLE_WEBSOCKET
  #include "app/app.h"
  #include "base/base64.h"
  #include "os/event.h"
  #include "os/event_queue.h"

  #include <ixwebsocket/IXHttpServer.h>
  #include <ixwebsocket/IXNetSystem.h>

  #include <algorithm>
  #include <chrono>
  #include <condition_variable>
  #include <cstdlib>
  #include <cstring>
  #include <exception>
  #include <mutex>
#endif

#if LAF_WINDOWS
  #include <io.h>
#else
  #include <unistd.h>
#endif

#include <cstddef>

namespace app {
namespace mcp {

namespace {

int dupStdout()
{
#if LAF_WINDOWS
  return _dup(1);
#else
  return dup(1);
#endif
}

void redirectStdoutToStderr()
{
#if LAF_WINDOWS
  _dup2(2, 1);
#else
  dup2(2, 1);
#endif
}

void restoreStdout(const int fd)
{
#if LAF_WINDOWS
  _dup2(fd, 1);
  _close(fd);
#else
  dup2(fd, 1);
  close(fd);
#endif
}

// Writes the whole buffer to fd; stdio buffering doesn't apply here.
void writeAll(const int fd, const std::string& data)
{
  std::size_t pos = 0;
  while (pos < data.size()) {
#if LAF_WINDOWS
    const int n = _write(fd, data.data() + pos, static_cast<unsigned int>(data.size() - pos));
#else
    const int n = static_cast<int>(::write(fd, data.data() + pos, data.size() - pos));
#endif
    if (n <= 0) // Broken pipe; the client is gone
      return;
    pos += n;
  }
}

#ifdef ENABLE_WEBSOCKET

// How long a GUI-mode request waits for the main thread to answer it.
const int kHttpTimeoutSecs = 120;

std::string headerValue(const ix::HttpRequest& request, const char* name)
{
  const auto it = request.headers.find(name);
  return it != request.headers.end() ? it->second : std::string();
}

// Only loopback pages may talk to the server (DNS-rebinding defense).
bool isLoopbackOrigin(const std::string& origin)
{
  for (const char* prefix : { "http://localhost", "https://localhost",
                              "http://127.0.0.1", "https://127.0.0.1" }) {
    const std::size_t n = std::strlen(prefix);
    if (origin.size() < n || origin.compare(0, n, prefix) != 0)
      continue;
    const char next = (origin.size() > n ? origin[n] : '\0');
    if (next == '\0' || next == ':' || next == '/')
      return true;
  }
  return false;
}

// Mcp-* header values can be carried base64 encoded with the
// "=?base64?<data>?=" sentinel.
std::string decodeMcpHeader(const std::string& value)
{
  const std::string prefix = "=?base64?";
  const std::string suffix = "?=";
  if (value.size() >= prefix.size() + suffix.size() &&
      value.compare(0, prefix.size(), prefix) == 0 &&
      value.compare(value.size() - suffix.size(), suffix.size(), suffix) == 0) {
    return base::decode_base64s(
      value.substr(prefix.size(), value.size() - prefix.size() - suffix.size()));
  }
  return value;
}

ix::HttpResponsePtr httpResponse(const int status, const char* description,
                                 const std::string& body,
                                 const ix::WebSocketHttpHeaders& extraHeaders = {})
{
  ix::WebSocketHttpHeaders headers = extraHeaders;
  if (!body.empty())
    headers["Content-Type"] = "application/json";
  return std::make_shared<ix::HttpResponse>(status, description, ix::HttpErrorCode::Ok,
                                            headers, body);
}

ix::HttpResponsePtr httpErrorJson(const int status, const char* description,
                                  const int code, const std::string& message,
                                  const json11::Json& id = json11::Json(),
                                  const json11::Json& data = json11::Json())
{
  return httpResponse(status, description,
                      protocol::dumpCompact(protocol::makeError(id, code, message, data)));
}

#endif // ENABLE_WEBSOCKET

} // anonymous namespace

McpServer* McpServer::instance()
{
  static McpServer server;
  return &server;
}

void McpServer::runStdio(script::Engine& engine, Context& context)
{
  ToolContext::instance().setEngine(&engine);
  ToolContext::instance().setContext(&context);

  // Safety net: stdout carries nothing but MCP messages. The whole
  // file descriptor 1 is redirected to stderr for the duration of the
  // loop (this covers C stdio writes like Console::printf, not just
  // std::cout), and the responses go to the saved original descriptor.
  fflush(stdout);
  const int stdoutFd = dupStdout();
  if (stdoutFd < 0)
    return;

  redirectStdoutToStderr();

  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty())
      continue;

    std::string parseError;
    const json11::Json request = json11::Json::parse(line, parseError);
    const json11::Json response =
      (parseError.empty() ? handleMessage(request)
                          : protocol::makeError(json11::Json(), -32700, "Parse error"));

    if (!response.is_null())
      writeAll(stdoutFd, protocol::dumpCompact(response) + '\n');
  }

  // Flush what was buffered on the redirected stdout before restoring
  // it, so it can't leak after the loop.
  fflush(stdout);
  restoreStdout(stdoutFd);
}

#ifdef ENABLE_WEBSOCKET

// State shared between an ix worker thread and the main thread that
// runs a marshaled request in GUI mode.
struct McpServer::PendingHttpRequest {
  std::mutex mutex;
  std::condition_variable cond;
  bool done = false;
  bool cancelled = false;
  json11::Json response;
};

bool McpServer::startHttp(const int port, std::string& error)
{
  // Replace any previous instance (e.g. a port change from the dialog).
  stopHttp();
  {
    std::lock_guard<std::mutex> lock(m_pendingHttpMutex);
    m_httpStopping = false;
  }

  if (App* app = App::instance()) {
    ToolContext::instance().setEngine(app->scriptEngine());
    ToolContext::instance().setContext(app->context());
  }

  // The scripting WebSocket class initializes the net system on the
  // first connection; do the same before creating the server.
  static std::once_flag netOnce;
  std::call_once(netOnce, &ix::initNetSystem);

  auto server = std::make_unique<ix::HttpServer>(port, "127.0.0.1");
  server->setOnConnectionCallback(
    [](ix::HttpRequestPtr request, std::shared_ptr<ix::ConnectionState>) -> ix::HttpResponsePtr {
      return McpServer::instance()->handleHttpRequest(request);
    });

  const std::pair<bool, std::string> listenResult = server->listen();
  if (!listenResult.first) {
    error = listenResult.second;
    return false;
  }

  m_httpServer = std::move(server);
  m_httpServer->start();
  return true;
}

void McpServer::stopHttp()
{
  if (!m_httpServer)
    return;

  // Cancel every request that is still waiting to be marshaled to the
  // main thread so stop() can join the connection threads right away
  // instead of after the 120s timeout, and reject requests that reach
  // the dispatch while the server is stopping.
  std::vector<std::shared_ptr<PendingHttpRequest>> pending;
  {
    std::lock_guard<std::mutex> lock(m_pendingHttpMutex);
    m_httpStopping = true;
    pending.swap(m_pendingHttp);
  }
  for (const auto& state : pending) {
    std::lock_guard<std::mutex> lock(state->mutex);
    state->cancelled = true;
    state->cond.notify_all();
  }

  m_httpServer->stop();
  m_httpServer.reset();
}

void McpServer::removePendingHttpRequest(const std::shared_ptr<PendingHttpRequest>& pending)
{
  std::lock_guard<std::mutex> lock(m_pendingHttpMutex);
  m_pendingHttp.erase(std::remove(m_pendingHttp.begin(), m_pendingHttp.end(), pending),
                      m_pendingHttp.end());
}

bool McpServer::isHttpRunning() const
{
  return m_httpServer != nullptr;
}

void McpServer::runHttpOnly(const int port)
{
  std::string error;
  if (!startHttp(port, error)) {
    std::cerr << "Cannot start MCP HTTP server on port " << port << ": " << error << '\n';
    std::exit(1);
  }

  // Block until the server is stopped (only a signal ends the process
  // in this mode; the default SIGINT/SIGTERM disposition applies).
  while (isHttpRunning())
    m_httpServer->wait();
}

ix::HttpResponsePtr McpServer::handleHttpRequest(const ix::HttpRequestPtr& request)
{
  // The ix connection thread has no exception handling around this
  // callback, so any escaping exception must become a JSON-RPC error
  // instead of terminating the process.
  try {
    // DNS-rebinding defense: only requests without Origin or from
    // loopback pages are accepted.
    const std::string origin = headerValue(*request, "Origin");
    if (!origin.empty() && !isLoopbackOrigin(origin))
      return httpErrorJson(403, "Forbidden", -32600, "Origin not allowed");

    // Single endpoint: POST /mcp.
    const std::size_t query = request->uri.find('?');
    if (request->uri.substr(0, query) != "/mcp")
      return httpErrorJson(404, "Not Found", -32600, "Not found");
    if (request->method != "POST") {
      return httpResponse(405, "Method Not Allowed",
                          protocol::dumpCompact(
                            protocol::makeError(json11::Json(), -32600, "Method not allowed")),
                          { { "Allow", "POST" } });
    }

    // The body must be exactly one JSON-RPC message.
    std::string parseError;
    const json11::Json body = json11::Json::parse(request->body, parseError);
    if (!parseError.empty())
      return httpErrorJson(400, "Bad Request", -32700, "Parse error");

    const json11::Json id = body["id"];
    const std::string method = body["method"].string_value();

    // A modern request carries the protocol version in the header
    // and/or in the body _meta: when both are present they must agree,
    // and the Mcp-* headers must match the body values.
    const std::string headerVersion = headerValue(*request, "MCP-Protocol-Version");
    const std::string metaVersion =
      body["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"].string_value();
    const bool modern = !headerVersion.empty();

    if (modern || !metaVersion.empty()) {
      const std::string mcpName = headerValue(*request, "Mcp-Name");
      const bool mismatch =
        headerVersion.empty() ||
        (!metaVersion.empty() && metaVersion != headerVersion) ||
        headerValue(*request, "Mcp-Method") != method ||
        (method == "tools/call" &&
         (mcpName.empty() ||
          decodeMcpHeader(mcpName) != body["params"]["name"].string_value()));
      if (mismatch)
        return httpErrorJson(400, "Bad Request", -32020, "HeaderMismatch", id);

      if (headerVersion != protocol::kModernVersion) {
        return httpErrorJson(400, "Bad Request", -32022, "Unsupported protocol version", id,
                             json11::Json::object {
                               { "supported", json11::Json::array { protocol::kModernVersion } },
                             });
      }
    }

    const json11::Json response = dispatchHttp(body, modern);
    if (response.is_null()) // Notification: no reply, just the status
      return httpResponse(202, "Accepted", std::string());

    // Unknown JSON-RPC methods are a 404 at the transport level.
    if (response["error"]["code"].int_value() == -32601)
      return httpResponse(404, "Not Found", protocol::dumpCompact(response));

    return httpResponse(200, "OK", protocol::dumpCompact(response));
  }
  catch (const std::exception& ex) {
    return httpErrorJson(500, "Internal Server Error", -32603, ex.what());
  }
  catch (...) {
    return httpErrorJson(500, "Internal Server Error", -32603, "Internal error");
  }
}

json11::Json McpServer::dispatchHttp(const json11::Json& request, const bool modern)
{
  App* app = App::instance();
  if (!app || !app->isGui()) {
    // No UI loop to marshal to (--mcp-http): run on this ix worker
    // thread; the mutex serializes concurrent requests.
    std::lock_guard<std::mutex> lock(m_httpDispatchMutex);
    return (modern ? protocol::handle(request, protocol::Era::Modern)
                   : handleMessage(request));
  }

  // GUI mode: queue the request on the main thread's event loop and
  // wait for its response.
  auto pending = std::make_shared<PendingHttpRequest>();
  {
    std::lock_guard<std::mutex> lock(m_pendingHttpMutex);
    if (m_httpStopping) // stopHttp() is already past the cancel sweep
      return protocol::makeError(request["id"], -32603, "Server stopping");
    m_pendingHttp.push_back(pending);
  }

  os::Event ev;
  ev.setType(os::Event::Callback);
  ev.setCallback([pending, request, modern]() {
    {
      std::lock_guard<std::mutex> lock(pending->mutex);
      if (pending->cancelled) // stopHttp() already answered this request
        return;
    }

    json11::Json response;
    try {
      response = (modern ? protocol::handle(request, protocol::Era::Modern)
                         : McpServer::instance()->handleMessage(request));
    }
    catch (const std::exception& ex) {
      response = protocol::makeError(request["id"], -32603, ex.what());
    }
    catch (...) {
      response = protocol::makeError(request["id"], -32603, "Internal error");
    }

    {
      std::lock_guard<std::mutex> lock(pending->mutex);
      pending->response = response;
      pending->done = true;
    }
    pending->cond.notify_one();
  });
  os::queue_event(ev);

  std::unique_lock<std::mutex> lock(pending->mutex);
  if (!pending->cond.wait_for(lock, std::chrono::seconds(kHttpTimeoutSecs),
                              [&pending]() { return pending->done || pending->cancelled; })) {
    lock.unlock();
    removePendingHttpRequest(pending);
    return protocol::makeError(request["id"], -32603, "Request timed out");
  }
  if (pending->cancelled) {
    lock.unlock();
    removePendingHttpRequest(pending);
    return protocol::makeError(request["id"], -32603, "Server stopping");
  }
  const json11::Json response = pending->response;
  lock.unlock();
  removePendingHttpRequest(pending);
  return response;
}

#endif // ENABLE_WEBSOCKET

json11::Json McpServer::handleMessage(const json11::Json& request)
{
  return protocol::handle(request, protocol::detectEra(request));
}

} // namespace mcp
} // namespace app
