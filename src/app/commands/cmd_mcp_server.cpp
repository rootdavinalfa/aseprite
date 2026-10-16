// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifdef HAVE_CONFIG_H
  #include "config.h"
#endif

#ifndef ENABLE_SCRIPTING
  #error ENABLE_SCRIPTING must be defined
#endif

#include "app/commands/cmd_mcp_server.h"

#include "app/context.h"
#include "app/ui/mcp_server_window.h"

namespace app {

static McpServerWindow* g_window = nullptr;

OpenMcpServerCommand::OpenMcpServerCommand() : Command(CommandId::OpenMcpServer())
{
}

bool OpenMcpServerCommand::onEnabled(Context* context)
{
  return context->isUIAvailable();
}

void OpenMcpServerCommand::onExecute(Context* context)
{
  if (!g_window)
    g_window = new McpServerWindow();

  if (g_window->isVisible())
    g_window->closeWindow(nullptr);
  else
    g_window->openWindow();
}

Command* CommandFactory::createOpenMcpServerCommand()
{
  return new OpenMcpServerCommand;
}

} // namespace app
