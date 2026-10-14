// Aseprite
// Copyright (C) 2026  Igara Studio S.A.
//
// This program is distributed under the terms of
// the End-User License Agreement for Aseprite.

#ifndef APP_UI_MCP_SERVER_WINDOW_H_INCLUDED
#define APP_UI_MCP_SERVER_WINDOW_H_INCLUDED
#pragma once

#include "mcp_server.xml.h"
#include "ui/timer.h"

namespace app {

// Window to enable/disable the MCP HTTP server and to show how to
// connect an AI client (via stdio or HTTP).
class McpServerWindow : public app::gen::McpServer {
public:
  McpServerWindow();

protected:
  bool onProcessMessage(ui::Message* msg) override;

private:
  void onStartStop();
  void onPortChange();
  void onCopy(ui::Button* button, const std::string& text);
  void onCopyTimerTick();
  void updateDetails();
  void updateStatus(const std::string& error = std::string());
  int currentPort() const;

  ui::Timer m_copyTimer;
  ui::Button* m_copiedButton = nullptr;
};

} // namespace app

#endif
