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

#include <iostream>
#include <string>

namespace app {
namespace mcp {

McpServer* McpServer::instance()
{
  static McpServer server;
  return &server;
}

void McpServer::runStdio(script::Engine& engine, Context& context)
{
  ToolContext::instance().setEngine(&engine);
  ToolContext::instance().setContext(&context);

  // Safety net: stdout carries nothing but MCP messages, so any stray
  // std::cout write during the loop is redirected to stderr.
  std::streambuf* stdoutBuf = std::cout.rdbuf();
  std::cout.rdbuf(std::cerr.rdbuf());

  std::ostream out(stdoutBuf);
  std::string line;
  while (std::getline(std::cin, line)) {
    if (line.empty())
      continue;

    std::string parseError;
    const json11::Json request = json11::Json::parse(line, parseError);
    const json11::Json response =
      (parseError.empty() ? handleMessage(request)
                          : protocol::makeError(json11::Json(), -32700, "Parse error"));

    if (!response.is_null()) {
      out << protocol::dumpCompact(response) << '\n';
      out.flush();
    }
  }

  std::cout.rdbuf(stdoutBuf);
}

json11::Json McpServer::handleMessage(const json11::Json& request)
{
  return protocol::handle(request, protocol::detectEra(request));
}

} // namespace mcp
} // namespace app
