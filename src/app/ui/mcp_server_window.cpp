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

#include "app/ui/mcp_server_window.h"

#include "app/i18n/strings.h"
#include "app/mcp/mcp_server.h"
#include "app/pref/preferences.h"
#include "base/fs.h"
#include "clip/clip.h"
#include "fmt/format.h"
#include "ui/entry.h"
#include "ui/message.h"
#include "ui/textbox.h"

namespace app {

namespace {

// The MCP HTTP server is only available in builds with websocket
// support.
bool httpRunning()
{
#ifdef ENABLE_WEBSOCKET
  return mcp::McpServer::instance()->isHttpRunning();
#else
  return false;
#endif
}

bool startHttp(const int port, std::string& error)
{
#ifdef ENABLE_WEBSOCKET
  return mcp::McpServer::instance()->startHttp(port, error);
#else
  error = "this build doesn't support the MCP HTTP server "
          "(built without websocket support)";
  return false;
#endif
}

void stopHttp()
{
#ifdef ENABLE_WEBSOCKET
  mcp::McpServer::instance()->stopHttp();
#endif
}

// Windows paths contain backslashes which are not valid JSON escapes.
std::string escape_json_string(const std::string& s)
{
  std::string result;
  result.reserve(s.size());
  for (const char c : s) {
    if (c == '\\' || c == '"')
      result += '\\';
    result += c;
  }
  return result;
}

} // anonymous namespace

McpServerWindow::McpServerWindow() : m_copyTimer(1000, this)
{
  startStop()->Click.connect([this] { onStartStop(); });
  port()->Change.connect([this] { onPortChange(); });
  copyStdio()->Click.connect([this] { onCopy(copyStdio(), stdioSnippet()->text()); });
  copyHttp()->Click.connect([this] { onCopy(copyHttp(), httpSnippet()->text()); });
  m_copyTimer.Tick.connect([this] { onCopyTimerTick(); });
}

bool McpServerWindow::onProcessMessage(ui::Message* msg)
{
  switch (msg->type()) {
    case ui::kOpenMessage:
      port()->setTextf("%d", Preferences::instance().mcp.port());
      updateDetails();
      updateStatus();
      break;

    case ui::kCloseMessage:
      m_copyTimer.stop();
      if (m_copiedButton) {
        m_copiedButton->setText(Strings::mcp_server_mcp_copy());
        m_copiedButton = nullptr;
      }
      break;
  }
  return app::gen::McpServer::onProcessMessage(msg);
}

void McpServerWindow::onStartStop()
{
  auto& pref = Preferences::instance();

  if (httpRunning()) {
    stopHttp();
    pref.mcp.enabled(false);
    updateStatus();
    return;
  }

  const int newPort = port()->textInt();
  if (newPort < 1 || newPort > 65535) {
    // Same range check (and wording) as the --mcp-http option in
    // src/app/cli/app_options.cpp.
    pref.mcp.enabled(false);
    updateStatus("port must be 1-65535");
    return;
  }

  pref.mcp.port(newPort);

  std::string error;
  if (startHttp(newPort, error)) {
    pref.mcp.enabled(true);
    updateStatus();
  }
  else {
    pref.mcp.enabled(false);
    updateStatus(error);
  }
}

void McpServerWindow::onPortChange()
{
  const int newPort = port()->textInt();
  if (newPort >= 1 && newPort <= 65535)
    Preferences::instance().mcp.port(newPort);

  updateDetails();
  updateStatus();
}

void McpServerWindow::onCopy(ui::Button* button, const std::string& text)
{
  clip::set_text(text);

  if (m_copiedButton)
    m_copiedButton->setText(Strings::mcp_server_mcp_copy());

  m_copiedButton = button;
  button->setText(Strings::mcp_server_mcp_copied());
  m_copyTimer.stop();
  m_copyTimer.start();
}

void McpServerWindow::onCopyTimerTick()
{
  m_copyTimer.stop();
  if (m_copiedButton) {
    m_copiedButton->setText(Strings::mcp_server_mcp_copy());
    m_copiedButton = nullptr;
  }
}

void McpServerWindow::updateDetails()
{
  const std::string appPath = base::get_app_path();
  const std::string url = fmt::format("http://127.0.0.1:{}/mcp", currentPort());

  stdioCommand()->setText(fmt::format("\"{}\" --mcp", appPath));
  stdioSnippet()->setText(
    fmt::format("{{\"mcpServers\":{{\"aseprite\":{{\"command\":\"{}\",\"args\":[\"--mcp\"]}}}}}}",
                escape_json_string(appPath)));
  httpUrl()->setText(url);
  httpSnippet()->setText(
    fmt::format("{{\"mcpServers\":{{\"aseprite\":{{\"url\":\"{}\"}}}}}}", url));
}

void McpServerWindow::updateStatus(const std::string& error)
{
  const bool running = httpRunning();

  if (running) {
    status()->setText(Strings::mcp_server_mcp_status_running(
      fmt::format("http://127.0.0.1:{}/mcp", currentPort())));
    startStop()->setText(Strings::mcp_server_mcp_stop());
  }
  else {
    const std::string stopped = Strings::mcp_server_mcp_status_stopped();
    status()->setText(error.empty() ? stopped : fmt::format("{}: {}", stopped, error));
    startStop()->setText(Strings::mcp_server_mcp_start());
  }

  // The port of a running server can only be changed by restarting it.
  port()->setEnabled(!running);
}

int McpServerWindow::currentPort() const
{
  const int portValue = port()->textInt();
  return (portValue >= 1 && portValue <= 65535 ? portValue : Preferences::instance().mcp.port());
}

} // namespace app
