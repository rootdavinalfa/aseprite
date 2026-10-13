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

json11::Json McpServer::handleMessage(const json11::Json& request)
{
  return protocol::handle(request, protocol::detectEra(request));
}

} // namespace mcp
} // namespace app
