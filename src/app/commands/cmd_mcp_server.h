// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_COMMANDS_CMD_MCP_SERVER_H_INCLUDED
#define APP_COMMANDS_CMD_MCP_SERVER_H_INCLUDED
#pragma once

#include "app/commands/command.h"

namespace app {

class OpenMcpServerCommand : public Command {
public:
  OpenMcpServerCommand();

protected:
  bool onEnabled(Context* context) override;
  void onExecute(Context* context) override;
};

} // namespace app

#endif
